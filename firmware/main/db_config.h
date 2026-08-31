/*
 * db_config.h - Persistent configuration for the Klingelbox appliance.
 *
 * One versioned struct, one NVS namespace ("klingelbox"), one blob. Everything the
 * box needs to come up on a network and talk to a radio lives here:
 *
 *   - network identity (hostname -> DHCP name + "<hostname>.local")
 *   - home Wi-Fi (STA) credentials, up to DB_STA_MAX networks tried in order
 *   - softAP settings (ssid / pass / security / channel / subnet / enabled)
 *   - recovery-portal settings (Tasmota-style config AP when nothing connects)
 *   - MQTT broker + Home Assistant discovery settings
 *   - the default OTA URL
 *   - the radio parameters (§3.5 of the design notes frequency, modulation, data rate and
 *     bandwidth are CONFIG, never constants sprinkled through the code) plus the
 *     transmit repeat policy
 *
 * WHY A SINGLE BLOB. Per-key NVS would mean one read/write and one error path per
 * field. A single plain-old-data struct (fixed-size char arrays, no pointers) is
 * copied in and out atomically, which makes load/save trivial and impossible to
 * leave half-applied. The cost is that a struct layout change invalidates the
 * stored bytes — paid for by the version header and the migration chain in
 * db_config.c, so a firmware upgrade never loses the user's Wi-Fi credentials.
 *
 * WHY NO BAKED-IN AP PASSPHRASE. The default softAP is WPA2 and its key is drawn
 * from the hardware RNG at first boot (see db_config_defaults), then persisted.
 * Shipping a constant in the image would give every unit on earth the same
 * passphrase — a release-hygiene rule inherited from the reference firmware.
 *
 * NOT PORTED from the reference (deliberate): the browser-flasher config seed
 * (le_seed_import, NVS namespace "leseed"). This project has no browser flasher,
 * so the JSON seed import and its cJSON dependency are out of scope.
 */
#ifndef DB_CONFIG_H
#define DB_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DB_STR_SSID     33   /* 32 + NUL */
#define DB_STR_PASS     65   /* 64 + NUL */
#define DB_STR_HOST     64
#define DB_STR_TOPIC    48
#define DB_STR_NAME     48
#define DB_STR_HOSTNAME 32   /* mDNS/DHCP hostname, 31 + NUL */
#define DB_STR_URL      128

/* Home networks tried in order at boot. Three is the reference's number and is
 * plenty for "house Wi-Fi + guest Wi-Fi + a phone hotspot for the workshop". */
#define DB_STA_MAX 3

/* Default softAP address. Also the DNS server the AP's DHCP hands out, and the
 * fallback dns_server.c answers with before the AP netif is up. */
#define DB_DEFAULT_AP_IP "192.168.66.1"

typedef struct {
    char ssid[DB_STR_SSID];   /* "" = empty slot */
    char pass[DB_STR_PASS];   /* "" = open network */
} db_sta_net_t;

typedef struct {
    /* ---- network identity ---- */
    char     hostname[DB_STR_HOSTNAME];  /* DHCP + mDNS -> "<hostname>.local" */

    /* ---- home Wi-Fi (STA): up to DB_STA_MAX networks tried in order ---- */
    db_sta_net_t sta[DB_STA_MAX];

    /* ---- softAP ---- */
    char     ap_ssid[DB_STR_SSID];
    char     ap_pass[DB_STR_PASS];       /* generated at first boot, see above */
    uint8_t  ap_security;                /* 0 = open, 2 = WPA2-PSK */
    uint8_t  ap_channel;                 /* honoured only in AP-only mode: on a
                                            single radio the STA channel wins */
    char     ap_ip[16];                  /* DB_DEFAULT_AP_IP; gateway == this box */
    /* Serve the operational softAP at all (default true). false = boot STA-only.
     * SAFETY: this is a boot-time preference only — whenever no STA connection
     * exists (factory, failed boot connect, LAN loss, recovery) the AP comes up
     * regardless, so the box can never make itself unreachable. */
    bool     ap_enabled;

    /* ---- recovery (Tasmota-style config AP when no STA connects) ---- */
    bool     ap_fallback_enabled;        /* default true */
    char     recovery_ap_pass[DB_STR_PASS];  /* "" = open recovery portal
                                                (default); >= 8 chars = WPA2 */

    /* ---- MQTT ---- */
    bool     mqtt_enabled;
    char     mqtt_host[DB_STR_HOST];
    uint16_t mqtt_port;
    char     mqtt_user[DB_STR_NAME];
    char     mqtt_pass[DB_STR_PASS];
    char     mqtt_base_topic[DB_STR_TOPIC];
    bool     mqtt_homeassistant;
    char     mqtt_discovery_prefix[DB_STR_TOPIC];

    /* ---- OTA ---- */
    char     ota_url[DB_STR_URL];        /* last/default update URL (optional) */

    /* ---- radio ----
     * Stored as plain integers on purpose: db_config.h must not drag the CC1101
     * driver into every translation unit that reads config. radio_modulation
     * holds a cc1101_modulation_t value (0 = CC1101_MOD_ASK_OOK); rf_service
     * casts it when it builds a cc1101_radio_cfg_t. */
    uint32_t radio_freq_hz;              /* 433920000 */
    uint8_t  radio_modulation;           /* cc1101_modulation_t as a plain int */
    uint32_t radio_datarate_bps;         /* 5000 — sets the demodulator filtering
                                            even in async mode */
    uint32_t radio_bandwidth_hz;         /* 203000 channel filter */
    int8_t   radio_tx_power_dbm;         /* 10 — modest by default; 433 MHz ISM
                                            duty-cycle/power limits are regional */

    /* ---- transmit policy ----
     * Real doorbell receivers integrate several copies of a frame before they
     * accept it (the original remotes transmit for as long as the button is
     * held), so a single replay is routinely ignored. Repeating the frame is the
     * difference between "the chime rings" and "nothing happens". */
    uint8_t  tx_repeats;                 /* 6 */
    uint32_t tx_gap_us;                  /* 8000 — silence between repeats */
} db_config_t;

/* Initialise NVS flash (call once, early, before db_config_load). Erases and
 * re-initialises a full/incompatible NVS partition rather than failing. */
esp_err_t db_nvs_init(void);

/* Fill *cfg with factory defaults. Generates the softAP passphrase from the
 * hardware RNG, so two calls do NOT produce identical structs — db_config_load
 * persists the first one it generates to keep it stable across reboots. */
void db_config_defaults(db_config_t *cfg);

/* Load the persisted config over the defaults. Never fails hard: a missing,
 * unreadable or unknown-version blob leaves *cfg at its defaults. A blob written
 * by an older layout is migrated in place and re-saved. Always returns ESP_OK
 * once *cfg is populated. */
esp_err_t db_config_load(db_config_t *cfg);

/* Persist the whole config blob (single atomic NVS key). */
esp_err_t db_config_save(const db_config_t *cfg);

/* Number of non-empty STA slots. 0 = factory state -> the boot goes straight to
 * the recovery portal (wifi_mgr.c). */
int db_config_sta_count(const db_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* DB_CONFIG_H */
