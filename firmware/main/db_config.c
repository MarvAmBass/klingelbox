/*
 * db_config.c - see db_config.h.
 *
 * STORAGE FORMAT. The whole db_config_t is written under one NVS key as
 *
 *     [ db_cfg_hdr_t { version, size } ][ db_config_t payload ]
 *
 * The header is what makes a firmware upgrade safe. On load the stored version
 * AND the stored struct size are checked against the running build: only an
 * exact match is memcpy'd straight into db_config_t. Anything older goes through
 * the migration chain below; anything unrecognised falls back to factory
 * defaults (loudly) rather than reinterpreting bytes under a changed layout,
 * which is how you get a device that "works" but with garbage in half its
 * fields.
 *
 * THE MIGRATION CHAIN. Every shipped layout is frozen here as its own typedef
 * (db_config_vN_t) and gets a migrate_vN() that copies field by field over the
 * current defaults. Field-by-field, never memcpy: a future edit to db_config_t
 * must not silently skew an old migration. Fields introduced after vN simply
 * keep their default value.
 *
 * v1 is the first shipped layout, so the chain is currently EMPTY — the switch
 * in migrate_blob() exists with the scaffolding in place and documented, so
 * adding v2 is a small, obvious edit rather than a redesign. See the comment on
 * migrate_blob() for the exact three steps.
 */
#include "db_config.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
/* For DB_UPDATE_APP_URL — the ONE place the release repo is named (see
 * update_check.h). Header-only: no dependency on the update checker itself. */
#include "update_check.h"

static const char *TAG = "db_cfg";

#define DB_NS          "klingelbox"
#define DB_BLOB_KEY    "cfg"
#define DB_CFG_VERSION 1u

/* A tiny header stamped in front of the blob so a layout change is detected. */
typedef struct {
    uint32_t version;
    uint32_t size;
} db_cfg_hdr_t;

esp_err_t db_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "erasing NVS (%s) and re-initialising", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

/* Random WPA2 passphrase: 12 chars out of [a-zA-Z0-9] from the hardware RNG.
 * esp_random() is only a true RNG once the radio is up, but this runs at first
 * boot before Wi-Fi starts; the entropy is still far better than a constant
 * baked into every image, and the value is persisted immediately so the user
 * can read it back off the UI. */
static void db_gen_ap_pass(char *dst, size_t cap)
{
    static const char alphabet[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    size_t n = 12;
    if (n >= cap) n = cap - 1;
    for (size_t i = 0; i < n; i++)
        dst[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    dst[n] = '\0';
}

void db_config_defaults(db_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    strlcpy(cfg->hostname, "klingelbox", sizeof(cfg->hostname));

    /* No home Wi-Fi out of the box: every sta[] slot stays empty (memset above),
     * so a factory image boots straight into the recovery portal and the first
     * thing the user sees is the Wi-Fi wizard. */

    strlcpy(cfg->ap_ssid, "Klingelbox", sizeof(cfg->ap_ssid));
    cfg->ap_security = 2;                 /* WPA2-PSK: the operational AP is
                                             never open out of the box */
    db_gen_ap_pass(cfg->ap_pass, sizeof(cfg->ap_pass));
    cfg->ap_channel = 6;
    strlcpy(cfg->ap_ip, DB_DEFAULT_AP_IP, sizeof(cfg->ap_ip));
    cfg->ap_enabled = true;

    cfg->ap_fallback_enabled = true;
    cfg->recovery_ap_pass[0] = '\0';      /* open recovery portal */

    cfg->mqtt_enabled = false;
    cfg->mqtt_host[0] = '\0';
    cfg->mqtt_port = 1883;
    strlcpy(cfg->mqtt_base_topic, "klingelbox", sizeof(cfg->mqtt_base_topic));
    cfg->mqtt_homeassistant = true;
    strlcpy(cfg->mqtt_discovery_prefix, "homeassistant",
            sizeof(cfg->mqtt_discovery_prefix));

    /* The stable release asset, so "update from a URL" is a button and not a
     * typing exercise. Still fully editable — a fork, a local web server or a
     * one-off test build is a matter of replacing the text. The AUTOMATIC check
     * does not use this at all; it follows the URLs it finds in the release. */
    strlcpy(cfg->ota_url, DB_UPDATE_APP_URL, sizeof(cfg->ota_url));

    /* Radio: a 433.92 MHz OOK doorbell. These mirror cc1101_radio_cfg_default()
     * — the driver's helper is the bring-up default, this is the persisted,
     * user-editable one. */
    cfg->radio_freq_hz       = 433920000u;
    cfg->radio_modulation    = 0;         /* CC1101_MOD_ASK_OOK */
    cfg->radio_datarate_bps  = 5000u;
    cfg->radio_bandwidth_hz  = 203000u;
    cfg->radio_tx_power_dbm  = 10;

    cfg->tx_repeats = 6;                  /* see db_config.h: one copy is
                                             routinely ignored by receivers */
    cfg->tx_gap_us  = 8000u;
}

/* ---- migration chain -------------------------------------------------------
 *
 * Called with the stored header and payload when the blob is NOT the current
 * layout. Returns true if *cfg was populated from the old bytes (the caller then
 * re-saves it in the current layout), false to keep the factory defaults.
 *
 * ADDING v2 (the whole recipe):
 *   1. Copy the CURRENT db_config_t verbatim into a frozen
 *      `typedef struct { ... } db_config_v1_t;` right above this function,
 *      commented "layout as shipped in DB_CFG_VERSION 1".
 *   2. Bump DB_CFG_VERSION to 2 and edit db_config_t / db_config_defaults().
 *   3. Add a `migrate_v1()` that calls db_config_defaults(cfg) and then copies
 *      every v1 field across one by one, and wire it into the switch below:
 *
 *        case 1:
 *            if (size != sizeof(db_config_v1_t) ||
 *                payload_len != sizeof(db_config_v1_t)) break;
 *            migrate_v1(cfg, (const db_config_v1_t *)payload);
 *            ESP_LOGI(TAG, "config migrated v1 -> v%u", DB_CFG_VERSION);
 *            return true;
 *
 * Migrations are chained through the current struct, not against each other:
 * every migrate_vN() lands directly on today's db_config_t, so an upgrade from
 * any shipped version is a single hop and old code never has to be kept alive.
 */
static bool migrate_blob(db_config_t *cfg, uint32_t version, uint32_t size,
                         const void *payload, size_t payload_len)
{
    (void)cfg;
    (void)size;
    (void)payload;
    (void)payload_len;

    switch (version) {
    /* No older layout has ever shipped: v1 is the first. See the recipe above. */
    default:
        ESP_LOGW(TAG, "stored config layout v%u/%u is not migratable — "
                      "falling back to factory defaults",
                 (unsigned)version, (unsigned)size);
        return false;
    }
}

esp_err_t db_config_load(db_config_t *cfg)
{
    db_config_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored config (%s) — using defaults",
                 esp_err_to_name(err));
        db_config_save(cfg);   /* persist now so the generated AP passphrase is
                                  stable across reboots */
        return ESP_OK;
    }

    /* save_back: write the blob in the current layout after a successful
     * migration (so the next boot reads it directly), or persist the factory
     * defaults when no blob exists. Never set on a read/OOM failure or an
     * unknown stored version — an existing blob must not be clobbered by a
     * transient error. */
    bool save_back = false;
    size_t got = 0;
    err = nvs_get_blob(h, DB_BLOB_KEY, NULL, &got);
    if (err == ESP_OK && got >= sizeof(db_cfg_hdr_t)) {
        uint8_t *buf = malloc(got);
        if (buf) {
            if (nvs_get_blob(h, DB_BLOB_KEY, buf, &got) == ESP_OK) {
                db_cfg_hdr_t hdr;
                memcpy(&hdr, buf, sizeof(hdr));
                const uint8_t *payload = buf + sizeof(hdr);
                size_t payload_len = got - sizeof(hdr);

                if (hdr.version == DB_CFG_VERSION &&
                    hdr.size == sizeof(db_config_t) &&
                    payload_len == sizeof(db_config_t)) {
                    memcpy(cfg, payload, sizeof(db_config_t));
                    ESP_LOGI(TAG, "config loaded from NVS (v%u, %u bytes)",
                             (unsigned)hdr.version, (unsigned)payload_len);
                } else if (migrate_blob(cfg, hdr.version, hdr.size,
                                        payload, payload_len)) {
                    save_back = true;
                } else {
                    /* migrate_blob already logged the reason; cfg still holds
                     * the factory defaults. Deliberately NOT re-saved: an
                     * unrecognised blob may come from a NEWER firmware the user
                     * is about to roll back to, and overwriting it would throw
                     * their configuration away for good. */
                }
            } else {
                ESP_LOGE(TAG, "config blob read failed — using defaults");
            }
            free(buf);
        } else {
            ESP_LOGE(TAG, "out of memory reading the config blob — using defaults");
        }
    } else {
        ESP_LOGI(TAG, "config blob absent/too small — using defaults");
        save_back = true;   /* persist defaults incl. the generated AP passphrase */
    }
    nvs_close(h);

    if (save_back)
        db_config_save(cfg);
    return ESP_OK;
}

esp_err_t db_config_save(const db_config_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t need = sizeof(db_cfg_hdr_t) + sizeof(db_config_t);
    uint8_t *buf = malloc(need);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }

    db_cfg_hdr_t hdr = { .version = DB_CFG_VERSION, .size = sizeof(db_config_t) };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), cfg, sizeof(db_config_t));

    err = nvs_set_blob(h, DB_BLOB_KEY, buf, need);
    if (err == ESP_OK) err = nvs_commit(h);
    free(buf);
    nvs_close(h);

    if (err == ESP_OK) ESP_LOGI(TAG, "config saved to NVS (%u bytes)",
                                (unsigned)need);
    else ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
    return err;
}

int db_config_sta_count(const db_config_t *cfg)
{
    int n = 0;
    for (int i = 0; i < DB_STA_MAX; i++)
        if (cfg->sta[i].ssid[0]) n++;
    return n;
}
