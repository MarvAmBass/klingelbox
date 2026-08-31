/*
 * wifi_mgr.c - see wifi_mgr.h.
 *
 * OWNERSHIP RULE (the single source of truth for STA retries): db_wifi_task owns
 * EVERY esp_wifi_connect() — the boot slot iteration, the runtime LAN-loss
 * retries, and the recovery background pass. on_wifi_event only sets and clears
 * event-group bits; it never blind-retries. That split is what keeps the state
 * machine understandable: exactly one task decides what the STA is doing, and
 * the event handler is pure notification.
 *
 * The other rule worth repeating here, because it shapes both loops below:
 * recovery is entered at BOOT ONLY. operational_loop() has no path into
 * DB_WIFI_RECOVERY at all — a lost LAN is retried forever with the softAP left
 * untouched, so the doorbell keeps working and keeps serving its UI.
 */
#include "wifi_mgr.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "dhcpserver/dhcpserver.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"

static const char *TAG = "db_wifi";

/* ---- timings ---- */
#define DB_STA_TRY_MS          15000   /* per-slot association + DHCP budget    */
#define DB_STA_ROUNDS_BOOT     2       /* passes over sta[] before falling back */
#define DB_STA_ROUND_PAUSE_MS  5000
#define DB_RECOVERY_RETRY_MS   180000  /* background STA retry period in recovery */
#define DB_OPER_RETRY_PAUSE_MS 60000   /* pause between passes after a LAN loss */

#define DB_OPER_LAST_SLOT_TRIES 3      /* retries of the last-good slot on loss */
#define DB_SCAN_MAX_APS         32     /* scan-result cap for the recovery gate */
#define DB_SCAN_RETRIES         3      /* db_wifi_scan attempts when the radio
                                          is momentarily busy connecting        */

static esp_netif_t       *s_ap_netif;
static esp_netif_t       *s_sta_netif;
static EventGroupHandle_t s_events;
static const db_config_t *s_cfg;       /* app_main's long-lived instance */
static char               s_ap_ssid[33];   /* SSID currently beaconed */

#define STA_CONNECTED_BIT BIT0   /* IP_EVENT_STA_GOT_IP seen, still associated */
#define STA_FAIL_BIT      BIT1   /* WIFI_EVENT_STA_DISCONNECTED seen           */
#define RETRY_KICK_BIT    BIT2   /* db_wifi_retry_sta()                        */
#define STA_STARTED_BIT   BIT3   /* WIFI_EVENT_STA_START seen                  */
#define STA_ASSOC_BIT     BIT4   /* WIFI_EVENT_STA_CONNECTED seen (associated) */

static volatile db_wifi_mode_t s_mode = DB_WIFI_CONNECTING;
static volatile int            s_active_slot = -1;

/* Operational AP intentionally down because cfg->ap_enabled == false. SAFETY:
 * every no-STA path (enter_recovery, operational_loop's STA-down branch) clears
 * this and brings the AP back up, so the flag can never make the box
 * unreachable. Unlike the reference firmware, nothing sets it automatically when
 * the STA connects — see the wifi_mgr.h header. */
static volatile bool s_ap_suppressed = false;

/* Effective softAP IP (network byte order; 0 until configure_ap_netif ran).
 * Usually cfg->ap_ip, but hops to a fallback /24 when the home LAN collides. */
static uint32_t s_ap_ip_eff;

/* Fallback AP /24s, tried in order, when the configured AP subnet collides with
 * the home LAN. The STA occupies exactly one /24, so with three distinct
 * candidates at least two are always free. */
static const char *const k_ap_ip_candidates[] = {
    DB_DEFAULT_AP_IP, "192.168.67.1", "192.168.4.1",
};

/* /24 network mask in network byte order (the AP netmask is fixed 255.255.255.0). */
static inline uint32_t ap_slash24_mask(void)
{
    return esp_netif_htonl(0xFFFFFF00UL);
}

/* Return `want` unless its /24 equals the STA's; then the first candidate on a
 * different /24 (one always exists). Network byte order. */
static uint32_t ap_ip_avoiding(uint32_t want, uint32_t sta_ip)
{
    const uint32_t mask = ap_slash24_mask();
    if ((want & mask) != (sta_ip & mask))
        return want;
    for (size_t i = 0;
         i < sizeof(k_ap_ip_candidates) / sizeof(k_ap_ip_candidates[0]); i++) {
        uint32_t cand = ipaddr_addr(k_ap_ip_candidates[i]);
        if ((cand & mask) != (sta_ip & mask))
            return cand;
    }
    return want;   /* unreachable: three distinct /24s vs one STA /24 */
}

static void ap_avoid_sta_subnet(uint32_t sta_ip);

/* Single-radio APSTA: the softAP can only beacon on the STA's channel. If the AP
 * config disagrees with the channel the STA actually associated on, the AP fails
 * to beacon and becomes undiscoverable. Re-sync the AP config to the real
 * operating channel whenever the STA (re)connects. */
static void ap_follow_sta_channel(void)
{
    uint8_t primary = 0;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &second) != ESP_OK || primary == 0)
        return;
    wifi_config_t apc;
    if (esp_wifi_get_config(WIFI_IF_AP, &apc) != ESP_OK)
        return;
    if (apc.ap.channel == primary)
        return;                              /* already in sync */
    apc.ap.channel = primary;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &apc);
    if (err == ESP_OK)
        ESP_LOGI(TAG, "softAP channel re-synced to STA channel %u", primary);
    else
        ESP_LOGE(TAG, "softAP channel re-sync to %u failed: %s",
                 primary, esp_err_to_name(err));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* No esp_wifi_connect() here — db_wifi_task owns all retries. */
            xEventGroupSetBits(s_events, STA_STARTED_BIT);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            xEventGroupSetBits(s_events, STA_ASSOC_BIT);
            ap_follow_sta_channel();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *e = data;
            xEventGroupClearBits(s_events, STA_CONNECTED_BIT | STA_ASSOC_BIT);
            xEventGroupSetBits(s_events, STA_FAIL_BIT);
            ESP_LOGW(TAG, "STA disconnected (reason %d) — connect task will retry",
                     e ? e->reason : -1);
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = data;
            ESP_LOGI(TAG, "AP client joined " MACSTR, MAC2STR(e->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = data;
            ESP_LOGI(TAG, "AP client left " MACSTR, MAC2STR(e->mac));
            break;
        }
        default: break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "STA got IP " IPSTR, IP2STR(&e->ip_info.ip));
        ap_avoid_sta_subnet(e->ip_info.ip.addr);   /* AP/LAN /24 collision guard */
        xEventGroupSetBits(s_events, STA_CONNECTED_BIT);
    }
}

/* Apply ap_addr (network byte order) as the AP netif's static IP and re-arm the
 * DHCP server. Core of configure_ap_netif; also called directly by
 * ap_avoid_sta_subnet when hopping to a fallback subnet. */
static void configure_ap_netif_ip(uint32_t ap_addr)
{
    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = ap_addr;
    ip.gw.addr = ip.ip.addr;
    ip.netmask.addr = ipaddr_addr("255.255.255.0");

    /* Tolerant of "already stopped/started": besides boot this also runs when the
     * softAP is re-enabled after db_wifi_stop_ap() (OTA), where the dhcps state
     * depends on how far the AP restart has progressed. */
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
        ESP_LOGW(TAG, "AP dhcps stop: %s", esp_err_to_name(err));
    err = esp_netif_set_ip_info(s_ap_netif, &ip);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "AP set_ip_info: %s", esp_err_to_name(err));

    /* THE CAPTIVE-PORTAL LINCHPIN: hand OURSELVES out as the DNS server (DHCP
     * option 6) so every AP client resolves through dns_server.c. Skip this and
     * a joining phone keeps its cellular resolver, its connectivity check
     * succeeds against the real internet, and the portal sheet never opens. */
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    err = esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "AP set_dns_info: %s", esp_err_to_name(err));
    dhcps_offer_t offer = OFFER_DNS;   /* required, or option 6 is never sent */
    err = esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &offer, sizeof(offer));
    if (err != ESP_OK)
        ESP_LOGW(TAG, "AP dhcps DNS option: %s", esp_err_to_name(err));
    err = esp_netif_dhcps_start(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
        ESP_LOGW(TAG, "AP dhcps start: %s", esp_err_to_name(err));

    s_ap_ip_eff = ap_addr;
}

/* Configure the AP netif from cfg->ap_ip, avoiding the home-LAN /24 when the STA
 * is already connected (a softAP restart after an OTA — at first boot the STA has
 * no IP yet and ap_avoid_sta_subnet handles the GOT_IP case). */
static void configure_ap_netif(const db_config_t *cfg)
{
    uint32_t want = ipaddr_addr(cfg->ap_ip[0] ? cfg->ap_ip : DB_DEFAULT_AP_IP);
    esp_netif_ip_info_t sta = {0};
    if (db_wifi_sta_connected() && s_sta_netif &&
        esp_netif_get_ip_info(s_sta_netif, &sta) == ESP_OK && sta.ip.addr != 0) {
        uint32_t use = ap_ip_avoiding(want, sta.ip.addr);
        if (use != want) {
            esp_ip4_addr_t w = { .addr = want }, u = { .addr = use };
            char ws[16], us[16];
            ESP_LOGW(TAG, "configured AP subnet %s/24 collides with the home "
                          "LAN — softAP stays on fallback %s/24",
                     esp_ip4addr_ntoa(&w, ws, sizeof(ws)),
                     esp_ip4addr_ntoa(&u, us, sizeof(us)));
            want = use;
        }
    }
    configure_ap_netif_ip(want);
}

/* Home-LAN /24 vs softAP /24 collision guard: if the subnet the STA just got an
 * IP on equals the softAP subnet, routing on AP clients breaks silently. Hop the
 * AP to the first free fallback /24 and re-arm its DHCP server. A no-op unless a
 * real collision is detected. */
static void ap_avoid_sta_subnet(uint32_t sta_ip)
{
    const uint32_t mask = ap_slash24_mask();
    uint32_t cur = s_ap_ip_eff;
    if (cur == 0 || (cur & mask) != (sta_ip & mask))
        return;                              /* AP not up yet, or no collision */

    uint32_t next = ap_ip_avoiding(cur, sta_ip);
    esp_ip4_addr_t c = { .addr = cur }, n = { .addr = next }, s = { .addr = sta_ip };
    char cs[16], ns[16], ss[16];
    ESP_LOGW(TAG, "softAP subnet %s/24 COLLIDES with home LAN %s/24 — hopping "
                  "softAP to %s/24 (AP clients must renew their DHCP lease)",
             esp_ip4addr_ntoa(&c, cs, sizeof(cs)),
             esp_ip4addr_ntoa(&s, ss, sizeof(ss)),
             esp_ip4addr_ntoa(&n, ns, sizeof(ns)));
    configure_ap_netif_ip(next);
}

/* Operational softAP personality (cfg->ap_*). */
static void build_ap_wifi_config(const db_config_t *cfg, wifi_config_t *out)
{
    memset(out, 0, sizeof(*out));
    strlcpy((char *)out->ap.ssid, cfg->ap_ssid[0] ? cfg->ap_ssid : "Klingelbox",
            sizeof(out->ap.ssid));
    out->ap.ssid_len = strlen((char *)out->ap.ssid);
    /* Single-radio APSTA: the STA channel always wins, so a configured AP channel
     * that differs from the STA's makes the AP fail to beacon. When a home Wi-Fi
     * STA is configured, start on the current STA channel if known (else a
     * provisional channel 1) and let ap_follow_sta_channel() re-sync on
     * WIFI_EVENT_STA_CONNECTED. Only honour cfg->ap_channel in AP-only mode. */
    if (db_config_sta_count(cfg) > 0) {
        uint8_t primary = 0;
        wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
        if (esp_wifi_get_channel(&primary, &second) == ESP_OK && primary != 0)
            out->ap.channel = primary;
        else
            out->ap.channel = 1;   /* provisional; follows the STA on connect */
    } else {
        out->ap.channel = cfg->ap_channel ? cfg->ap_channel : 6;
    }
    out->ap.ssid_hidden = 0;       /* must be discoverable in scans */
    out->ap.max_connection = 8;
    if (cfg->ap_security == 0) {
        out->ap.authmode = WIFI_AUTH_OPEN;   /* explicitly configured open */
    } else if (strlen(cfg->ap_pass) < 8) {
        /* A <8-char key is not a valid WPA2-PSK: the driver would reject it, so
         * open is the only safe fallback — but say so LOUDLY. A silent downgrade
         * to an open AP is exactly the kind of thing nobody notices. */
        out->ap.authmode = WIFI_AUTH_OPEN;
        ESP_LOGE(TAG, "AP password too short for WPA2 (%u chars, need >= 8) — "
                      "softAP '%s' is running OPEN (unencrypted)! Set an 8+ "
                      "char ap_pass to secure it",
                 (unsigned)strlen(cfg->ap_pass), (char *)out->ap.ssid);
    } else {
        strlcpy((char *)out->ap.password, cfg->ap_pass, sizeof(out->ap.password));
        out->ap.authmode = WIFI_AUTH_WPA2_PSK;   /* WPA2 only — WPA3 forces PMF */
    }
    /* 802.11w/PMF OFF. Carried over from the reference, where advertising PMF
     * made the AP send SA-Query challenges that simple WPA2 clients never
     * answered, and the AP disassociated them every ~30 s. Plain WPA2 with PMF
     * off is the configuration that proved stable with the widest range of
     * clients (and phones join an open+PMF-off portal without complaint). */
    out->ap.pmf_cfg.capable = false;
    out->ap.pmf_cfg.required = false;
}

/* Recovery portal personality: "Klingelbox-XXXX" from the last two bytes of the
 * STA MAC (Tasmota-style, stable per device so a user can be told the exact name
 * to look for), fixed channel 1, open unless cfg->recovery_ap_pass is a usable
 * WPA2 key. */
static void build_recovery_ap_config(const db_config_t *cfg, wifi_config_t *out)
{
    memset(out, 0, sizeof(*out));
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf((char *)out->ap.ssid, sizeof(out->ap.ssid),
             "Klingelbox-%02X%02X", mac[4], mac[5]);
    out->ap.ssid_len = strlen((char *)out->ap.ssid);
    out->ap.channel = 1;           /* fixed: there is no STA to follow */
    out->ap.ssid_hidden = 0;
    out->ap.max_connection = 4;
    if (strlen(cfg->recovery_ap_pass) >= 8) {
        strlcpy((char *)out->ap.password, cfg->recovery_ap_pass,
                sizeof(out->ap.password));
        out->ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        if (cfg->recovery_ap_pass[0])
            /* A key was configured but is unusable — warn rather than downgrade
             * silently (an empty pass is the intentional open-portal default). */
            ESP_LOGE(TAG, "recovery AP password too short for WPA2 (%u chars, "
                          "need >= 8) — recovery AP '%s' is running OPEN "
                          "(unencrypted)!",
                     (unsigned)strlen(cfg->recovery_ap_pass),
                     (char *)out->ap.ssid);
        out->ap.authmode = WIFI_AUTH_OPEN;   /* default: open portal */
    }
    out->ap.pmf_cfg.capable = false;
    out->ap.pmf_cfg.required = false;
}

/* Remember what we are beaconing, for db_wifi_ap_ssid() / the portal UI. */
static void note_ap_ssid(const wifi_config_t *apc)
{
    strlcpy(s_ap_ssid, (const char *)apc->ap.ssid, sizeof(s_ap_ssid));
}

/* ---------------------------------------------------------------- connect task */

/* Wait up to ms for a retry kick; true if kicked (bit consumed). */
static bool wait_kick_or_delay(uint32_t ms)
{
    EventBits_t b = xEventGroupWaitBits(s_events, RETRY_KICK_BIT,
                                        pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
    return (b & RETRY_KICK_BIT) != 0;
}

static int ap_station_count(void)
{
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK)
        return 0;
    return list.num;
}

/* One connect attempt on sta[slot]: set the STA config, connect, wait up to
 * DB_STA_TRY_MS for an IP. A DISCONNECTED event aborts the wait early so a wrong
 * passphrase costs a second, not the full budget. True once the STA has an IP. */
static bool try_slot(int slot)
{
    const db_sta_net_t *net = &s_cfg->sta[slot];
    if (!net->ssid[0])
        return false;

    ESP_LOGI(TAG, "STA slot %d: trying '%s'", slot, net->ssid);
    esp_wifi_disconnect();                       /* idempotent; flush stale state */
    vTaskDelay(pdMS_TO_TICKS(100));              /* let a stale DISCONNECTED land */
    xEventGroupClearBits(s_events, STA_CONNECTED_BIT | STA_FAIL_BIT);

    wifi_config_t stac = {0};
    strlcpy((char *)stac.sta.ssid, net->ssid, sizeof(stac.sta.ssid));
    strlcpy((char *)stac.sta.password, net->pass, sizeof(stac.sta.password));
    stac.sta.threshold.authmode = net->pass[0] ? WIFI_AUTH_WPA2_PSK
                                               : WIFI_AUTH_OPEN;
    if (esp_wifi_set_config(WIFI_IF_STA, &stac) != ESP_OK)
        return false;
    if (esp_wifi_connect() != ESP_OK)
        return false;

    TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(DB_STA_TRY_MS)) {
        EventBits_t b = xEventGroupWaitBits(s_events,
                                            STA_CONNECTED_BIT | STA_FAIL_BIT,
                                            pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & STA_CONNECTED_BIT)
            return true;
        if (b & STA_FAIL_BIT) {                  /* early abort on disconnect */
            xEventGroupClearBits(s_events, STA_FAIL_BIT);
            ESP_LOGI(TAG, "STA slot %d: '%s' failed", slot, net->ssid);
            return false;
        }
    }
    ESP_LOGI(TAG, "STA slot %d: '%s' timed out", slot, net->ssid);
    esp_wifi_disconnect();                       /* give up on this slot */
    return false;
}

/* One pass over sta[0..DB_STA_MAX-1], skipping empties. Connected slot or -1. */
static int try_all_slots(void)
{
    for (int i = 0; i < DB_STA_MAX; i++) {
        if (!s_cfg->sta[i].ssid[0])
            continue;
        if (try_slot(i))
            return i;
    }
    return -1;
}

/* Recovery pass: one scan, then attempt only the slots whose SSID is actually in
 * the air — otherwise a household with one configured-but-absent network burns
 * DB_STA_TRY_MS per slot per cycle for nothing. */
static int try_visible_slots(void)
{
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* blocking scan */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "recovery scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0)
        return -1;
    if (num > DB_SCAN_MAX_APS)
        num = DB_SCAN_MAX_APS;
    wifi_ap_record_t *recs = malloc(num * sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return -1;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) != ESP_OK) {
        free(recs);
        return -1;
    }

    int got = -1;
    for (int i = 0; i < DB_STA_MAX && got < 0; i++) {
        if (!s_cfg->sta[i].ssid[0])
            continue;
        bool visible = false;
        for (int j = 0; j < num; j++) {
            if (strcmp((const char *)recs[j].ssid, s_cfg->sta[i].ssid) == 0) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            ESP_LOGI(TAG, "recovery: slot %d '%s' not visible — skipped",
                     i, s_cfg->sta[i].ssid);
            continue;
        }
        if (try_slot(i))
            got = i;
    }
    free(recs);
    return got;
}

static void set_operational_connected(int slot)
{
    s_mode = DB_WIFI_OPERATIONAL;
    s_active_slot = slot;
    ESP_LOGI(TAG, "OPERATIONAL: home Wi-Fi up via slot %d ('%s')",
             slot, s_cfg->sta[slot].ssid);
    /* NOT ported from the reference: it stopped its softAP here when the AP was
     * configured off, because that AP served appliances rather than people. This
     * box keeps its UI reachable on both the AP and the LAN, so the AP stays up
     * exactly as it was. */
}

/* Swap the softAP to the recovery personality. Boot-only entry point: on a single
 * radio this displaces the operational AP, which is acceptable precisely because
 * we only ever get here when there is no home network to serve anyway. */
static void enter_recovery(void)
{
    s_mode = DB_WIFI_RECOVERY;
    s_active_slot = -1;

    /* SAFETY: the recovery portal is exempt from cfg->ap_enabled. If the boot ran
     * STA-only, the AP interface is down entirely and needs the full APSTA
     * bring-up, not just a personality swap. */
    if (s_ap_suppressed) {
        esp_err_t rerr = db_wifi_restart_ap(s_cfg);   /* clears s_ap_suppressed */
        if (rerr != ESP_OK)
            ESP_LOGE(TAG, "recovery AP bring-up failed: %s", esp_err_to_name(rerr));
        ESP_LOGW(TAG, "RECOVERY: config AP raised despite ap_enabled=false "
                      "(recovery is exempt — the device must stay reachable)");
        return;
    }

    wifi_config_t apc;
    build_recovery_ap_config(s_cfg, &apc);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &apc);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "recovery AP config failed: %s", esp_err_to_name(err));
    else
        note_ap_ssid(&apc);
    ESP_LOGW(TAG, "RECOVERY: config portal '%s' (%s, ch 1) — join it and the "
                  "Wi-Fi wizard opens automatically",
             (char *)apc.ap.ssid,
             apc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
}

/* RECOVERY: background retry every DB_RECOVERY_RETRY_MS, gated on "no station
 * associated" so a periodic pass never yanks the portal out from under someone
 * filling in the wizard. An explicit kick (the wizard just saved credentials)
 * runs the pass immediately, stations or not. Never returns — success reboots
 * into a clean OPERATIONAL bring-up rather than trying to mutate this one. */
static void recovery_loop(void)
{
    for (;;) {
        bool kicked = wait_kick_or_delay(DB_RECOVERY_RETRY_MS);
        if (db_config_sta_count(s_cfg) == 0)
            continue;                       /* nothing configured to try yet */
        if (!kicked && ap_station_count() > 0)
            continue;                       /* periodic pass: portal in use */

        int slot = try_visible_slots();
        if (slot >= 0) {
            s_active_slot = slot;
            ESP_LOGW(TAG, "recovery: connected via slot %d ('%s') — rebooting "
                          "to operational in 3 s", slot, s_cfg->sta[slot].ssid);
            vTaskDelay(pdMS_TO_TICKS(3000));   /* let the portal show success */
            esp_restart();
        }
    }
}

/* OPERATIONAL runtime policy: the softAP stays up untouched — the doorbell does
 * not need the LAN to receive or replay RF. On STA loss, retry the last-good slot
 * DB_OPER_LAST_SLOT_TRIES times (a router reboot usually comes back on the same
 * network), then cycle all slots forever. This loop NEVER drops to recovery; see
 * the file header. Never returns. */
static void operational_loop(void)
{
    for (;;) {
        if (db_wifi_sta_connected()) {
            /* Connected: idle until a disconnect or a config-change kick. */
            EventBits_t b = xEventGroupWaitBits(s_events,
                                                STA_FAIL_BIT | RETRY_KICK_BIT,
                                                pdTRUE, pdFALSE,
                                                pdMS_TO_TICKS(1000));
            if (b & RETRY_KICK_BIT) {
                ESP_LOGI(TAG, "sta[] changed — restarting the connect cycle");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(300));
            }
            continue;
        }

        /* ---- STA down: LAN lost, boot-fail with the fallback off, or AP-only ---- */

        /* SAFETY: the STA is gone — if the operational AP was suppressed by
         * ap_enabled=false, bring it back up NOW so the box stays reachable. */
        if (s_ap_suppressed) {
            ESP_LOGW(TAG, "home Wi-Fi down with the softAP disabled — restoring "
                          "it so the appliance stays reachable");
            db_wifi_restart_ap(s_cfg);   /* clears s_ap_suppressed on success */
        }

        int last = s_active_slot;
        s_active_slot = -1;

        if (db_config_sta_count(s_cfg) == 0) {
            /* AP-only: nothing to try until the API adds a network. */
            wait_kick_or_delay(DB_OPER_RETRY_PAUSE_MS);
            continue;
        }

        ESP_LOGW(TAG, "home Wi-Fi down — softAP stays up, retrying the STA "
                      "in the background (staying OPERATIONAL by design)");
        int got = -1;
        if (last >= 0 && last < DB_STA_MAX && s_cfg->sta[last].ssid[0]) {
            for (int i = 0; i < DB_OPER_LAST_SLOT_TRIES && got < 0; i++) {
                if (try_slot(last))
                    got = last;
            }
        }
        while (got < 0) {
            if (db_config_sta_count(s_cfg) == 0)
                break;                      /* outer loop re-evaluates */
            got = try_all_slots();
            if (got >= 0)
                break;
            wait_kick_or_delay(DB_OPER_RETRY_PAUSE_MS);
        }
        if (got >= 0)
            set_operational_connected(got);
    }
}

static void db_wifi_task(void *arg)
{
    if (s_mode == DB_WIFI_CONNECTING) {
        /* Make sure the STA interface is actually up before the first connect. */
        xEventGroupWaitBits(s_events, STA_STARTED_BIT, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(5000));

        int got = -1;
        for (int round = 0; round < DB_STA_ROUNDS_BOOT && got < 0; round++) {
            got = try_all_slots();
            if (got < 0 && round + 1 < DB_STA_ROUNDS_BOOT)
                wait_kick_or_delay(DB_STA_ROUND_PAUSE_MS);
        }

        if (got >= 0) {
            set_operational_connected(got);
        } else if (s_cfg->ap_fallback_enabled) {
            ESP_LOGW(TAG, "all STA slots failed after %d round(s) — falling back "
                          "to the recovery portal", DB_STA_ROUNDS_BOOT);
            enter_recovery();
        } else {
            /* Fallback explicitly disabled: stay on the operational AP and keep
             * cycling the slots forever (operational_loop's STA-down path). */
            s_mode = DB_WIFI_OPERATIONAL;
            ESP_LOGW(TAG, "all STA slots failed; fallback disabled — staying "
                          "OPERATIONAL and cycling forever");
        }
    }

    if (s_mode == DB_WIFI_RECOVERY)
        recovery_loop();       /* never returns (esp_restart on success) */
    else
        operational_loop();    /* never returns */
}

/* ------------------------------------------------------------------ public API */

esp_err_t db_wifi_start(const db_config_t *cfg)
{
    s_cfg = cfg;
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* Hostname (DHCP option 12 on the STA; also what mDNS announces). Must be set
     * before esp_wifi_start()/connect so the DHCP DISCOVER already carries it —
     * otherwise the router lists the box as "espressif" until the next lease. */
    const char *host = cfg->hostname[0] ? cfg->hostname : "klingelbox";
    esp_err_t herr = esp_netif_set_hostname(s_sta_netif, host);
    if (herr != ESP_OK)
        ESP_LOGW(TAG, "STA hostname set failed: %s", esp_err_to_name(herr));
    herr = esp_netif_set_hostname(s_ap_netif, host);
    if (herr != ESP_OK)
        ESP_LOGW(TAG, "AP hostname set failed: %s", esp_err_to_name(herr));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    /* Initial mode: no STA slots -> straight to RECOVERY (or AP-only when the
     * fallback is disabled); otherwise CONNECTING with the softAP already up so
     * the UI is reachable during the connect attempts. */
    int nsta = db_config_sta_count(cfg);
    if (nsta == 0)
        s_mode = cfg->ap_fallback_enabled ? DB_WIFI_RECOVERY
                                          : DB_WIFI_OPERATIONAL;  /* AP-only */
    else
        s_mode = DB_WIFI_CONNECTING;

    /* cfg->ap_enabled == false: boot STA-only. SAFETY: only possible when STA
     * slots exist — a factory / no-Wi-Fi boot always raises an AP regardless, and
     * a failed connect re-raises it too (enter_recovery / operational_loop), so
     * the box is never unreachable. */
    bool ap_at_boot = cfg->ap_enabled || s_mode != DB_WIFI_CONNECTING;
    s_ap_suppressed = !ap_at_boot;

    wifi_config_t apc;
    memset(&apc, 0, sizeof(apc));
    if (ap_at_boot) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        if (s_mode == DB_WIFI_RECOVERY)
            build_recovery_ap_config(cfg, &apc);
        else
            build_ap_wifi_config(cfg, &apc);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apc));
        note_ap_ssid(&apc);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable STA power-save: WIFI_PS_MIN_MODEM (the default) sleeps the shared
     * radio between DTIM beacons, which starves the softAP of airtime in APSTA
     * mode and makes it unreliable to discover and join. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_LOGI(TAG, "Wi-Fi power-save disabled (WIFI_PS_NONE) for a reliable APSTA softAP");
    if (ap_at_boot)
        configure_ap_netif(cfg);   /* after start, so the AP netif exists */

    if (ap_at_boot) {
        char ap_ip[16];
        db_wifi_ap_ip(ap_ip);
        ESP_LOGI(TAG, "softAP '%s' up on %s (ch %d, %s)%s",
                 (char *)apc.ap.ssid, ap_ip, apc.ap.channel,
                 apc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2",
                 s_mode == DB_WIFI_RECOVERY ? " [recovery portal]" : "");
    } else {
        ESP_LOGW(TAG, "softAP disabled (ap_enabled=false) — booting STA-only; "
                      "the AP comes back automatically if home Wi-Fi fails");
    }

    if (s_mode == DB_WIFI_CONNECTING)
        ESP_LOGI(TAG, "connecting: %d STA slot(s), %d s budget each, %d round(s)",
                 nsta, DB_STA_TRY_MS / 1000, DB_STA_ROUNDS_BOOT);
    else if (s_mode == DB_WIFI_OPERATIONAL)
        ESP_LOGW(TAG, "no home Wi-Fi configured and the fallback is disabled — "
                      "AP-only mode");

    /* The connect task runs in every mode: it owns retries, mode transitions and
     * the recovery background pass (idle until kicked when there is nothing to do). */
    xTaskCreate(db_wifi_task, "db_wifi", 4096, NULL, 5, NULL);
    return ESP_OK;
}

db_wifi_mode_t db_wifi_mode(void)
{
    return s_mode;
}

const char *db_wifi_mode_str(void)
{
    switch (s_mode) {
    case DB_WIFI_RECOVERY:    return "recovery";
    case DB_WIFI_OPERATIONAL: return "normal";
    default:                  return "connecting";
    }
}

void db_wifi_retry_sta(void)
{
    if (s_events)
        xEventGroupSetBits(s_events, RETRY_KICK_BIT);
}

int db_wifi_active_sta_slot(void)
{
    return s_active_slot;
}

bool db_wifi_sta_connected(void)
{
    return s_events &&
           (xEventGroupGetBits(s_events) & STA_CONNECTED_BIT) != 0;
}

esp_err_t db_wifi_scan(db_wifi_ap_t *out, size_t max_aps, size_t *found_out)
{
    if (found_out) *found_out = 0;
    if (!out || max_aps == 0)
        return ESP_ERR_INVALID_ARG;

    /* The connect task may be mid-attempt, in which case the driver rejects a
     * scan with ESP_ERR_WIFI_STATE. A couple of short retries covers that window
     * without making the wizard's "scan" button feel broken. */
    esp_err_t err = ESP_FAIL;
    wifi_scan_config_t sc = { .show_hidden = false };
    for (int i = 0; i < DB_SCAN_RETRIES; i++) {
        err = esp_wifi_scan_start(&sc, true);   /* blocking */
        if (err == ESP_OK)
            break;
        ESP_LOGW(TAG, "scan attempt %d failed: %s", i + 1, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    if (err != ESP_OK)
        return err;

    uint16_t num = DB_SCAN_MAX_APS;
    wifi_ap_record_t *recs = calloc(num, sizeof(*recs));
    if (!recs) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&num, recs);
    if (err != ESP_OK) {
        free(recs);
        return err;
    }

    size_t n = 0;
    for (int i = 0; i < num && n < max_aps; i++) {
        const char *ssid = (const char *)recs[i].ssid;
        if (!ssid[0])
            continue;                 /* hidden network: nothing to show or pick */
        memset(&out[n], 0, sizeof(out[n]));
        strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
        out[n].rssi     = recs[i].rssi;
        out[n].authmode = (uint8_t)recs[i].authmode;
        out[n].channel  = recs[i].primary;
        n++;
    }
    free(recs);
    if (found_out) *found_out = n;
    ESP_LOGI(TAG, "scan: %u network(s) visible, %u reported",
             (unsigned)num, (unsigned)n);
    return ESP_OK;
}

esp_err_t db_wifi_stop_ap(void)
{
    ESP_LOGW(TAG, "stopping the softAP (radio claimed for an update) — "
                  "AP clients drop until it is restarted");
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "stop softAP: set STA mode failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t db_wifi_restart_ap(const db_config_t *cfg)
{
    /* AP disabled by config + STA connected: honour it and stay down. Recovery
     * and every no-STA state are exempt — reachability always wins. */
    if (!cfg->ap_enabled && s_mode != DB_WIFI_RECOVERY && db_wifi_sta_connected()) {
        ESP_LOGW(TAG, "softAP restart skipped: disabled by config "
                      "(ap_enabled=false) — staying STA-only");
        s_ap_suppressed = true;
        esp_wifi_set_mode(WIFI_MODE_STA);   /* idempotent: make sure it IS down */
        return ESP_OK;
    }

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "softAP restart: set APSTA failed: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(300));      /* let WIFI_EVENT_AP_START raise the netif */

    /* Re-apply the personality that db_wifi_stop_ap() displaced. */
    wifi_config_t apc;
    if (s_mode == DB_WIFI_RECOVERY)
        build_recovery_ap_config(cfg, &apc);
    else
        build_ap_wifi_config(cfg, &apc);
    err = esp_wifi_set_config(WIFI_IF_AP, &apc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "softAP restart: AP config failed: %s", esp_err_to_name(err));
        return err;
    }
    note_ap_ssid(&apc);
    ap_follow_sta_channel();             /* beacon on the live STA channel, if any */
    configure_ap_netif(cfg);             /* re-arm the DHCP server + DNS handout */
    s_ap_suppressed = false;             /* the AP is (back) up */

    char ap_ip[16];
    db_wifi_ap_ip(ap_ip);
    ESP_LOGI(TAG, "softAP '%s' restored on %s (ch %d, %s)", (char *)apc.ap.ssid,
             ap_ip, apc.ap.channel,
             apc.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    return ESP_OK;
}

esp_err_t db_wifi_apply_ap_enabled(const db_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;

    if (cfg->ap_enabled) {
        if (!s_ap_suppressed)
            return ESP_OK;   /* already up — or temporarily stopped by an OTA,
                                whose own restore path brings it back */
        ESP_LOGW(TAG, "softAP re-enabled — bringing it back up");
        return db_wifi_restart_ap(cfg);   /* clears s_ap_suppressed on success */
    }

    /* Disable requested. SAFETY: only honour it while the STA is connected — with
     * no STA the AP is the only way into the device, so it stays up and the
     * stored flag applies at the next boot instead. */
    if (s_mode == DB_WIFI_RECOVERY || !db_wifi_sta_connected()) {
        ESP_LOGW(TAG, "ap_enabled=false stored, but there is no home Wi-Fi "
                      "connection — keeping the AP up (safety); the flag takes "
                      "effect at the next boot");
        return ESP_OK;
    }
    if (s_ap_suppressed)
        return ESP_OK;                    /* already down */
    ESP_LOGW(TAG, "softAP disabled via the API — stopping it; the UI stays "
                  "reachable over the LAN");
    esp_err_t err = db_wifi_stop_ap();
    if (err == ESP_OK)
        s_ap_suppressed = true;
    return err;
}

static void netif_ip(esp_netif_t *nif, char out[16])
{
    esp_netif_ip_info_t ip = {0};
    out[0] = '\0';
    if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr)
        esp_ip4addr_ntoa(&ip.ip, out, 16);
}

void db_wifi_sta_ip(char out[16]) { netif_ip(s_sta_netif, out); }
void db_wifi_ap_ip(char out[16])  { netif_ip(s_ap_netif, out); }

void db_wifi_ap_ssid(char out[33]) { strlcpy(out, s_ap_ssid, 33); }

esp_netif_t *db_wifi_ap_netif(void) { return s_ap_netif; }
