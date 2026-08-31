/*
 * wifi_mgr.h - Wi-Fi bring-up + connect state machine for the Klingelbox box.
 *
 * APSTA on a single radio, with two softAP personalities:
 *
 *   - OPERATIONAL AP (cfg->ap_ssid, default "Klingelbox"): the everyday hotspot.
 *     It stays up while the STA connects, while it is connected, and while it is
 *     being retried. Its channel must follow the STA's (one radio, one channel).
 *   - RECOVERY portal ("Klingelbox-XXXX" from the MAC, Tasmota-style): raised at
 *     BOOT when no STA slot is configured (factory state) or every boot connect
 *     pass failed, and cfg->ap_fallback_enabled is set. Open by default, WPA2 if
 *     cfg->recovery_ap_pass is a usable key. The captive DNS (dns_server.c) makes
 *     joining it pop the Wi-Fi wizard on a phone.
 *
 * RECOVERY IS A BOOT-TIME DECISION, FULL STOP. This is the single most important
 * rule in this file and it was learned the hard way in the reference firmware: a
 * box that drops into a config portal because the house router rebooted is a box
 * that has stopped being an appliance. Once running, losing the home Wi-Fi
 * changes NOTHING about the mode — db_wifi_task just keeps retrying the STA
 * slots in the background, forever, while the doorbell keeps receiving and
 * replaying RF and the softAP keeps serving the UI. The only ways into recovery
 * are a reboot with no usable network, and the only way out of recovery is a
 * successful background connect, which reboots into a clean operational bring-up.
 *
 * THE AP'S DHCP SERVER HANDS OUT *ITSELF* AS THE DNS SERVER (option 6). That one
 * line is what makes the captive portal work at all: without it a joining phone
 * keeps its cellular resolver, our dns_server.c never sees a query, the OS
 * connectivity check succeeds against the real internet, and no portal sheet ever
 * appears. See configure_ap_netif_ip().
 *
 * DIFFERENCE FROM THE REFERENCE (deliberate): the reference gated its web UI on
 * the softAP — while its STA was connected, requests arriving over the AP were
 * 403'd and the AP could even be taken down automatically, because that AP
 * existed to serve appliances rather than people. This box has no such split:
 * the doorbell UI is meant to be reachable on BOTH the softAP and the LAN at all
 * times, so there is no db_wifi_ui_allowed_on_ap() here and the AP is never
 * pulled down just because the STA came up.
 *
 * Subnet-conflict avoidance is kept: if the home LAN turns out to share the AP's
 * /24, the AP hops to the first free fallback /24 with a loud warning, and
 * db_wifi_ap_ip() always reports the EFFECTIVE address so the UI, the DNS server
 * and the captive portal follow automatically.
 */
#ifndef DB_WIFI_MGR_H
#define DB_WIFI_MGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "db_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DB_WIFI_CONNECTING,    /* boot: iterating sta[]                            */
    DB_WIFI_OPERATIONAL,   /* normal service; STA connected or being retried   */
    DB_WIFI_RECOVERY,      /* recovery config AP + captive portal + wizard     */
} db_wifi_mode_t;

/* Bring up netif + the default event loop + Wi-Fi in APSTA, then spawn the
 * connect task. Returns as soon as the AP is up; the STA connects
 * asynchronously. `cfg` must stay valid forever (app_main's long-lived
 * instance) — the task reads it live. Call once at boot. */
esp_err_t db_wifi_start(const db_config_t *cfg);

/* Current state-machine mode. */
db_wifi_mode_t db_wifi_mode(void);

/* Stable lowercase name for GET /api/system: "connecting" | "normal" |
 * "recovery". The web UI swaps the dashboard for the Wi-Fi wizard on
 * "recovery", so this string is an API contract — do not reword it. */
const char *db_wifi_mode_str(void);

/* Apply new home-Wi-Fi credentials LIVE, with no reboot: the caller writes
 * cfg->sta[] and saves the config, then calls this to kick the connect task
 * into restarting its slot cycle immediately. In recovery mode this also
 * bypasses the "portal is in use" gate, so a wizard save is tried at once. */
void db_wifi_retry_sta(void);

/* Slot index (0..DB_STA_MAX-1) of the connected STA network; -1 if none. */
int db_wifi_active_sta_slot(void);

/* True once the STA has an IP (home Wi-Fi joined). */
bool db_wifi_sta_connected(void);

/* Copy the current STA / AP IPv4 (dotted) into out (>= 16 bytes). "" if not up.
 * db_wifi_ap_ip() reports the EFFECTIVE AP address, which may differ from
 * cfg->ap_ip after a subnet-collision hop. */
void db_wifi_sta_ip(char out[16]);
void db_wifi_ap_ip(char out[16]);

/* The SSID the softAP is currently beaconing — the operational one, or the
 * "Klingelbox-XXXX" recovery portal. "" before db_wifi_start(). out >= 33. */
void db_wifi_ap_ssid(char out[33]);

/* The AP netif (the DNS server uses it to learn the AP IP). NULL before start. */
esp_netif_t *db_wifi_ap_netif(void);

/* ---- network scan, for the first-run wizard ------------------------------- */
typedef struct {
    char    ssid[DB_STR_SSID];
    int8_t  rssi;        /* dBm */
    uint8_t authmode;    /* wifi_auth_mode_t as a plain int; 0 = open */
    uint8_t channel;
} db_wifi_ap_t;

/* Blocking scan of every visible network, newest results, up to `max_aps`
 * entries (hidden/empty SSIDs skipped). *found_out gets the number written.
 * Safe to call in any mode — but it DOES take the radio for a second or two,
 * which briefly interrupts AP traffic, so the UI should call it on demand
 * rather than polling. Returns the scan error if the radio was busy (e.g. a
 * connect attempt was in flight) after a few retries. */
esp_err_t db_wifi_scan(db_wifi_ap_t *out, size_t max_aps, size_t *found_out);

/* ---- softAP control ------------------------------------------------------- */

/* Stop the softAP (used by ota.c to free RAM + radio for the download). AP
 * clients drop until it is restarted. Returns the esp_wifi_set_mode() result. */
esp_err_t db_wifi_stop_ap(void);

/* Undo db_wifi_stop_ap(): re-enter APSTA, re-apply the softAP personality that
 * matches the current mode, re-sync its channel to the STA, and restart the AP
 * netif's DHCP/DNS handout. */
esp_err_t db_wifi_restart_ap(const db_config_t *cfg);

/* Apply cfg->ap_enabled live after the API changed it. Enabling brings the
 * softAP back up. Disabling takes it down ONLY while the STA is connected;
 * with no STA connection (recovery, boot-fail, LAN lost) the AP stays up
 * regardless, because it is then the only way into the device, and the stored
 * flag takes effect at the next boot instead. Note that a later STA reconnect
 * does NOT re-suppress the AP: unlike the reference this box never pulls its
 * AP down on its own (see the file header). */
esp_err_t db_wifi_apply_ap_enabled(const db_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DB_WIFI_MGR_H */
