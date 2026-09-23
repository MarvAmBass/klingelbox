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
 * v2 added the web-access fields (http_pass, tls_enabled); v3 replaced the
 * PLAINTEXT http_pass with the salted PBKDF2 record http_pw (pw_hash.h).
 * v1 and v2 are frozen below and migrated per the recipe on migrate_blob().
 */
#include "db_config.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_log.h"
#include "nvs.h"
#include "nvs_flash.h"
/* For DB_UPDATE_APP_URL — the ONE place the release repo is named (see
 * update_check.h). Header-only: no dependency on the update checker itself. */
#include "update_check.h"

static const char *TAG = "db_cfg";

#define DB_NS          "klingelbox"
#define DB_BLOB_KEY    "cfg"
#define DB_CFG_VERSION 3u

/* A tiny header stamped in front of the blob so a layout change is detected. */
typedef struct {
    uint32_t version;
    uint32_t size;
} db_cfg_hdr_t;

esp_err_t db_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();

    /*
     * The stock IDF example answers BOTH failure codes with nvs_flash_erase().
     * On this appliance that is a data-destruction bug, because the partition
     * is the user's whole world: Wi-Fi credentials, the generated AP pass,
     * every learned signal, the node graph. The two codes mean very different
     * things and must not share a fate:
     *
     * NEW_VERSION_FOUND — the partition holds a VALID store written in a newer
     * on-flash format, i.e. a downgrade or rollback booted an older-IDF
     * firmware after a newer one ran. The data is perfectly recoverable by the
     * firmware that wrote it, and this codebase promises everywhere else
     * (signal_store.c load_index, node_graph.c load_blob, db_config_load) that
     * a rollback finds its data intact. So: leave the partition alone and
     * return the error. Yes, the caller's ESP_ERROR_CHECK then halts the boot
     * — deliberately. Running on without NVS is not an option (the Wi-Fi
     * driver's init requires it and would panic anyway, with a message that
     * points at the wrong culprit), and erasing would trade the user's entire
     * configuration for one convenient boot of a DOWNGRADED firmware. A halt
     * whose serial log names the fix — flash the newer firmware back — loses
     * nothing and lies about nothing.
     *
     * NO_FREE_PAGES — no erased page exists for the GC to work with: the
     * partition was truncated by a table change or is worn/corrupt. No
     * firmware, past or future, can mount it as it stands, so erasing is the
     * only path back to a working box. It still must not happen SILENTLY on a
     * serial console nobody is watching — the event ring surfaces in the web
     * UI, so the user gets told their settings are gone and why.
     */
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS was written in a NEWER on-flash format than this "
                      "firmware understands. REFUSING to erase it: the stored "
                      "settings and signals are intact and readable by the "
                      "firmware that wrote them. Flash that (newer) firmware "
                      "back to recover the device and its data.");
        return err;
    }
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGE(TAG, "NVS is unusable (no free pages — truncated or worn "
                      "partition). Erasing and re-initialising; stored settings "
                      "and signals are lost.");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
        db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0,
                       "Storage was unusable and had to be reset - "
                       "settings and signals were lost");
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

    /* Web access: open and plain out of the box, matching the trusted-LAN
     * posture the box has always shipped with. Both are opt-in. http_pw is
     * already all-zero from the memset above — iters == 0 IS "no password". */
    cfg->tls_enabled  = false;
}

/* ---- web password hashing --------------------------------------------------
 *
 * The plaintext exists only in flight: in the POST body that sets it, in the
 * Authorization header that presents it, and — once, during the v2->v3
 * migration below — in the old stored blob. What persists is the pw_hash.h
 * record. The iteration count is calibrated HERE, at set-time, by timing a
 * short probe derive on this very box (pw_hash.h explains the target and the
 * clamp); the count is stored in the record, so hashes written by an older,
 * slower or differently-calibrated build keep verifying forever.
 */
#define DB_PW_PROBE_ITERS 200   /* ~10-30 ms: enough signal, negligible cost */

static uint32_t calibrate_pw_iters(void)
{
    uint8_t salt[DB_PW_SALT_LEN] = { 0 };
    uint8_t out[DB_PW_HASH_LEN];
    int64_t t0 = esp_timer_get_time();
    db_pw_pbkdf2("calibration-probe", 17, salt, sizeof(salt),
                 DB_PW_PROBE_ITERS, out);
    int64_t dt = esp_timer_get_time() - t0;
    uint32_t iters = db_pw_pick_iterations(DB_PW_PROBE_ITERS,
                                           (dt > 0) ? (uint64_t)dt : 0);
    ESP_LOGI(TAG, "PBKDF2 calibration: %d iterations in %lld us -> %u chosen",
             DB_PW_PROBE_ITERS, (long long)dt, (unsigned)iters);
    return iters;
}

void db_config_set_http_password(db_config_t *cfg, const char *password)
{
    if (!password || !password[0]) {
        /* Remove: zero the whole record so no stale salt/hash lingers in the
         * blob (and so iters == 0 is unambiguous). */
        memset(&cfg->http_pw, 0, sizeof(cfg->http_pw));
        return;
    }
    uint8_t salt[DB_PW_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    db_pw_rec_from_plaintext(&cfg->http_pw, password, salt,
                             calibrate_pw_iters());
}

/* ---- migration chain -------------------------------------------------------
 *
 * Called with the stored header and payload when the blob is NOT the current
 * layout. Returns true if *cfg was populated from the old bytes (the caller then
 * re-saves it in the current layout), false to keep the factory defaults.
 *
 * ADDING v4 (the whole recipe, exactly as v2 and v3 followed it):
 *   1. Copy the CURRENT db_config_t verbatim into a frozen
 *      `typedef struct { ... } db_config_v3_t;` beside db_config_v2_t,
 *      commented "layout as shipped in DB_CFG_VERSION 3".
 *   2. Bump DB_CFG_VERSION to 4 and edit db_config_t / db_config_defaults().
 *   3. Add a `migrate_v3()` that calls db_config_defaults(cfg) and then copies
 *      every v3 field across one by one, and wire it into the switch below
 *      next to `case 2:`.
 *
 * Migrations are chained through the current struct, not against each other:
 * every migrate_vN() lands directly on today's db_config_t, so an upgrade from
 * any shipped version is a single hop and old code never has to be kept alive.
 */
/* Layout as shipped in DB_CFG_VERSION 1 — frozen verbatim; see the recipe. */
typedef struct {
    char     hostname[DB_STR_HOSTNAME];
    db_sta_net_t sta[DB_STA_MAX];
    char     ap_ssid[DB_STR_SSID];
    char     ap_pass[DB_STR_PASS];
    uint8_t  ap_security;
    uint8_t  ap_channel;
    char     ap_ip[16];
    bool     ap_enabled;
    bool     ap_fallback_enabled;
    char     recovery_ap_pass[DB_STR_PASS];
    bool     mqtt_enabled;
    char     mqtt_host[DB_STR_HOST];
    uint16_t mqtt_port;
    char     mqtt_user[DB_STR_NAME];
    char     mqtt_pass[DB_STR_PASS];
    char     mqtt_base_topic[DB_STR_TOPIC];
    bool     mqtt_homeassistant;
    char     mqtt_discovery_prefix[DB_STR_TOPIC];
    char     ota_url[DB_STR_URL];
    uint32_t radio_freq_hz;
    uint8_t  radio_modulation;
    uint32_t radio_datarate_bps;
    uint32_t radio_bandwidth_hz;
    int8_t   radio_tx_power_dbm;
    uint8_t  tx_repeats;
    uint32_t tx_gap_us;
} db_config_v1_t;

/* Layout as shipped in DB_CFG_VERSION 2 — frozen verbatim; see the recipe.
 * The distinguishing field is http_pass: v2 stored the web password as
 * PLAINTEXT. migrate_v2() below is the one place that plaintext is ever read
 * again, to hash it into the v3 record. */
typedef struct {
    char     hostname[DB_STR_HOSTNAME];
    db_sta_net_t sta[DB_STA_MAX];
    char     ap_ssid[DB_STR_SSID];
    char     ap_pass[DB_STR_PASS];
    uint8_t  ap_security;
    uint8_t  ap_channel;
    char     ap_ip[16];
    bool     ap_enabled;
    bool     ap_fallback_enabled;
    char     recovery_ap_pass[DB_STR_PASS];
    bool     mqtt_enabled;
    char     mqtt_host[DB_STR_HOST];
    uint16_t mqtt_port;
    char     mqtt_user[DB_STR_NAME];
    char     mqtt_pass[DB_STR_PASS];
    char     mqtt_base_topic[DB_STR_TOPIC];
    bool     mqtt_homeassistant;
    char     mqtt_discovery_prefix[DB_STR_TOPIC];
    char     ota_url[DB_STR_URL];
    uint32_t radio_freq_hz;
    uint8_t  radio_modulation;
    uint32_t radio_datarate_bps;
    uint32_t radio_bandwidth_hz;
    int8_t   radio_tx_power_dbm;
    uint8_t  tx_repeats;
    uint32_t tx_gap_us;
    char     http_pass[DB_STR_PASS];
    bool     tls_enabled;
} db_config_v2_t;

/* Field by field over the current defaults, never memcpy — a future edit to
 * db_config_t must not silently skew this migration. The v2/v3 additions
 * (web password, tls_enabled) keep their defaults: auth off, TLS off, exactly
 * the behaviour every v1 box already had. */
static void migrate_v1(db_config_t *cfg, const db_config_v1_t *old)
{
    db_config_defaults(cfg);

    strlcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    for (int i = 0; i < DB_STA_MAX; i++) {
        strlcpy(cfg->sta[i].ssid, old->sta[i].ssid, sizeof(cfg->sta[i].ssid));
        strlcpy(cfg->sta[i].pass, old->sta[i].pass, sizeof(cfg->sta[i].pass));
    }
    strlcpy(cfg->ap_ssid, old->ap_ssid, sizeof(cfg->ap_ssid));
    strlcpy(cfg->ap_pass, old->ap_pass, sizeof(cfg->ap_pass));
    cfg->ap_security = old->ap_security;
    cfg->ap_channel  = old->ap_channel;
    strlcpy(cfg->ap_ip, old->ap_ip, sizeof(cfg->ap_ip));
    cfg->ap_enabled          = old->ap_enabled;
    cfg->ap_fallback_enabled = old->ap_fallback_enabled;
    strlcpy(cfg->recovery_ap_pass, old->recovery_ap_pass,
            sizeof(cfg->recovery_ap_pass));

    cfg->mqtt_enabled = old->mqtt_enabled;
    strlcpy(cfg->mqtt_host, old->mqtt_host, sizeof(cfg->mqtt_host));
    cfg->mqtt_port = old->mqtt_port;
    strlcpy(cfg->mqtt_user, old->mqtt_user, sizeof(cfg->mqtt_user));
    strlcpy(cfg->mqtt_pass, old->mqtt_pass, sizeof(cfg->mqtt_pass));
    strlcpy(cfg->mqtt_base_topic, old->mqtt_base_topic,
            sizeof(cfg->mqtt_base_topic));
    cfg->mqtt_homeassistant = old->mqtt_homeassistant;
    strlcpy(cfg->mqtt_discovery_prefix, old->mqtt_discovery_prefix,
            sizeof(cfg->mqtt_discovery_prefix));

    strlcpy(cfg->ota_url, old->ota_url, sizeof(cfg->ota_url));

    cfg->radio_freq_hz       = old->radio_freq_hz;
    cfg->radio_modulation    = old->radio_modulation;
    cfg->radio_datarate_bps  = old->radio_datarate_bps;
    cfg->radio_bandwidth_hz  = old->radio_bandwidth_hz;
    cfg->radio_tx_power_dbm  = old->radio_tx_power_dbm;
    cfg->tx_repeats          = old->tx_repeats;
    cfg->tx_gap_us           = old->tx_gap_us;
}

/* v2 -> v3: everything is a straight field copy except the web password. A v2
 * blob holds it as PLAINTEXT; this is the single place that plaintext is ever
 * readable again, so it is hashed RIGHT HERE — through the same
 * db_config_set_http_password path a POST takes, calibration included — and
 * only the pw_hash record is ever written back. The upgrade contract: a v2 box
 * with a password comes up with that same password still working. */
static void migrate_v2(db_config_t *cfg, const db_config_v2_t *old)
{
    db_config_defaults(cfg);

    strlcpy(cfg->hostname, old->hostname, sizeof(cfg->hostname));
    for (int i = 0; i < DB_STA_MAX; i++) {
        strlcpy(cfg->sta[i].ssid, old->sta[i].ssid, sizeof(cfg->sta[i].ssid));
        strlcpy(cfg->sta[i].pass, old->sta[i].pass, sizeof(cfg->sta[i].pass));
    }
    strlcpy(cfg->ap_ssid, old->ap_ssid, sizeof(cfg->ap_ssid));
    strlcpy(cfg->ap_pass, old->ap_pass, sizeof(cfg->ap_pass));
    cfg->ap_security = old->ap_security;
    cfg->ap_channel  = old->ap_channel;
    strlcpy(cfg->ap_ip, old->ap_ip, sizeof(cfg->ap_ip));
    cfg->ap_enabled          = old->ap_enabled;
    cfg->ap_fallback_enabled = old->ap_fallback_enabled;
    strlcpy(cfg->recovery_ap_pass, old->recovery_ap_pass,
            sizeof(cfg->recovery_ap_pass));

    cfg->mqtt_enabled = old->mqtt_enabled;
    strlcpy(cfg->mqtt_host, old->mqtt_host, sizeof(cfg->mqtt_host));
    cfg->mqtt_port = old->mqtt_port;
    strlcpy(cfg->mqtt_user, old->mqtt_user, sizeof(cfg->mqtt_user));
    strlcpy(cfg->mqtt_pass, old->mqtt_pass, sizeof(cfg->mqtt_pass));
    strlcpy(cfg->mqtt_base_topic, old->mqtt_base_topic,
            sizeof(cfg->mqtt_base_topic));
    cfg->mqtt_homeassistant = old->mqtt_homeassistant;
    strlcpy(cfg->mqtt_discovery_prefix, old->mqtt_discovery_prefix,
            sizeof(cfg->mqtt_discovery_prefix));

    strlcpy(cfg->ota_url, old->ota_url, sizeof(cfg->ota_url));

    cfg->radio_freq_hz       = old->radio_freq_hz;
    cfg->radio_modulation    = old->radio_modulation;
    cfg->radio_datarate_bps  = old->radio_datarate_bps;
    cfg->radio_bandwidth_hz  = old->radio_bandwidth_hz;
    cfg->radio_tx_power_dbm  = old->radio_tx_power_dbm;
    cfg->tx_repeats          = old->tx_repeats;
    cfg->tx_gap_us           = old->tx_gap_us;

    /* The plaintext-to-hash moment. A NUL-termination clamp first: these
     * bytes come straight off flash, and a corrupted blob must not send
     * strlen() off the end of the frozen struct. */
    char pw[DB_STR_PASS];
    strlcpy(pw, old->http_pass, sizeof(pw));
    if (pw[0]) {
        db_config_set_http_password(cfg, pw);
        ESP_LOGI(TAG, "stored web password migrated from plaintext to a "
                      "salted PBKDF2 hash");
    }
    memset(pw, 0, sizeof(pw));

    cfg->tls_enabled = old->tls_enabled;
}

static bool migrate_blob(db_config_t *cfg, uint32_t version, uint32_t size,
                         const void *payload, size_t payload_len)
{
    switch (version) {
    case 1:
        if (size != sizeof(db_config_v1_t) ||
            payload_len != sizeof(db_config_v1_t)) break;
        migrate_v1(cfg, (const db_config_v1_t *)payload);
        ESP_LOGI(TAG, "config migrated v1 -> v%u", DB_CFG_VERSION);
        return true;
    case 2:
        if (size != sizeof(db_config_v2_t) ||
            payload_len != sizeof(db_config_v2_t)) break;
        migrate_v2(cfg, (const db_config_v2_t *)payload);
        ESP_LOGI(TAG, "config migrated v2 -> v%u", DB_CFG_VERSION);
        return true;
    default:
        break;
    }
    ESP_LOGW(TAG, "stored config layout v%u/%u is not migratable — "
                  "falling back to factory defaults",
             (unsigned)version, (unsigned)size);
    return false;
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
