/*
 * ota.c - see ota.h.
 *
 * The app path is esp_https_ota into the inactive slot, reboot, then app-rollback
 * verifies on the next boot (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE). The web-UI
 * path is a raw partition write, which needs its own care: the SPIFFS mount is
 * dropped before the erase so the writes cannot race a live filesystem, and the
 * device always reboots afterwards — success or failure — so it comes back with
 * a clean remount instead of half a filesystem.
 *
 * WHY THE SOFTAP GOES DOWN DURING AN UPDATE. On a single radio the AP costs both
 * RAM and airtime, and a download that stalls halfway is worse than a few minutes
 * without the hotspot. The STA link (which is what the download runs over) stays
 * up. Every path here reboots at the end, which is also what brings the AP back.
 *
 * FAILURE HONESTY. A failure BEFORE the storage erase leaves the web UI exactly
 * as it was. A failure AFTER it leaves the UI blank until a good image is pushed
 * again — the app, the REST API and MQTT keep working, which is what makes the
 * recovery possible at all. Both cases are logged as distinct messages on
 * purpose: "it failed" is not actionable, "it failed and your UI is now blank"
 * is.
 */
#include "ota.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "update_check.h"   /* DB_UPDATE_URL_MAX */
#include "wifi_mgr.h"

static const char *TAG = "db_ota";

/* Shared guard: the app OTA, the web-UI OTA and a browser upload session are all
 * mutually exclusive — they compete for the same flash and the same radio. */
static volatile bool s_running;
/* Sized to the longest URL any caller can legally hand over: update_check.c
 * delivers discovered asset URLs up to DB_UPDATE_URL_MAX. A URL that still does
 * not fit is rejected in set_url() — fetching a silently truncated URL would
 * fail with a baffling reboot instead of an error the user can act on. */
static char s_url[DB_UPDATE_URL_MAX];

/* Both URL entry points share this so neither can forget the truncation check. */
static esp_err_t set_url(const char *url)
{
    if (!url || !url[0]) return ESP_ERR_INVALID_ARG;
    if (strlcpy(s_url, url, sizeof(s_url)) >= sizeof(s_url)) {
        s_url[0] = '\0';
        ESP_LOGE(TAG, "OTA URL longer than %u bytes — refusing to fetch a "
                      "truncated URL", (unsigned)sizeof(s_url) - 1);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* SPIFFS mount identity — must match the web UI's mount in http_api.c. */
#define WEBUI_PART_LABEL  "storage"
#define WEBUI_CHUNK       4096

static void ota_task(void *arg)
{
    ESP_LOGW(TAG, "app OTA starting from %s", s_url);

    /* Free RAM + radio for the download: drop the softAP (the STA stays up). */
    db_wifi_stop_ap();
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_http_client_config_t http = {
        .url = s_url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
    };
    /* HTTPS (e.g. GitHub release assets): verify against the ESP-IDF certificate
     * bundle. Attached only for https:// so plain-HTTP URLs (a local test server,
     * allowed via CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP) need no TLS config at all. */
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncmp(s_url, "https://", 8) == 0)
        http.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    esp_https_ota_config_t ota = { .http_config = &http };

    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "app OTA OK — rebooting into the new image "
                      "(rollback protects this first boot)");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "app OTA failed: %s — staying on the current image",
                 esp_err_to_name(err));
        /* The AP was stopped; the simplest robust way back to a fully reachable
         * box is a reboot into the still-valid current slot. */
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }
    s_running = false;   /* not reached */
    vTaskDelete(NULL);
}

esp_err_t db_ota_start(const char *url)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!db_wifi_sta_connected()) {
        ESP_LOGE(TAG, "app OTA needs the STA (home Wi-Fi) up — use the browser "
                      "upload instead when the box is only on its own AP");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = set_url(url);
    if (err != ESP_OK) return err;
    s_running = true;
    if (xTaskCreate(ota_task, "db_ota", 8192, NULL, 5, NULL) != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ web-UI (SPIFFS) OTA
 *
 * Streams a raw SPIFFS image over HTTP into the `storage` partition. The app OTA
 * slots are never touched.
 */
static void webui_ota_task(void *arg)
{
    ESP_LOGW(TAG, "web UI OTA starting from %s", s_url);

    /* Let the httpd flush its "started" reply, then (like the app OTA) drop the
     * softAP to free RAM + radio. We reboot afterwards anyway. */
    vTaskDelay(pdMS_TO_TICKS(500));
    db_wifi_stop_ap();
    vTaskDelay(pdMS_TO_TICKS(500));

    bool erased = false;
    char *buf = NULL;
    esp_http_client_handle_t cli = NULL;
    const char *fail = NULL;         /* human-readable failure reason */
    esp_err_t err;
    int64_t len = 0;
    size_t off = 0;

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, WEBUI_PART_LABEL);
    if (!part) { fail = "no '" WEBUI_PART_LABEL "' SPIFFS partition in the table"; goto done; }

    esp_http_client_config_t http = {
        .url = s_url,
        .timeout_ms = 20000,
        .keep_alive_enable = true,
        /* Plain HTTP allowed, same policy as the app OTA (see db_ota_start). */
    };
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    if (strncmp(s_url, "https://", 8) == 0)
        http.crt_bundle_attach = esp_crt_bundle_attach;
#endif
    cli = esp_http_client_init(&http);
    if (!cli) { fail = "http client init failed"; goto done; }

    /* Open + follow redirects manually: open/fetch_headers does not auto-follow,
     * and GitHub's /releases/latest/download/<asset> 302s to objects.githubusercontent.com. */
    int status = 0;
    for (int redirects = 0; ; redirects++) {
        if (esp_http_client_open(cli, 0) != ESP_OK) { fail = "could not open URL"; goto done; }
        len = esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects >= 5) { fail = "too many HTTP redirects"; goto done; }
            esp_http_client_set_redirection(cli);   /* re-target at Location: */
            esp_http_client_close(cli);
            continue;
        }
        break;
    }
    if (status != 200) { fail = "HTTP status != 200"; goto done; }
    if (len <= 0) { fail = "missing/zero Content-Length (chunked not supported)"; goto done; }
    if ((uint64_t)len > part->size) { fail = "image larger than the storage partition"; goto done; }

    buf = malloc(WEBUI_CHUNK);
    if (!buf) { fail = "out of memory for the download buffer"; goto done; }

    /* Point of no return: unmount, then erase the whole partition. */
    esp_vfs_spiffs_unregister(WEBUI_PART_LABEL);   /* ignore "not mounted" */
    err = esp_partition_erase_range(part, 0, part->size);
    if (err != ESP_OK) { fail = "partition erase failed"; goto done; }
    erased = true;

    while (off < (size_t)len) {
        size_t want = (size_t)len - off;
        if (want > WEBUI_CHUNK) want = WEBUI_CHUNK;
        int n = esp_http_client_read(cli, buf, (int)want);
        if (n < 0) { fail = "download read error"; goto done; }
        if (n == 0) break;                          /* premature EOF: caught below */
        err = esp_partition_write(part, off, buf, (size_t)n);
        if (err != ESP_OK) { fail = "partition write failed"; goto done; }
        off += (size_t)n;
    }
    if (off != (size_t)len) { fail = "short download (written != Content-Length)"; goto done; }

done:
    if (cli) esp_http_client_cleanup(cli);
    free(buf);
    if (!fail) {
        ESP_LOGW(TAG, "web UI OTA OK (%u bytes) — rebooting to remount the new UI",
                 (unsigned)off);
    } else if (!erased) {
        ESP_LOGE(TAG, "web UI OTA failed before erase: %s — UI untouched, rebooting", fail);
    } else {
        ESP_LOGE(TAG, "web UI OTA failed after erase: %s — the UI is now blank; "
                      "push a good storage image again (the app, REST API and MQTT "
                      "still work). Rebooting.", fail);
    }
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    s_running = false;   /* not reached; kept for symmetry with ota_task */
    vTaskDelete(NULL);
}

esp_err_t db_ota_webui_from_url(const char *url)
{
    if (s_running) return ESP_ERR_INVALID_STATE;
    if (!db_wifi_sta_connected()) {
        ESP_LOGE(TAG, "web UI OTA needs the STA (home Wi-Fi) up");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = set_url(url);
    if (err != ESP_OK) return err;
    s_running = true;
    if (xTaskCreate(webui_ota_task, "db_webui_ota", 8192, NULL, 5, NULL) != pdPASS) {
        s_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ browser-upload OTA
 *
 * Caller-driven streaming session (see ota.h): http_api.c feeds us the raw POST
 * body chunk by chunk from the httpd worker task. Reuses the same write targets
 * as the URL paths above — esp_ota_* into the inactive app slot, or raw
 * esp_partition_write into the erased `storage` partition — and shares their
 * s_running mutual-exclusion guard.
 *
 * Note that an upload session does NOT stop the softAP: the upload very often
 * arrives OVER that AP (a box with no home Wi-Fi is exactly the one you update by
 * hand), so killing it would kill the transfer.
 */
#define UPLOAD_MIN_LEN 4096   /* reject obviously-truncated junk before touching flash */

static struct {
    bool active;
    bool erased;                    /* web UI: storage partition already wiped */
    db_ota_upload_kind_t kind;
    size_t len, off;
    esp_ota_handle_t app;           /* app-image session handle */
    const esp_partition_t *part;
} s_up;

esp_err_t db_ota_upload_begin(db_ota_upload_kind_t kind, size_t image_len,
                              const char **errmsg)
{
    *errmsg = NULL;
    if (s_running) { *errmsg = "another update is already running"; return ESP_ERR_INVALID_STATE; }
    if (image_len < UPLOAD_MIN_LEN) { *errmsg = "file too small to be a firmware image"; return ESP_ERR_INVALID_ARG; }
    memset(&s_up, 0, sizeof(s_up));
    s_up.kind = kind;
    s_up.len = image_len;

    if (kind == DB_OTA_UPLOAD_APP) {
        s_up.part = esp_ota_get_next_update_partition(NULL);
        if (!s_up.part) { *errmsg = "no inactive OTA app slot found"; return ESP_ERR_NOT_FOUND; }
        if (image_len > s_up.part->size) { *errmsg = "image larger than the OTA app slot"; return ESP_ERR_INVALID_SIZE; }
        /* Erases the slot up front; a few seconds for a full 2 MB image. */
        esp_err_t err = esp_ota_begin(s_up.part, image_len, &s_up.app);
        if (err != ESP_OK) { *errmsg = "could not prepare the OTA slot"; return err; }
    } else {
        s_up.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                             WEBUI_PART_LABEL);
        if (!s_up.part) { *errmsg = "no '" WEBUI_PART_LABEL "' SPIFFS partition in the table"; return ESP_ERR_NOT_FOUND; }
        if (image_len > s_up.part->size) { *errmsg = "image larger than the storage partition"; return ESP_ERR_INVALID_SIZE; }
        /* Point of no return, same policy as the URL web-UI OTA: unmount so the
         * raw writes cannot race a live filesystem, then wipe the partition. */
        esp_vfs_spiffs_unregister(WEBUI_PART_LABEL);   /* ignore "not mounted" */
        esp_err_t err = esp_partition_erase_range(s_up.part, 0, s_up.part->size);
        if (err != ESP_OK) { s_up.erased = true; *errmsg = "partition erase failed"; return err; }
        s_up.erased = true;
    }
    s_running = true;
    s_up.active = true;
    ESP_LOGW(TAG, "upload OTA (%s) started: %u bytes -> '%s'",
             kind == DB_OTA_UPLOAD_APP ? "app" : "web UI",
             (unsigned)image_len, s_up.part->label);
    return ESP_OK;
}

esp_err_t db_ota_upload_write(const void *data, size_t len, const char **errmsg)
{
    *errmsg = NULL;
    if (!s_up.active) { *errmsg = "no upload in progress"; return ESP_ERR_INVALID_STATE; }
    if (s_up.off + len > s_up.len) { *errmsg = "more data than the announced length"; return ESP_ERR_INVALID_SIZE; }
    esp_err_t err;
    if (s_up.kind == DB_OTA_UPLOAD_APP) {
        /* esp_ota_write also sanity-checks the image header (magic/chip) early,
         * so a .bin for the wrong target fails in the first chunk. */
        err = esp_ota_write(s_up.app, data, len);
        if (err != ESP_OK) {
            *errmsg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
                          ? "not a valid app image for this chip"
                          : "flash write failed";
            return err;
        }
    } else {
        err = esp_partition_write(s_up.part, s_up.off, data, len);
        if (err != ESP_OK) { *errmsg = "flash write failed"; return err; }
    }
    s_up.off += len;
    return ESP_OK;
}

esp_err_t db_ota_upload_finish(const char **errmsg)
{
    *errmsg = NULL;
    if (!s_up.active) { *errmsg = "no upload in progress"; return ESP_ERR_INVALID_STATE; }
    if (s_up.off != s_up.len) { *errmsg = "short upload (received less than announced)"; return ESP_ERR_INVALID_SIZE; }
    if (s_up.kind == DB_OTA_UPLOAD_APP) {
        esp_err_t err = esp_ota_end(s_up.app);   /* full image validation */
        s_up.app = 0;
        if (err != ESP_OK) {
            s_up.active = false;
            s_running = false;
            *errmsg = (err == ESP_ERR_OTA_VALIDATE_FAILED)
                          ? "image validation failed (not a bootable app image)"
                          : "finalizing the OTA slot failed";
            return err;
        }
        err = esp_ota_set_boot_partition(s_up.part);
        if (err != ESP_OK) {
            s_up.active = false;
            s_running = false;
            *errmsg = "could not set the new boot partition";
            return err;
        }
    }
    /* Web UI: the raw image is fully written; there is nothing to finalize. */
    ESP_LOGW(TAG, "upload OTA (%s) complete: %u bytes — caller reboots now",
             s_up.kind == DB_OTA_UPLOAD_APP ? "app" : "web UI", (unsigned)s_up.off);
    s_up.active = false;
    /* s_running stays set on purpose: the caller reboots momentarily, and a
     * second update slipping in during that window would be a disaster. */
    return ESP_OK;
}

bool db_ota_upload_abort(void)
{
    bool reboot_needed = s_up.erased;   /* storage wiped -> reboot for a clean remount */
    if (s_up.active && s_up.kind == DB_OTA_UPLOAD_APP && s_up.app)
        esp_ota_abort(s_up.app);        /* inactive slot: harmless half-written */
    memset(&s_up, 0, sizeof(s_up));
    s_running = false;
    return reboot_needed;
}

void db_ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "confirming the new image (rollback cancelled)");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}
