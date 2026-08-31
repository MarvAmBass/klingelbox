/*
 * ota.h - Firmware and web-UI updates (see ota.c).
 *
 * Two independent things can be updated, and keeping them separate is the point:
 *
 *   1. THE APP. Written to the inactive OTA slot, marked as the boot target, then
 *      the box reboots. The bootloader plus app-rollback verify the new image on
 *      that first boot: if it never calls db_ota_mark_valid(), the next reset
 *      reverts to the previous slot. A bad flash therefore costs a reboot, not a
 *      USB cable and a disassembled doorbell.
 *   2. THE WEB UI. A raw SPIFFS image written straight to the `storage`
 *      partition. Because the UI is a plain no-build-step bundle flashed as a
 *      filesystem image, it can be replaced without recompiling or touching the
 *      app slots at all.
 *
 * Each can arrive either by URL (the box downloads it) or by browser upload (the
 * browser POSTs the bytes and we stream them into flash). All four paths share
 * ONE in-progress guard: only a single update of any kind may run at a time.
 *
 * The softAP is stopped for the duration of an update, freeing RAM and radio time
 * for the download — the deliberate "stop AP during update" behaviour carried
 * over from the reference firmware.
 */
#ifndef DB_OTA_H
#define DB_OTA_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kick off an app OTA from `url` in a background task. Plain HTTP and HTTPS are
 * both accepted (HTTPS is verified against the ESP-IDF certificate bundle).
 * Returns ESP_OK if the task was created — NOT that the update succeeded; the
 * device reboots either way. ESP_ERR_INVALID_STATE if another update is running
 * or the home Wi-Fi is down. */
esp_err_t db_ota_start(const char *url);

/* Web-UI OTA: fetch a raw `storage.bin` SPIFFS image from `url` and write it to
 * the `storage` partition in a background task, then reboot to remount it. The
 * SPIFFS mount is dropped before the erase; on any failure the device still
 * reboots (a failure after the erase leaves the UI blank until a good image is
 * pushed — the app itself is untouched and the REST API keeps working). Shares
 * the in-progress guard with db_ota_start(). */
esp_err_t db_ota_webui_from_url(const char *url);

/* ---- browser-upload OTA (streamed, caller-driven) --------------------------
 *
 * The HTTP layer streams a raw .bin POSTed by the browser straight into flash:
 *   begin(kind, len) -> write(chunk)* -> finish()   (then the caller reboots)
 * Nothing is buffered in RAM, so image size is bounded by the partition, not by
 * the heap. Shares the single in-progress guard with the URL OTAs above. All
 * calls must come from the same task (the httpd worker). On any error the caller
 * must call db_ota_upload_abort(); if that returns true the `storage` partition
 * was already erased and the device should reboot anyway. */
typedef enum {
    DB_OTA_UPLOAD_APP,    /* app image -> inactive OTA slot (rollback-safe)   */
    DB_OTA_UPLOAD_WEBUI,  /* raw SPIFFS image -> `storage` (web UI) partition */
} db_ota_upload_kind_t;

/* Open a session for an image of exactly `image_len` bytes. Validates the size
 * against the target partition, then prepares it (app: esp_ota_begin; web UI:
 * unmount + full erase). On error returns a code and points *errmsg at a static
 * human-readable reason suitable for showing in the UI. */
esp_err_t db_ota_upload_begin(db_ota_upload_kind_t kind, size_t image_len,
                              const char **errmsg);

/* Append the next chunk. The total written may never exceed image_len. */
esp_err_t db_ota_upload_write(const void *data, size_t len, const char **errmsg);

/* All bytes received: validate and activate. For an app image this runs the
 * esp_ota_end() image checks and sets the boot partition; the caller then
 * reboots (rollback still protects the first boot). For the web UI there is
 * nothing to finalize — the caller reboots to remount. */
esp_err_t db_ota_upload_finish(const char **errmsg);

/* Tear down a failed session and release the guard. Returns true when the device
 * must reboot anyway (a web-UI upload aborted after the erase). */
bool db_ota_upload_abort(void);

/* Call once at boot: mark the running image valid so app-rollback keeps it.
 * Skipping this on a freshly flashed OTA image means the NEXT reboot reverts. */
void db_ota_mark_valid(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_OTA_H */
