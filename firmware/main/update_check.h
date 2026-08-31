/*
 * update_check.h - "is there a newer release?" (see update_check.c).
 *
 * ota.h can already install an image from a URL. What it cannot do is tell a
 * user that an image EXISTS: every OTA path here starts with someone pasting a
 * link, which means a box quietly runs an old firmware until its owner happens
 * to visit a release page. This module closes that gap and nothing more — it
 * ASKS GitHub what the newest release is, compares it to the running version,
 * and hands the discovered asset URLs straight to db_ota_start() /
 * db_ota_webui_from_url(). It never writes flash itself.
 *
 * THREE PROPERTIES ARE LOAD-BEARING, and each exists because the obvious
 * implementation is broken in a way that only shows up on the device:
 *
 *   1. THE CHECK IS ASYNCHRONOUS. A TLS handshake plus a few tens of kilobytes
 *      over a domestic uplink is seconds, and httpd has a handful of worker
 *      threads. So db_update_check() only starts a task; the REST layer reads
 *      the cached result and reports `checking` while a fetch is in flight.
 *
 *   2. THE RESULT IS CACHED AND RATE-LIMITED. GitHub allows 60 unauthenticated
 *      requests per hour per IP. A UI that polled this endpoint the way it
 *      polls /api/events would exhaust that in under a minute and then get 403s
 *      for the rest of the hour — for every box behind the same NAT. A check is
 *      therefore refused (and the cached answer served) unless the last one is
 *      older than DB_UPDATE_MIN_INTERVAL_S, or `force` is set.
 *
 *   3. VERSIONS ARE COMPARED NUMERICALLY, NEVER WITH strcmp(). "0.10.0" sorts
 *      BEFORE "0.9.0" as text, so a string compare would hide exactly the
 *      release a user most wants. See ver_cmp() in update_check.c.
 *
 * The check needs the STA (home Wi-Fi) up. On a box that only has its own
 * softAP the status carries a plain-language reason instead of an empty result.
 */
#ifndef DB_UPDATE_CHECK_H
#define DB_UPDATE_CHECK_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE ONE PLACE A FORK CHANGES.
 *
 * Everything this module asks GitHub for is derived from this slug, and so are
 * the two DEFAULT MANUAL UPDATE URLS below. It lives in the header rather than
 * in update_check.c because db_config.c seeds the stored OTA URL from it and
 * http_api.c hands both defaults to the web UI, and a second copy of a repo name
 * in either of those is a fork that half-works.
 *
 * The defaults point at `releases/latest/download/<asset>`, which GitHub
 * redirects to the newest release's asset — so a box updated by URL follows the
 * stable line without anyone editing a version into a text field.
 *
 * NOTE WHAT THESE ARE NOT FOR. The automatic check (GET /api/update, POST
 * /api/update/install) never touches them: it reads the exact
 * browser_download_url out of the release it just fetched. These exist purely so
 * the MANUAL "update from a URL" path has something sensible already typed in.
 */
#define DB_UPDATE_REPO_SLUG   "MarvAmBass/klingelbox"
/* The asset names the release workflow publishes. Named once, used both to find
 * the asset inside a release document and to build the URLs below. */
#define DB_UPDATE_ASSET_APP   "klingelbox.bin"
#define DB_UPDATE_ASSET_WEBUI "storage.bin"
#define DB_UPDATE_RELEASE_URL "https://github.com/" DB_UPDATE_REPO_SLUG "/releases/latest/download/"
#define DB_UPDATE_APP_URL     DB_UPDATE_RELEASE_URL DB_UPDATE_ASSET_APP
#define DB_UPDATE_WEBUI_URL   DB_UPDATE_RELEASE_URL DB_UPDATE_ASSET_WEBUI

/* Field sizes are part of the API: the REST layer copies this struct out. */
#define DB_UPDATE_VER_MAX   32    /* "v0.10.0-rc1" and then some            */
#define DB_UPDATE_HTML_MAX  160   /* the release page                       */
#define DB_UPDATE_URL_MAX   200   /* a browser_download_url of an asset     */
#define DB_UPDATE_ERR_MAX   96    /* one human sentence, "" when fine       */

/* Do not re-query GitHub more often than this unless `force` is set. */
#define DB_UPDATE_MIN_INTERVAL_S  (6 * 3600)
/* Even a forced check has a floor, so a stuck UI cannot burn the hourly quota. */
#define DB_UPDATE_FORCE_FLOOR_S   60

typedef struct {
    bool     valid;             /* a check has COMPLETED at least once        */
    bool     checking;          /* a fetch is in flight right now             */
    bool     update_available;  /* latest is semantically newer than current  */
    char     current[DB_UPDATE_VER_MAX];   /* esp_app_get_description()       */
    char     latest[DB_UPDATE_VER_MAX];    /* the tag, e.g. "v0.2.0"          */
    char     published_at[DB_UPDATE_VER_MAX]; /* ISO-8601, "" if absent       */
    char     html_url[DB_UPDATE_HTML_MAX]; /* the release page, for a link    */
    char     app_url[DB_UPDATE_URL_MAX];   /* asset "klingelbox.bin", "" none */
    char     webui_url[DB_UPDATE_URL_MAX]; /* asset "storage.bin", "" if none */
    char     error[DB_UPDATE_ERR_MAX];     /* "" when the last check was fine */
    int64_t  checked_at_us;     /* esp_timer_get_time() of the last COMPLETED
                                   check; 0 = never                           */
} db_update_status_t;

/* Call once at boot, before the HTTP server. Touches no network: it only
 * records the running version and creates the lock. */
void db_update_init(void);

/* Start an asynchronous check. Returns immediately.
 *   ESP_OK                 a fetch was started, OR a fresh cached result is
 *                          already available / one is already in flight.
 *   ESP_ERR_INVALID_STATE  the STA is down (the status carries the reason).
 *   ESP_ERR_NO_MEM         the checker task could not be created.
 * `force` skips the DB_UPDATE_MIN_INTERVAL_S window but not the floor. */
esp_err_t db_update_check(bool force);

/* Snapshot of the cached result. Never blocks on the network. */
void db_update_get(db_update_status_t *out);

/*
 * Hand the discovered asset off to ota.c and return — the OTA runs in its own
 * task and reboots the box.
 *
 * `webui` picks WHICH image, it does not add a second one: ota.c has a single
 * in-progress guard and every path there ends in a reboot, so the app and the
 * web UI can never be flashed in one call. false installs klingelbox.bin (the
 * app), true installs storage.bin (the web UI) — which is the natural second
 * step once the box is back up, and matches the existing "an app OTA leaves the
 * old UI in place" behaviour ota.h documents.
 *
 * On failure returns a code and points *errmsg at a static reason for the UI:
 *   ESP_ERR_INVALID_STATE  no completed check, no update, or an OTA is running
 *   ESP_ERR_NOT_FOUND      the release carries no asset of that kind
 */
esp_err_t db_update_install(bool webui, const char **errmsg);

#ifdef __cplusplus
}
#endif

#endif /* DB_UPDATE_CHECK_H */
