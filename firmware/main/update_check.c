/*
 * update_check.c - see update_check.h.
 *
 * WHY THERE IS A HAND-WRITTEN JSON SCANNER IN HERE.
 *
 * The obvious implementation is: read the body into one buffer, cJSON_Parse it,
 * pick five fields, free it. On a desktop that is three lines. On this box it is
 * a memory bomb. GitHub's /releases/latest document is TENS OF KILOBYTES —
 * almost all of it the release notes (`body`, which for this project is a full
 * changelog) plus a fat author/uploader object repeated for every asset. cJSON
 * would need the raw text AND the whole node tree resident at once, on a heap
 * that is simultaneously carrying Wi-Fi buffers, an open TLS session (mbedTLS
 * alone wants ~40 KB for the handshake) and the SPIFFS-backed web UI. That is
 * how a feature nobody uses often turns into an out-of-memory reboot.
 *
 * So the body is never buffered. It is read in GH_CHUNK-sized pieces straight
 * into an incremental, byte-at-a-time scanner (gh_feed) that:
 *
 *   - keeps at most one JSON token at a time, in a bounded buffer. A token
 *     longer than GH_TOK_MAX is truncated and DISCARDED — which is exactly what
 *     happens to the enormous `body` field, at a cost of 208 bytes.
 *   - tracks object/array depth, so `html_url` is read from the release and not
 *     from the author object, and `name` from an asset and not from its
 *     uploader.
 *   - STOPS READING as soon as the assets array has closed and a tag has been
 *     seen. GitHub emits `tag_name` before `assets`, and `body` after it, so
 *     the single largest field is usually never transferred at all.
 *   - and, as a backstop against a pathological or hostile response, refuses to
 *     read more than GH_MAX_BODY in total.
 *
 * Peak extra memory for a check is therefore one gh_parse_t (~800 B) plus a
 * GH_CHUNK read buffer, both on the heap, plus whatever esp_http_client and
 * mbedTLS need for the connection itself.
 *
 * THE USER-AGENT IS NOT OPTIONAL. api.github.com answers a request without one
 * with 403 and a JSON explanation — which looks, from the device, exactly like
 * a rate limit or a broken uplink. It is the classic silent failure of this
 * integration, so the header is set unconditionally and its absence can never
 * be introduced by a config change.
 *
 * REPORTING. No new db_diag_t value is invented here (that enum is owned by
 * db_diag.h and enumerated wholesale by GET /api/diagnostics); an update check
 * is a system-level event, so results land in the event ring as DB_EV_SYSTEM
 * where they sit next to boot and radio messages in the UI's activity feed.
 */
#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "event_log.h"
#include "ota.h"
#include "version_cmp.h"
#include "wifi_mgr.h"

static const char *TAG = "db_update";

/* The slug itself lives in update_check.h — db_config.c and http_api.c derive
 * the default manual-update URLs from the same one, so there is exactly one
 * repo name in the firmware for a fork to change. */
#define GH_REPO_SLUG    DB_UPDATE_REPO_SLUG
#define GH_LATEST_URL   "https://api.github.com/repos/" GH_REPO_SLUG "/releases/latest"

/* GitHub 403s a request with no User-Agent. See the file header. */
#define GH_USER_AGENT   "klingelbox-esp32 (+https://github.com/" GH_REPO_SLUG ")"
#define GH_ACCEPT       "application/vnd.github+json"
#define GH_API_VERSION  "2022-11-28"

/* The asset names the release workflow publishes (update_check.h, which also
 * builds the default manual-update URLs out of them). */
#define GH_ASSET_APP    DB_UPDATE_ASSET_APP
#define GH_ASSET_WEBUI  DB_UPDATE_ASSET_WEBUI

#define GH_CHUNK        512      /* bytes pulled from the socket at a time     */
#define GH_MAX_BODY     (128 * 1024)  /* hard ceiling on what we will read     */
#define GH_TOK_MAX      208      /* longest token kept: an asset download URL  */
#define GH_KEY_MAX      32
#define GH_TIMEOUT_MS   15000
#define GH_MAX_REDIRECT 4

/* ------------------------------------------------------------------ state */

static SemaphoreHandle_t s_lock;
static db_update_status_t s_st;     /* guarded by s_lock */
static volatile bool s_busy;        /* a checker task exists */

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* Version parsing and ordering live in version_cmp.h (header-only so the host
 * test suite compiles the same code): numeric component compare, and a
 * prerelease sorts BEFORE its final release — both directions of that rule
 * matter, see the header. */

/* ------------------------------------------------------------------ scanner */

typedef struct {
    /* lexer */
    bool   in_str, esc, pending;  /* pending: a string closed, key-or-value unknown */
    int    depth;                 /* '{' nesting; the release object is depth 1 */
    int    arr;                   /* '[' nesting */
    int    assets_arr;            /* arr depth of assets[]; 0 = not inside it */
    int    asset_depth;           /* depth of the asset object being read; 0 = none */
    char   tok[GH_TOK_MAX];
    size_t tok_len;
    bool   tok_trunc;
    char   key[GH_KEY_MAX];

    /* the asset under construction */
    char   a_name[48];
    char   a_url[DB_UPDATE_URL_MAX];

    /* results */
    char   tag[DB_UPDATE_VER_MAX];
    char   published[DB_UPDATE_VER_MAX];
    char   html[DB_UPDATE_HTML_MAX];
    char   app_url[DB_UPDATE_URL_MAX];
    char   webui_url[DB_UPDATE_URL_MAX];
    bool   assets_done;
} gh_parse_t;

static void tok_push(gh_parse_t *p, char c)
{
    if (p->tok_len + 1 >= sizeof(p->tok)) { p->tok_trunc = true; return; }
    p->tok[p->tok_len++] = c;
}

/* A completed string VALUE, dispatched against the key that introduced it. */
static void gh_value(gh_parse_t *p)
{
    p->tok[p->tok_len] = '\0';
    if (p->tok_trunc) return;      /* the release notes and nothing we want */

    /* Inside an asset object — and only DIRECTLY inside it, so the uploader
     * sub-object cannot contribute a `name`. */
    if (p->asset_depth && p->depth == p->asset_depth) {
        if (strcmp(p->key, "name") == 0)
            strlcpy(p->a_name, p->tok, sizeof(p->a_name));
        else if (strcmp(p->key, "browser_download_url") == 0)
            strlcpy(p->a_url, p->tok, sizeof(p->a_url));
        return;
    }
    /* Top level only: `author` carries an html_url too, and taking it would
     * link the UI at a GitHub profile instead of the release. */
    if (p->depth != 1) return;
    if (strcmp(p->key, "tag_name") == 0)
        strlcpy(p->tag, p->tok, sizeof(p->tag));
    else if (strcmp(p->key, "html_url") == 0)
        strlcpy(p->html, p->tok, sizeof(p->html));
    else if (strcmp(p->key, "published_at") == 0)
        strlcpy(p->published, p->tok, sizeof(p->published));
}

static void gh_asset_end(gh_parse_t *p)
{
    if (p->a_url[0]) {
        if (strcmp(p->a_name, GH_ASSET_APP) == 0)
            strlcpy(p->app_url, p->a_url, sizeof(p->app_url));
        else if (strcmp(p->a_name, GH_ASSET_WEBUI) == 0)
            strlcpy(p->webui_url, p->a_url, sizeof(p->webui_url));
    }
    p->a_name[0] = '\0';
    p->a_url[0] = '\0';
}

/* Everything we need has been seen; the rest of the document is `body`. */
static bool gh_complete(const gh_parse_t *p)
{
    return p->assets_done && p->tag[0] != '\0';
}

/*
 * Feed one chunk. Deliberately stateless with respect to chunk boundaries: every
 * decision is made from the persistent struct, so a token, a key or an object
 * may straddle any number of reads. Returns true when parsing may stop.
 */
static bool gh_feed(gh_parse_t *p, const char *buf, int len)
{
    for (int i = 0; i < len; i++) {
        char c = buf[i];

        if (p->in_str) {
            if (p->esc) {
                /* \" \\ \/ become the literal character. A \uXXXX leaves its
                 * hex digits in the token, which is harmless: no field we keep
                 * can legally contain an escape. */
                p->esc = false;
                tok_push(p, c);
            } else if (c == '\\') {
                p->esc = true;
            } else if (c == '"') {
                p->in_str = false;
                p->pending = true;   /* key or value? the next char decides */
            } else {
                tok_push(p, c);
            }
            continue;
        }

        if (p->pending) {
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
            if (c == ':') {                       /* it was a key */
                p->tok[p->tok_len] = '\0';
                strlcpy(p->key, p->tok_trunc ? "" : p->tok, sizeof(p->key));
                p->pending = false;
                p->tok_len = 0;
                p->tok_trunc = false;
                continue;
            }
            gh_value(p);                          /* it was a value */
            p->pending = false;
            p->tok_len = 0;
            p->tok_trunc = false;
            p->key[0] = '\0';
            /* fall through: `c` is structural and still has to be handled */
        }

        switch (c) {
        case '"':
            p->tok_len = 0;
            p->tok_trunc = false;
            p->in_str = true;
            break;
        case '{':
            p->depth++;
            if (p->assets_arr && p->arr == p->assets_arr && !p->asset_depth)
                p->asset_depth = p->depth;
            p->key[0] = '\0';
            break;
        case '}':
            if (p->asset_depth == p->depth) { gh_asset_end(p); p->asset_depth = 0; }
            p->depth--;
            p->key[0] = '\0';
            break;
        case '[':
            p->arr++;
            if (!p->assets_arr && p->depth == 1 && strcmp(p->key, "assets") == 0)
                p->assets_arr = p->arr;
            p->key[0] = '\0';
            break;
        case ']':
            if (p->assets_arr && p->assets_arr == p->arr) {
                p->assets_arr = 0;
                p->assets_done = true;
            }
            p->arr--;
            p->key[0] = '\0';
            break;
        case ',':
            p->key[0] = '\0';
            break;
        default:
            break;      /* numbers, true/false/null: nothing we read is one */
        }

        if (gh_complete(p)) return true;
    }
    return gh_complete(p);
}

/* ------------------------------------------------------------------ fetch */

/* Fill *p from the GitHub API. On failure writes a human sentence into `err`. */
static bool gh_fetch(gh_parse_t *p, char *err, size_t errsz)
{
    esp_http_client_config_t cfg = {
        .url = GH_LATEST_URL,
        .method = HTTP_METHOD_GET,
        .timeout_ms = GH_TIMEOUT_MS,
        .keep_alive_enable = false,
        /* The response headers alone (rate-limit, caching, tracing) run past
         * the 512-byte default and a short header buffer aborts the request. */
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        .crt_bundle_attach = esp_crt_bundle_attach,
#endif
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { strlcpy(err, "could not create the HTTPS client", errsz); return false; }

    esp_http_client_set_header(cli, "User-Agent", GH_USER_AGENT);
    esp_http_client_set_header(cli, "Accept", GH_ACCEPT);
    esp_http_client_set_header(cli, "X-GitHub-Api-Version", GH_API_VERSION);

    char *buf = NULL;
    bool ok = false;
    int status = 0;

    /* Open and follow redirects by hand: open/fetch_headers does not
     * auto-follow, and the API host has moved paths before. */
    for (int redirects = 0; ; redirects++) {
        if (esp_http_client_open(cli, 0) != ESP_OK) {
            strlcpy(err, "could not reach api.github.com", errsz);
            goto done;
        }
        esp_http_client_fetch_headers(cli);
        status = esp_http_client_get_status_code(cli);
        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirects >= GH_MAX_REDIRECT) {
                strlcpy(err, "too many HTTP redirects from GitHub", errsz);
                goto done;
            }
            esp_http_client_set_redirection(cli);
            esp_http_client_close(cli);
            continue;
        }
        break;
    }

    if (status == 403 || status == 429) {
        /* Almost always the 60-requests-per-hour unauthenticated quota, shared
         * by every device behind this NAT — hence the caching in db_update_check. */
        strlcpy(err, "GitHub is rate-limiting this network — try again later", errsz);
        goto done;
    }
    if (status == 404) {
        strlcpy(err, "no published release for " GH_REPO_SLUG " yet", errsz);
        goto done;
    }
    if (status != 200) {
        snprintf(err, errsz, "GitHub answered HTTP %d", status);
        goto done;
    }

    buf = malloc(GH_CHUNK);
    if (!buf) { strlcpy(err, "out of memory for the download buffer", errsz); goto done; }

    /* esp_http_client_read returns SHORT READS routinely (one TLS record, one
     * chunked-transfer piece), so this loops on the return value rather than
     * assuming a full buffer, and never treats a short read as the end. */
    int total = 0;
    while (total < GH_MAX_BODY) {
        int n = esp_http_client_read(cli, buf, GH_CHUNK);
        if (n < 0) { strlcpy(err, "the connection dropped mid-response", errsz); goto done; }
        if (n == 0) break;              /* end of body (or the peer went away) */
        total += n;
        if (gh_feed(p, buf, n)) break;  /* everything we need; skip the rest */
    }

    if (!p->tag[0]) {
        strlcpy(err, "GitHub's answer carried no release tag", errsz);
        goto done;
    }
    ok = true;

done:
    free(buf);
    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);
    return ok;
}

/* ------------------------------------------------------------------ the task */

static void update_task(void *arg)
{
    (void)arg;
    gh_parse_t *p = calloc(1, sizeof(*p));
    char err[DB_UPDATE_ERR_MAX] = "";

    if (!p) {
        strlcpy(err, "out of memory for the update check", sizeof(err));
    } else if (gh_fetch(p, err, sizeof(err))) {
        db_semver_t cur, latest;
        char current[DB_UPDATE_VER_MAX];
        strlcpy(current, s_st.current, sizeof(current));

        if (!db_ver_parse(p->tag, &latest)) {
            snprintf(err, sizeof(err), "unreadable release tag \"%s\"", p->tag);
        } else if (!db_ver_parse(current, &cur)) {
            snprintf(err, sizeof(err), "unreadable running version \"%s\"", current);
        } else {
            bool newer = db_ver_cmp(&latest, &cur) > 0;
            lock();
            strlcpy(s_st.latest, p->tag, sizeof(s_st.latest));
            strlcpy(s_st.published_at, p->published, sizeof(s_st.published_at));
            strlcpy(s_st.html_url, p->html, sizeof(s_st.html_url));
            strlcpy(s_st.app_url, p->app_url, sizeof(s_st.app_url));
            strlcpy(s_st.webui_url, p->webui_url, sizeof(s_st.webui_url));
            s_st.update_available = newer;
            s_st.error[0] = '\0';
            s_st.valid = true;
            s_st.checked_at_us = esp_timer_get_time();
            unlock();

            if (newer) {
                ESP_LOGW(TAG, "update available: %s (running %s)", p->tag, current);
                db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0,
                               "update %s available (running %s)", p->tag, current);
            } else {
                ESP_LOGI(TAG, "up to date: running %s, latest %s", current, p->tag);
            }
            free(p);
            s_busy = false;
            vTaskDelete(NULL);
            return;
        }
    }

    /* Every failure path lands here: record it, but keep any PREVIOUS good
     * result visible — "the last check failed" is more useful than an answer
     * that silently reverts to "unknown" on one flaky DNS lookup. */
    lock();
    strlcpy(s_st.error, err, sizeof(s_st.error));
    s_st.checked_at_us = esp_timer_get_time();
    unlock();
    ESP_LOGE(TAG, "update check failed: %s", err);
    db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0, "update check failed: %s", err);

    free(p);
    s_busy = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ public */

void db_update_init(void)
{
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    memset(&s_st, 0, sizeof(s_st));
    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(s_st.current, app ? app->version : "?", sizeof(s_st.current));
    ESP_LOGI(TAG, "update check ready (running %s, watching " GH_REPO_SLUG ")",
             s_st.current);
}

esp_err_t db_update_check(bool force)
{
    if (!s_lock) db_update_init();

    if (!db_wifi_sta_connected()) {
        lock();
        strlcpy(s_st.error,
                "not on the home Wi-Fi — an update check needs internet access",
                sizeof(s_st.error));
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_busy) return ESP_OK;      /* already in flight; the caller polls */

    /* The rate-limit policy of update_check.h, in one place. */
    lock();
    int64_t age_us = s_st.checked_at_us ? (esp_timer_get_time() - s_st.checked_at_us) : -1;
    unlock();
    if (age_us >= 0) {
        int64_t min_us = (force ? DB_UPDATE_FORCE_FLOOR_S : DB_UPDATE_MIN_INTERVAL_S)
                         * 1000000LL;
        if (age_us < min_us) return ESP_OK;   /* serve the cache */
    }

    s_busy = true;
    lock();
    s_st.checking = true;
    unlock();
    /* 8 KB: the mbedTLS handshake is the expensive frame here; the scanner
     * itself keeps everything on the heap. Priority below ota.c's task. */
    if (xTaskCreate(update_task, "db_update", 8192, NULL, 4, NULL) != pdPASS) {
        s_busy = false;
        lock();
        s_st.checking = false;
        strlcpy(s_st.error, "could not start the update checker task",
                sizeof(s_st.error));
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void db_update_get(db_update_status_t *out)
{
    if (!out) return;
    if (!s_lock) { memset(out, 0, sizeof(*out)); return; }
    lock();
    /* `checking` is derived from the task's own liveness rather than stored, so
     * a task that died between setting the flag and clearing it can never leave
     * the UI spinning forever. */
    s_st.checking = s_busy;
    *out = s_st;
    unlock();
}

esp_err_t db_update_install(bool webui, const char **errmsg)
{
    static const char *E_NOCHECK  = "no update check has completed yet";
    static const char *E_NOUPDATE = "this box already runs the newest release";
    static const char *E_NOASSET_APP =
        "the release carries no " GH_ASSET_APP " asset";
    static const char *E_NOASSET_UI =
        "the release carries no " GH_ASSET_WEBUI " asset";
    static const char *E_BUSY =
        "another update is already running, or the home network is down";
    static const char *E_STA = "the home Wi-Fi is down — the box cannot download";

    if (errmsg) *errmsg = NULL;

    db_update_status_t st;
    db_update_get(&st);
    if (!st.valid)            { if (errmsg) *errmsg = E_NOCHECK;  return ESP_ERR_INVALID_STATE; }
    if (!st.update_available) { if (errmsg) *errmsg = E_NOUPDATE; return ESP_ERR_INVALID_STATE; }
    if (!db_wifi_sta_connected()) { if (errmsg) *errmsg = E_STA;  return ESP_ERR_INVALID_STATE; }

    const char *url = webui ? st.webui_url : st.app_url;
    if (!url[0]) {
        if (errmsg) *errmsg = webui ? E_NOASSET_UI : E_NOASSET_APP;
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGW(TAG, "installing %s of %s from %s",
             webui ? "the web UI" : "the firmware", st.latest, url);
    db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0, "installing %s %s",
                   webui ? "web UI" : "firmware", st.latest);

    esp_err_t err = webui ? db_ota_webui_from_url(url) : db_ota_start(url);
    if (err != ESP_OK) {
        if (errmsg) *errmsg = E_BUSY;
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}
