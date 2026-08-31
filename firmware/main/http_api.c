/*
 * http_api.c - Web UI (SPIFFS) + the REST API of docs/API.md.
 *
 * docs/API.md is the contract, not a summary written after the fact: the web UI
 * and this file are both written against it and neither may invent an endpoint.
 * Every route below appears there, with the same shape.
 *
 *   GET  /api/system                     firmware + network identity + radio ident
 *   POST /api/system/hostname            {"hostname":"..."} (applies on reboot)
 *   POST /api/restart                    reboot
 *   GET  /api/radio    POST /api/radio   live radio parameters + transmit policy
 *   GET  /api/diagnostics                every db_diag state + capture counters
 *   GET  /api/signals                    stored signal metadata
 *   GET  /api/signals/<id>               ... plus the raw waveform (streamed)
 *   POST /api/signals/<id>               {"name":"..."}
 *   DEL  /api/signals/<id>
 *   POST /api/signals/<id>/transmit      {"repeats":6,"gap_us":8000}
 *   POST /api/signals/virtual            synthesize an EV1527 signal
 *   GET  /api/learn                      armed state + pending candidate
 *   POST /api/learn/arm|cancel|accept
 *   GET  /api/graph                      nodes + links
 *   POST /api/graph/nodes                create; POST /api/graph/nodes/<id> update
 *   DEL  /api/graph/nodes/<id>           delete (drops its links)
 *   POST /api/graph/nodes/<id>/fire      test-fire
 *   POST|DEL /api/graph/links            {"from":1,"to":2}
 *   GET  /api/gpio/available             pins offerable for a wired button
 *   GET  /api/events?since=<serial>      recent activity, newest first
 *   GET  /api/config   POST /api/config  non-secret configuration
 *   GET  /api/ap       POST /api/ap      softAP + recovery portal settings
 *   GET  /api/wifi/scan                  visible networks (first-run wizard)
 *   POST /api/wifi                       the wizard's save; reboots
 *   POST /api/ota | /api/ota/webui       update from a URL
 *   POST /api/ota/upload | /api/ota/webui/upload   raw .bin in the body
 *   GET  /api/update                     cached "is there a newer release?"
 *   POST /api/update/check               {"force":true} — async re-query
 *   POST /api/update/install             {"webui":true} — OTA from the release
 *
 * FOUR HANDLERS, NOT FORTY. ESP-IDF's httpd matches literal URIs unless
 * `httpd_uri_match_wildcard` is enabled, and it has no path-parameter support at
 * all. Registering one wildcard /api route per method and dispatching inside a
 * single function per verb is both cheaper (one handler slot instead of one per
 * endpoint) and
 * the only way to get `/api/signals/<id>` at all — ids are parsed here, by hand,
 * from req->uri.
 *
 * SECRETS ARE WRITE-ONLY. No password — Wi-Fi, softAP, recovery portal or MQTT —
 * is ever serialized into a response. The API reports `has_pass` /
 * `has_recovery_pass` booleans instead, and an empty string in a POST means
 * "leave the stored value alone" so a UI can round-trip a form it never received
 * the secret for. This is the single rule most easily broken by a well-meant
 * "just add the field so the form can prefill" change: do not.
 *
 * DIFFERENCE FROM THE REFERENCE FIRMWARE (deliberate, PLAN.md §12): there is no
 * per-request origin gate. That box refused management over its softAP; this one
 * must be reachable identically on the softAP and the LAN, because the softAP is
 * frequently the only network a doorbell in a hallway ever sees.
 *
 * BLOCKING. httpd worker threads are a scarce resource, so handlers must not
 * loiter. Two of them deliberately do: a transmit keys the radio for roughly
 * 200 ms (see api_signal_transmit) and a Wi-Fi scan takes a second or two. Both
 * are user-initiated, one-at-a-time actions with no sensible asynchronous
 * alternative that the UI could poll more cheaply than just waiting.
 */
#include "http_api.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "db_config.h"
#include "db_diag.h"
#include "db_mqtt.h"
#include "event_log.h"
#include "node_graph.h"
#include "ota.h"
#include "rf_capture.h"
#include "rf_decode.h"
#include "rf_frame.h"
#include "rf_service.h"
#include "signal_store.h"
#include "update_check.h"
#include "wifi_mgr.h"

#define LE_HOSTNAME_CMP_MAX 64

static const char *TAG = "http_api";

static db_config_t *s_cfg;   /* live config, owned by app_main */

/* Request bodies are small JSON documents; the only large POST bodies are the
 * OTA uploads, which are streamed and never go through read_body(). */
#define BODY_MAX 4096

/* ------------------------------------------------------------------ helpers */

/* Serialize and send `root`, then FREE IT. Every response path funnels through
 * here precisely so no error branch can forget the cJSON_Delete. */
static esp_err_t send_json(httpd_req_t *req, cJSON *root, const char *status)
{
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_status(req, status ? status : "200 OK");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, body ? body : "{}");
    cJSON_free(body);
    cJSON_Delete(root);
    return ESP_OK;
}

/* The uniform failure envelope of API.md: {"error": "..."} plus a real status. */
static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "error", msg);
    return send_json(req, e, status);
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o, "200 OK");
}

/* Map a domain error onto the status codes API.md promises. */
static const char *status_for(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NOT_FOUND:     return "404 Not Found";
    case ESP_ERR_INVALID_ARG:   return "400 Bad Request";
    case ESP_ERR_INVALID_SIZE:  return "400 Bad Request";
    case ESP_ERR_NO_MEM:        return "409 Conflict";   /* store/graph is full */
    case ESP_ERR_INVALID_STATE: return "409 Conflict";
    default:                    return "500 Internal Server Error";
    }
}

static esp_err_t send_esp_err(httpd_req_t *req, esp_err_t err, const char *what)
{
    char msg[128];
    snprintf(msg, sizeof(msg), "%s: %s", what, esp_err_to_name(err));
    return send_error(req, status_for(err), msg);
}

/* Read the whole (bounded) request body into a NUL-terminated heap buffer. */
static char *read_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total < 0 || total > BODY_MAX) return NULL;
    char *buf = malloc((size_t)total + 1);
    if (!buf) return NULL;
    int off = 0, timeouts = 0;
    while (off < total) {
        int r = httpd_req_recv(req, buf + off, total - off);
        /* Bounded retries, not `continue` forever: a client that opens a
         * Content-Length and then stalls must not pin an httpd worker. */
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= 3) continue;
        if (r <= 0) { free(buf); return NULL; }
        timeouts = 0;
        off += r;
    }
    buf[total] = '\0';
    return buf;
}

/* Parse the body as JSON. An EMPTY body is accepted as an empty object, because
 * API.md documents several routes as taking `{}` and browsers happily POST
 * nothing at all. Returns NULL only for genuinely malformed input. */
static cJSON *read_json(httpd_req_t *req)
{
    if (req->content_len == 0) return cJSON_CreateObject();
    char *body = read_body(req);
    if (!body) return NULL;
    cJSON *j = cJSON_Parse(body);
    free(body);
    return j;
}

/* True when `uri` is exactly `path`, ignoring any query string. */
static bool uri_is(const char *uri, const char *path)
{
    size_t n = strlen(path);
    return strncmp(uri, path, n) == 0 && (uri[n] == '\0' || uri[n] == '?');
}

/* True when `uri` starts with `prefix` (used for the /<id> families). */
static bool uri_starts(const char *uri, const char *prefix)
{
    return strncmp(uri, prefix, strlen(prefix)) == 0;
}

/*
 * Parse "<prefix><id><tail>" out of a request URI — the path-parameter support
 * esp_http_server does not have. Returns 0 when no positive integer id follows
 * the prefix. `tail` receives the remainder with any query string removed, so a
 * caller can distinguish "/api/signals/3" from "/api/signals/3/transmit".
 */
static uint16_t path_id(const char *uri, const char *prefix, char *tail, size_t tailsz)
{
    if (tail && tailsz) tail[0] = '\0';
    size_t pl = strlen(prefix);
    if (strncmp(uri, prefix, pl) != 0) return 0;

    const char *p = uri + pl;
    unsigned long v = 0;
    int digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 6) { v = v * 10 + (unsigned)(*p++ - '0'); digits++; }
    if (!digits || v == 0 || v > 0xFFFF) return 0;

    if (tail && tailsz) {
        size_t i = 0;
        while (*p && *p != '?' && i + 1 < tailsz) tail[i++] = *p++;
        tail[i] = '\0';
    }
    return (uint16_t)v;
}

/* cJSON accessors that tolerate a missing/mistyped field. */
static bool json_str(const cJSON *j, const char *key, const char **out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsString(v)) return false;
    *out = v->valuestring;
    return true;
}

static bool json_num(const cJSON *j, const char *key, double *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsNumber(v)) return false;
    *out = v->valuedouble;
    return true;
}

static bool json_bool(const cJSON *j, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}

/* Clamp helper for every numeric field that reaches a driver. */
static long clampl(double v, long lo, long hi)
{
    long x = (long)v;
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Same as clampl but keeps fractional precision — window_s may be e.g. 1.5. */
static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void trim_copy(const char *in, char *out, size_t outsz)
{
    while (*in == ' ' || *in == '\t') in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' ||
                 in[n - 1] == '\r' || in[n - 1] == '\n')) n--;
    if (n >= outsz) n = outsz - 1;
    memcpy(out, in, n);
    out[n] = '\0';
}

/* ------------------------------------------------------------------ system */

static const char *modulation_str(uint8_t m)
{
    switch (m) {
    case CC1101_MOD_ASK_OOK: return "ook";
    case CC1101_MOD_2FSK:    return "2fsk";
    case CC1101_MOD_GFSK:    return "gfsk";
    case CC1101_MOD_4FSK:    return "4fsk";
    case CC1101_MOD_MSK:     return "msk";
    default:                 return "unknown";
    }
}

static bool modulation_from_str(const char *s, cc1101_modulation_t *out)
{
    if (!strcasecmp(s, "ook") || !strcasecmp(s, "ask") || !strcasecmp(s, "ask_ook"))
        *out = CC1101_MOD_ASK_OOK;
    else if (!strcasecmp(s, "2fsk")) *out = CC1101_MOD_2FSK;
    else if (!strcasecmp(s, "gfsk")) *out = CC1101_MOD_GFSK;
    else if (!strcasecmp(s, "4fsk")) *out = CC1101_MOD_4FSK;
    else if (!strcasecmp(s, "msk"))  *out = CC1101_MOD_MSK;
    else return false;
    return true;
}

static esp_err_t api_system_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddStringToObject(root, "idf", app->idf_ver);
    cJSON_AddStringToObject(root, "hostname", s_cfg->hostname);
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(root, "partition", run ? run->label : "?");

    /* The UI swaps the dashboard for the first-run wizard on "recovery", so this
     * string is a contract owned by wifi_mgr — never reworded here. */
    cJSON_AddStringToObject(root, "wifi_mode", db_wifi_mode_str());

    char ip[16];
    db_wifi_sta_ip(ip);
    cJSON_AddBoolToObject(root, "sta_connected", db_wifi_sta_connected());
    cJSON_AddStringToObject(root, "sta_ip", ip);
    int slot = db_wifi_active_sta_slot();
    cJSON_AddStringToObject(root, "sta_ssid",
                            (slot >= 0 && slot < DB_STA_MAX) ? s_cfg->sta[slot].ssid : "");

    db_wifi_ap_ip(ip);
    cJSON_AddStringToObject(root, "ap_ip", ip);
    char ap_ssid[33];
    db_wifi_ap_ssid(ap_ssid);
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);

    cc1101_ident_t id = { 0 };
    rf_service_get_ident(&id);
    cJSON *radio = cJSON_AddObjectToObject(root, "radio");
    cJSON_AddBoolToObject(radio, "present", rf_service_radio_present());
    cJSON_AddNumberToObject(radio, "partnum", id.partnum);
    cJSON_AddNumberToObject(radio, "version", id.version);

    return send_json(req, root, "200 OK");
}

/* POST /api/system/hostname — one RFC-1123 label; applied by DHCP/mDNS at boot. */
static esp_err_t api_system_hostname(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    const char *raw = NULL;
    if (!json_str(j, "hostname", &raw)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "hostname must be a string");
    }
    char name[DB_STR_HOSTNAME + 16];
    trim_copy(raw, name, sizeof(name));
    cJSON_Delete(j);

    size_t n = strlen(name);
    if (!n) return send_error(req, "400 Bad Request", "hostname must not be empty");
    if (n > DB_STR_HOSTNAME - 1)
        return send_error(req, "400 Bad Request", "hostname is too long (max 31 characters)");
    for (size_t i = 0; i < n; i++) {
        char c = (char)tolower((unsigned char)name[i]);
        name[i] = c;
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok || (c == '-' && (i == 0 || i == n - 1)))
            return send_error(req, "400 Bad Request",
                              "hostname must be lowercase letters, digits and hyphens, "
                              "and may not start or end with a hyphen");
    }

    strlcpy(s_cfg->hostname, name, sizeof(s_cfg->hostname));
    db_config_save(s_cfg);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "hostname", name);
    cJSON_AddBoolToObject(o, "reboot_required", true);
    return send_json(req, o, "200 OK");
}

static esp_err_t api_restart(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "restarting", true);
    send_json(req, o, "200 OK");
    vTaskDelay(pdMS_TO_TICKS(300));   /* let httpd flush the reply first */
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ radio */

static esp_err_t api_radio_get(httpd_req_t *req)
{
    cc1101_radio_cfg_t rc;
    rf_service_get_radio_cfg(&rc);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "freq_hz", (double)rc.freq_hz);
    cJSON_AddStringToObject(o, "modulation", modulation_str((uint8_t)rc.modulation));
    cJSON_AddNumberToObject(o, "datarate_bps", (double)rc.datarate_bps);
    cJSON_AddNumberToObject(o, "bandwidth_hz", (double)rc.rx_bandwidth_hz);
    cJSON_AddNumberToObject(o, "tx_power_dbm", rc.tx_power_dbm);
    cJSON_AddNumberToObject(o, "tx_repeats", s_cfg->tx_repeats);
    cJSON_AddNumberToObject(o, "tx_gap_us", (double)s_cfg->tx_gap_us);

    /* RSSI is only meaningful while the radio is receiving; with no chip the
     * read fails and 0 is reported rather than a fabricated noise floor. */
    int rssi = 0;
    if (rf_service_rssi(&rssi) != ESP_OK) rssi = 0;
    cJSON_AddNumberToObject(o, "rssi_dbm", rssi);
    return send_json(req, o, "200 OK");
}

/* POST /api/radio — any subset of the GET shape; reconfigures the chip live and
 * persists, so the setting survives a reboot (PLAN.md §3.5: radio parameters are
 * configuration, never constants). */
static esp_err_t api_radio_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    cc1101_radio_cfg_t rc;
    rf_service_get_radio_cfg(&rc);
    bool radio_changed = false;
    double d;
    const char *s;

    if (json_num(j, "freq_hz", &d)) {
        if (d < 300000000.0 || d > 928000000.0) {
            cJSON_Delete(j);
            return send_error(req, "400 Bad Request",
                              "freq_hz must be inside the CC1101's 300-928 MHz range");
        }
        rc.freq_hz = (uint32_t)d;
        radio_changed = true;
    }
    if (json_str(j, "modulation", &s)) {
        cc1101_modulation_t m;
        if (!modulation_from_str(s, &m)) {
            cJSON_Delete(j);
            return send_error(req, "400 Bad Request",
                              "modulation must be one of ook, 2fsk, gfsk, 4fsk, msk");
        }
        rc.modulation = m;
        radio_changed = true;
    }
    if (json_num(j, "datarate_bps", &d)) {
        rc.datarate_bps = (uint32_t)clampl(d, 600, 250000);
        radio_changed = true;
    }
    if (json_num(j, "bandwidth_hz", &d)) {
        rc.rx_bandwidth_hz = (uint32_t)clampl(d, 58000, 812000);
        radio_changed = true;
    }
    if (json_num(j, "tx_power_dbm", &d)) {
        rc.tx_power_dbm = (int8_t)clampl(d, -30, 12);
        radio_changed = true;
    }
    /* The repeat policy is ours, not the chip's: it lives in config only. */
    if (json_num(j, "tx_repeats", &d)) s_cfg->tx_repeats = (uint8_t)clampl(d, 1, 32);
    if (json_num(j, "tx_gap_us", &d))  s_cfg->tx_gap_us = (uint32_t)clampl(d, 0, 200000);
    cJSON_Delete(j);

    if (radio_changed) {
        esp_err_t err = rf_service_set_radio_cfg(&rc);
        if (err != ESP_OK)
            return send_esp_err(req, err, "could not apply the radio configuration");
        s_cfg->radio_freq_hz      = rc.freq_hz;
        s_cfg->radio_modulation   = (uint8_t)rc.modulation;
        s_cfg->radio_datarate_bps = rc.datarate_bps;
        s_cfg->radio_bandwidth_hz = rc.rx_bandwidth_hz;
        s_cfg->radio_tx_power_dbm = rc.tx_power_dbm;
    }
    db_config_save(s_cfg);
    return api_radio_get(req);
}

/* ------------------------------------------------------------------ diagnostics */

/*
 * The hard requirement of PLAN.md §7: every failure mode is a distinct, named,
 * separately-reported state, so the UI can explain a fault instead of rendering
 * a dead page. Enumerating the enum (rather than hand-listing states here) means
 * a new diagnostic shows up in the API the moment db_diag.h gains it.
 */
static esp_err_t api_diagnostics(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *states = cJSON_AddArrayToObject(root, "states");

    for (int i = 0; i < DB_DIAG__COUNT; i++) {
        db_diag_entry_t e;
        db_diag_get((db_diag_t)i, &e);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", db_diag_name((db_diag_t)i));
        cJSON_AddStringToObject(o, "help", db_diag_help((db_diag_t)i));
        cJSON_AddNumberToObject(o, "count", (double)e.count);
        cJSON_AddNumberToObject(o, "last_us", (double)e.last_us);
        cJSON_AddStringToObject(o, "detail", e.detail);
        cJSON_AddItemToArray(states, o);
    }

    /* "lots of dropped_short with zero frames" is the signature of RF energy
     * with no valid pulse stream — the counters that make that visible. */
    rf_capture_stats_t st = { 0 };
    rf_capture_get_stats(&st);
    cJSON *cap = cJSON_AddObjectToObject(root, "capture");
    cJSON_AddNumberToObject(cap, "frames", (double)st.frames);
    cJSON_AddNumberToObject(cap, "dropped_short", (double)st.dropped_short);
    cJSON_AddNumberToObject(cap, "dropped_full", (double)st.dropped_full);
    cJSON_AddNumberToObject(cap, "overruns", (double)st.overruns);

    return send_json(req, root, "200 OK");
}

/* ------------------------------------------------------------------ signals */

static const char *origin_str(uint8_t origin)
{
    switch (origin) {
    case DB_ORIGIN_SYNTHESIZED: return "synthesized";
    case DB_ORIGIN_IMPORTED:    return "imported";
    default:                    return "captured";
    }
}

/*
 * The decoded summary line. rf_decoded_t carries its own `text`, but the
 * resident metadata does not keep it (80 bytes each is the whole point of the
 * storage split in signal_store.h), so it is rebuilt from the parts in the same
 * format the decoder emits: "EV1527 id=0xA685A btn=0x8".
 */
static void decoded_text(const db_signal_meta_t *m, char *out, size_t outsz)
{
    char proto[RF_PROTOCOL_NAME_MAX];
    strlcpy(proto, m->protocol, sizeof(proto));
    for (char *p = proto; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(out, outsz, "%s id=0x%05" PRIX32 " btn=0x%X",
             proto, m->decoded_id, m->decoded_button);
}

/* Add the `decoded` member: an object, or JSON null for an unknown protocol —
 * which API.md calls out as a normal, fully supported state, not an error. */
static void add_decoded(cJSON *o, const db_signal_meta_t *m)
{
    if (!m->decoded_valid) {
        cJSON_AddNullToObject(o, "decoded");
        return;
    }
    char text[RF_DECODE_TEXT_MAX + RF_PROTOCOL_NAME_MAX];
    decoded_text(m, text, sizeof(text));
    cJSON *d = cJSON_AddObjectToObject(o, "decoded");
    cJSON_AddStringToObject(d, "protocol", m->protocol);
    cJSON_AddNumberToObject(d, "id", (double)m->decoded_id);
    cJSON_AddNumberToObject(d, "button", m->decoded_button);
    cJSON_AddStringToObject(d, "text", text);
}

/* The metadata half of a signal. `last_seen_s` is on the SAME monotonic clock as
 * an event's `ts_s` (esp_timer, i.e. seconds since boot), so the UI can relate
 * the two directly; 0 means "not seen since boot". */
static cJSON *signal_json(const db_signal_meta_t *m)
{
    char fp[9];
    snprintf(fp, sizeof(fp), "%08" PRIx32, (uint32_t)m->fingerprint);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", m->id);
    cJSON_AddStringToObject(o, "name", m->name);
    cJSON_AddStringToObject(o, "origin", origin_str(m->origin));
    cJSON_AddNumberToObject(o, "created_at", (double)m->created_at);
    cJSON_AddStringToObject(o, "fingerprint", fp);
    cJSON_AddNumberToObject(o, "base_us", m->base_us);
    cJSON_AddNumberToObject(o, "confidence", m->confidence);
    cJSON_AddNumberToObject(o, "pulse_count", m->pulse_count);
    add_decoded(o, m);
    cJSON_AddNumberToObject(o, "last_seen_s", (double)(m->last_seen_us / 1000000));
    cJSON_AddNumberToObject(o, "seen_count", (double)m->seen_count);
    return o;
}

static esp_err_t api_signals_list(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "signals");
    const db_signal_meta_t *list = db_signals_list();
    int n = db_signals_count();
    for (int i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, signal_json(&list[i]));
    return send_json(req, root, "200 OK");
}

/*
 * GET /api/signals/<id> — metadata plus the full waveform.
 *
 * WHY THIS ONE IS STREAMED. A frame holds up to RF_FRAME_MAX_PULSES (512)
 * durations; rendered as JSON that is several kilobytes, and cJSON would need
 * the printed string, the number array AND a 512-node object tree live at once —
 * on a box whose free heap is also carrying Wi-Fi buffers and an open TLS-free
 * HTTP connection. So the metadata object is printed normally, its closing brace
 * is clipped off, and the durations are appended straight to the socket in
 * chunks from a small fixed buffer. Peak extra memory is one rf_frame_t (1 KB,
 * on the heap, not the httpd worker's stack) plus 128 bytes of text.
 */
#define DURATIONS_CHUNK 192   /* bytes of "65535," text flushed at a time */

static esp_err_t api_signal_detail(httpd_req_t *req, uint16_t id)
{
    const db_signal_meta_t *m = db_signals_get(id);
    if (!m) return send_error(req, "404 Not Found", "no such signal");

    rf_frame_t *frame = malloc(sizeof(*frame));
    if (!frame) return send_error(req, "500 Internal Server Error", "out of memory");

    esp_err_t err = db_signals_load_frame(id, frame);
    if (err != ESP_OK) {
        free(frame);
        return send_esp_err(req, err, "could not load the stored waveform");
    }

    cJSON *o = signal_json(m);
    char *head = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!head) { free(frame); return send_error(req, "500 Internal Server Error", "out of memory"); }

    size_t hl = strlen(head);
    if (hl && head[hl - 1] == '}') head[--hl] = '\0';   /* re-opened below */

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, head, hl);
    cJSON_free(head);

    char buf[DURATIONS_CHUNK + 16];
    int len = snprintf(buf, sizeof(buf), ",\"first_level\":%u,\"durations_us\":[",
                       frame->first_level);
    httpd_resp_send_chunk(req, buf, len);

    uint16_t count = frame->count;
    if (count > RF_FRAME_MAX_PULSES) count = RF_FRAME_MAX_PULSES;   /* defensive */
    len = 0;
    for (uint16_t i = 0; i < count; i++) {
        len += snprintf(buf + len, sizeof(buf) - (size_t)len, "%s%u",
                        i ? "," : "", frame->durations_us[i]);
        if (len >= DURATIONS_CHUNK) {
            httpd_resp_send_chunk(req, buf, len);
            len = 0;
        }
    }
    len += snprintf(buf + len, sizeof(buf) - (size_t)len, "]}");
    httpd_resp_send_chunk(req, buf, len);
    httpd_resp_send_chunk(req, NULL, 0);   /* end of the chunked response */

    free(frame);
    return ESP_OK;
}

static esp_err_t api_signal_rename(httpd_req_t *req, uint16_t id)
{
    if (!db_signals_get(id)) return send_error(req, "404 Not Found", "no such signal");

    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    const char *name = NULL;
    if (!json_str(j, "name", &name) || !name[0]) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "name must be a non-empty string");
    }
    char clean[DB_SIGNAL_NAME_MAX];
    trim_copy(name, clean, sizeof(clean));
    cJSON_Delete(j);
    if (!clean[0]) return send_error(req, "400 Bad Request", "name must be a non-empty string");

    esp_err_t err = db_signals_rename(id, clean);
    if (err == ESP_OK) db_mqtt_on_signals_changed();
    if (err != ESP_OK) return send_esp_err(req, err, "could not rename the signal");

    const db_signal_meta_t *m = db_signals_get(id);
    if (!m) return send_error(req, "404 Not Found", "no such signal");
    return send_json(req, signal_json(m), "200 OK");
}

static esp_err_t api_signal_delete(httpd_req_t *req, uint16_t id)
{
    if (!db_signals_get(id)) return send_error(req, "404 Not Found", "no such signal");
    esp_err_t err = db_signals_delete(id);
    if (err == ESP_OK) db_mqtt_on_signals_changed();
    if (err != ESP_OK) return send_esp_err(req, err, "could not delete the signal");
    return send_ok(req);
}

/*
 * POST /api/signals/<id>/transmit
 *
 * BLOCKS THE WORKER FOR ~200 ms, ON PURPOSE. Six repeats of a ~24 ms EV1527
 * frame separated by 8 ms of silence is roughly 190 ms of keyed carrier, and
 * rf_service_transmit() holds the radio mutex for that whole handover
 * (release RX -> idle -> flip GDO0 -> TX -> back). Handing this to a background
 * task would buy nothing: the user is waiting for exactly this to finish, the
 * radio is single-owner so a second transmit must queue anyway, and an
 * immediate 202 would force the UI to poll for a result it can only get here.
 * With httpd's default worker count, one busy worker for a fifth of a second is
 * cheaper than the machinery to avoid it. Keep the repeat count bounded so this
 * stays a fifth of a second and not five.
 *
 * 503 (not 409) when the radio is missing: the endpoint is correct and the
 * request is well-formed, the *service* is unavailable — and the UI keys its
 * "no radio detected" banner off that status.
 */
static esp_err_t api_signal_transmit(httpd_req_t *req, uint16_t id)
{
    const db_signal_meta_t *m = db_signals_get(id);
    if (!m) return send_error(req, "404 Not Found", "no such signal");
    if (!rf_service_radio_present())
        return send_error(req, "503 Service Unavailable",
                          "no CC1101 detected — see /api/diagnostics");

    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    double d;
    uint8_t repeats = s_cfg->tx_repeats;
    uint32_t gap_us = s_cfg->tx_gap_us;
    if (json_num(j, "repeats", &d)) repeats = (uint8_t)clampl(d, 1, 32);
    if (json_num(j, "gap_us", &d))  gap_us = (uint32_t)clampl(d, 0, 200000);
    cJSON_Delete(j);

    rf_frame_t *frame = malloc(sizeof(*frame));
    if (!frame) return send_error(req, "500 Internal Server Error", "out of memory");
    esp_err_t err = db_signals_load_frame(id, frame);
    if (err != ESP_OK) {
        free(frame);
        return send_esp_err(req, err, "could not load the stored waveform");
    }

    err = rf_service_transmit(frame, repeats, gap_us);
    free(frame);
    if (err != ESP_OK) return send_esp_err(req, err, "transmit failed");

    /* Nothing else logs an API-initiated replay, and "did my transmit go out?"
     * is exactly what the event log exists to answer. */
    db_events_push(DB_EV_TRANSMIT, id, 0, 0, repeats, "%s", m->name);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "repeats", repeats);
    cJSON_AddNumberToObject(o, "gap_us", (double)gap_us);
    /* Honesty, per db_diag.h: TX_OK is a software-level claim only. */
    cJSON_AddStringToObject(o, "note",
        "software-level success only — the box cannot tell whether a receiver reacted");
    return send_json(req, o, "200 OK");
}

/* POST /api/signals/virtual — synthesize an EV1527 signal to pair one of the
 * user's OWN receivers to (PLAN.md §6.3). id20 == 0 draws a random address. */
static esp_err_t api_signal_virtual(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    const char *raw = NULL;
    char name[DB_SIGNAL_NAME_MAX] = "Virtual signal";
    if (json_str(j, "name", &raw) && raw[0]) trim_copy(raw, name, sizeof(name));

    double d;
    uint32_t id20 = 0;
    uint8_t button = 8;
    uint16_t base_us = 0;   /* 0 => the store's default */
    if (json_num(j, "id20", &d))    id20 = (uint32_t)clampl(d, 0, 0xFFFFF);
    if (json_num(j, "button", &d))  button = (uint8_t)clampl(d, 0, 15);
    if (json_num(j, "base_us", &d)) base_us = (uint16_t)clampl(d, 0, 5000);
    cJSON_Delete(j);

    uint16_t id = 0;
    esp_err_t err = db_signals_create_virtual(name, id20, button, base_us, &id);
    if (err == ESP_OK) db_mqtt_on_signals_changed();
    if (err != ESP_OK) return send_esp_err(req, err, "could not create the virtual signal");

    const db_signal_meta_t *m = db_signals_get(id);
    if (!m) return send_error(req, "500 Internal Server Error", "signal vanished after creation");
    return send_json(req, signal_json(m), "200 OK");
}

/* ------------------------------------------------------------------ learn mode */

/*
 * The learn view. The candidate's `repeats` comes from the metadata's
 * seen_count — a pending candidate has been "seen" exactly as many times as the
 * burst repeated, which is the admission evidence signal_store weighs most
 * heavily. `rssi_dbm`/`repeats` come from db_signals_learn_candidate_info(),
 * since they describe one reception rather than the stored signal.
 */
static cJSON *learn_json(void)
{
    uint32_t remaining = 0;
    bool active = db_signals_learn_active(&remaining);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "active", active);
    cJSON_AddNumberToObject(o, "remaining_s", (double)remaining);

    db_signal_meta_t cand;
    int16_t cand_rssi = 0;
    uint8_t cand_repeats = 0;
    if (!db_signals_learn_candidate_info(&cand, &cand_rssi, &cand_repeats)) {
        cJSON_AddNullToObject(o, "candidate");
        return o;
    }

    char fp[9];
    snprintf(fp, sizeof(fp), "%08" PRIx32, (uint32_t)cand.fingerprint);
    cJSON *c = cJSON_AddObjectToObject(o, "candidate");
    cJSON_AddStringToObject(c, "fingerprint", fp);
    cJSON_AddNumberToObject(c, "base_us", cand.base_us);
    cJSON_AddNumberToObject(c, "confidence", cand.confidence);
    cJSON_AddNumberToObject(c, "pulse_count", cand.pulse_count);
    cJSON_AddNumberToObject(c, "repeats", cand_repeats);
    cJSON_AddNumberToObject(c, "rssi_dbm", cand_rssi);
    add_decoded(c, &cand);
    return o;
}

static esp_err_t api_learn_get(httpd_req_t *req)
{
    return send_json(req, learn_json(), "200 OK");
}

static esp_err_t api_learn_arm(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    double d;
    uint32_t timeout = 0;   /* 0 => DB_LEARN_DEFAULT_SECS */
    if (json_num(j, "timeout_s", &d)) timeout = (uint32_t)clampl(d, 5, 600);
    cJSON_Delete(j);

    db_signals_learn_arm(timeout);
    db_events_push(DB_EV_LEARN, 0, 0, 0, 0, "learn armed for %" PRIu32 " s",
                   timeout ? timeout : (uint32_t)DB_LEARN_DEFAULT_SECS);
    return send_json(req, learn_json(), "200 OK");
}

static esp_err_t api_learn_cancel(httpd_req_t *req)
{
    db_signals_learn_cancel();
    db_events_push(DB_EV_LEARN, 0, 0, 0, 0, "learn cancelled");
    return send_json(req, learn_json(), "200 OK");
}

static esp_err_t api_learn_accept(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    const char *raw = NULL;
    char name[DB_SIGNAL_NAME_MAX] = "";
    if (json_str(j, "name", &raw)) trim_copy(raw, name, sizeof(name));
    cJSON_Delete(j);
    if (!name[0]) return send_error(req, "400 Bad Request", "name must be a non-empty string");

    uint16_t id = 0;
    esp_err_t err = db_signals_learn_accept(name, &id);
    if (err == ESP_OK) db_mqtt_on_signals_changed();
    if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NOT_FOUND)
        return send_error(req, "409 Conflict", "no learn candidate to accept");
    if (err != ESP_OK) return send_esp_err(req, err, "could not store the candidate");

    const db_signal_meta_t *m = db_signals_get(id);
    if (!m) return send_error(req, "500 Internal Server Error", "signal vanished after creation");
    db_events_push(DB_EV_LEARN, id, 0, 0, 0, "learned \"%s\"", name);
    return send_json(req, signal_json(m), "200 OK");
}

/* ------------------------------------------------------------------ node graph */

/*
 * The wire names of db_node_type_t, which are the API.md contract.
 *
 * DESIGNATED INITIALIZERS, NOT POSITIONAL ONES. The enum has already been
 * renumbered once (source.any_rf was inserted after source.virtual, shifting
 * every logic/sink value), and a positional table would have kept compiling
 * while silently relabelling every stored node — turning a saved sink.transmit
 * into a logic.throttle on the next GET. Indexing by name makes an insertion a
 * no-op here and a missing entry a visible NULL rather than a wrong string.
 */
static const char *const NODE_TYPES[DB_NODE__COUNT] = {
    [DB_NODE_SOURCE_BUTTON]  = "source.button",
    [DB_NODE_SOURCE_GPIO]    = "source.gpio",
    [DB_NODE_SOURCE_VIRTUAL] = "source.virtual",
    /* Wildcard: fires on every received burst, matched or not. It carries no
     * type-specific parameters — wiring it to a sink is the whole feature. */
    [DB_NODE_SOURCE_ANY_RF]  = "source.any_rf",
    [DB_NODE_LOGIC_GROUP]    = "logic.group",
    [DB_NODE_LOGIC_THROTTLE] = "logic.throttle",
    [DB_NODE_SINK_TRANSMIT]  = "sink.transmit",
    [DB_NODE_SINK_MQTT]      = "sink.mqtt",
};

static bool node_type_from_str(const char *s, db_node_type_t *out)
{
    for (int i = 0; i < DB_NODE__COUNT; i++)
        if (NODE_TYPES[i] && strcmp(s, NODE_TYPES[i]) == 0) {
            *out = (db_node_type_t)i;
            return true;
        }
    return false;
}

/* Build the "must be one of ..." message from the table, so a new node type can
 * never be accepted by the parser while still being rejected by the help text. */
static void node_types_help(char *out, size_t outsz)
{
    size_t n = (size_t)snprintf(out, outsz, "type must be one of");
    for (int i = 0; i < DB_NODE__COUNT && n < outsz; i++)
        if (NODE_TYPES[i])
            n += (size_t)snprintf(out + n, outsz - n, "%s %s",
                                  i ? "," : "", NODE_TYPES[i]);
}

/* The flat node struct serializes mechanically — that is exactly why node_graph.h
 * chose it over a tagged union. Fields irrelevant to a type are still reported;
 * API.md says so, and it keeps the UI's form bindings unconditional. */
static cJSON *node_json(const db_node_t *n)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", n->id);
    cJSON_AddStringToObject(o, "type",
                            (n->type < DB_NODE__COUNT && NODE_TYPES[n->type])
                                ? NODE_TYPES[n->type] : "source.virtual");
    cJSON_AddStringToObject(o, "name", n->name);
    cJSON_AddBoolToObject(o, "enabled", n->enabled);
    cJSON_AddNumberToObject(o, "signal_id", n->signal_id);
    cJSON_AddNumberToObject(o, "gpio_pin", n->gpio_pin);
    cJSON_AddBoolToObject(o, "gpio_active_low", n->gpio_active_low);
    cJSON_AddNumberToObject(o, "gpio_debounce_ms", n->gpio_debounce_ms);
    cJSON_AddNumberToObject(o, "repeats", n->repeats);
    cJSON_AddNumberToObject(o, "gap_us", (double)n->gap_us);
    /* Seconds is the unit users think in for a cooldown ("ring at most once
     * every 30 s"), so it is the canonical field. window_ms is still emitted for
     * compatibility and because a group window can legitimately be sub-second. */
    cJSON_AddNumberToObject(o, "window_s", (double)n->window_ms / 1000.0);
    cJSON_AddNumberToObject(o, "window_ms", (double)n->window_ms);
    cJSON_AddStringToObject(o, "group_mode", n->group_mode == DB_GROUP_ALL ? "all" : "any");
    cJSON_AddStringToObject(o, "topic", n->topic);
    cJSON_AddNumberToObject(o, "ui_x", n->ui_x);
    cJSON_AddNumberToObject(o, "ui_y", n->ui_y);
    return o;
}

/* Apply every field present in `j` onto *n. Absent fields are left alone, which
 * is what makes POST /api/graph/nodes/<id> a partial update for free. */
static void node_apply_json(db_node_t *n, const cJSON *j)
{
    const char *s;
    double d;
    bool b;

    if (json_str(j, "name", &s))   trim_copy(s, n->name, sizeof(n->name));
    if (json_bool(j, "enabled", &b)) n->enabled = b;
    if (json_num(j, "signal_id", &d)) n->signal_id = (uint16_t)clampl(d, 0, 0xFFFF);
    /* -1 keeps the "unset" convention of node_graph.h; the picker's legal range
     * is whatever GET /api/gpio/available offered. */
    if (json_num(j, "gpio_pin", &d)) n->gpio_pin = (int8_t)clampl(d, -1, 48);
    if (json_bool(j, "gpio_active_low", &b)) n->gpio_active_low = b;
    if (json_num(j, "gpio_debounce_ms", &d)) n->gpio_debounce_ms = (uint16_t)clampl(d, 0, 2000);
    if (json_num(j, "repeats", &d)) n->repeats = (uint8_t)clampl(d, 1, 32);
    if (json_num(j, "gap_us", &d))  n->gap_us = (uint32_t)clampl(d, 0, 200000);
    /* window_s wins when both are present. Upper bound 6000 s (~100 min): well
     * past anything sensible, but the ceiling is the user's to choose, not ours. */
    if (json_num(j, "window_s", &d))
        n->window_ms = (uint32_t)(clampd(d, 0.0, 6000.0) * 1000.0);
    else if (json_num(j, "window_ms", &d))
        n->window_ms = (uint32_t)clampl(d, 0, 6000000);
    if (json_str(j, "group_mode", &s))
        n->group_mode = (uint8_t)(strcmp(s, "all") == 0 ? DB_GROUP_ALL : DB_GROUP_ANY);
    if (json_str(j, "topic", &s)) trim_copy(s, n->topic, sizeof(n->topic));
    if (json_num(j, "ui_x", &d))  n->ui_x = (int16_t)clampl(d, -32768, 32767);
    if (json_num(j, "ui_y", &d))  n->ui_y = (int16_t)clampl(d, -32768, 32767);
}

static esp_err_t api_graph_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    const db_node_t *nl = db_graph_nodes();
    int nn = db_graph_node_count();
    for (int i = 0; i < nn; i++)
        cJSON_AddItemToArray(nodes, node_json(&nl[i]));

    cJSON *links = cJSON_AddArrayToObject(root, "links");
    const db_link_t *ll = db_graph_links();
    int ln = db_graph_link_count();
    for (int i = 0; i < ln; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddNumberToObject(l, "from", ll[i].from);
        cJSON_AddNumberToObject(l, "to", ll[i].to);
        cJSON_AddItemToArray(links, l);
    }
    return send_json(req, root, "200 OK");
}

static esp_err_t api_node_create(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    const char *ts = NULL;
    db_node_type_t type;
    if (!json_str(j, "type", &ts) || !node_type_from_str(ts, &type)) {
        cJSON_Delete(j);
        char help[192];
        node_types_help(help, sizeof(help));
        return send_error(req, "400 Bad Request", help);
    }

    /* Type defaults first (repeats, windows, debounce), then the body on top —
     * so a caller only has to send what it actually wants to differ. */
    db_node_t n;
    db_graph_node_defaults(&n, type);
    node_apply_json(&n, j);
    cJSON_Delete(j);
    n.id = 0;                 /* the graph assigns it */
    n.type = (uint8_t)type;

    uint16_t id = 0;
    esp_err_t err = db_graph_add_node(&n, &id);
    if (err == ESP_OK) { db_graph_apply_gpio_inputs(); db_mqtt_on_graph_changed(); }
    if (err != ESP_OK) return send_esp_err(req, err, "could not create the node");

    /* A new/changed wired input needs its pin claimed before it can fire. */
    if (type == DB_NODE_SOURCE_GPIO) db_graph_apply_gpio_inputs();

    const db_node_t *created = db_graph_node(id);
    if (!created) return send_error(req, "500 Internal Server Error", "node vanished after creation");
    return send_json(req, node_json(created), "200 OK");
}

static esp_err_t api_node_update(httpd_req_t *req, uint16_t id)
{
    const db_node_t *cur = db_graph_node(id);
    if (!cur) return send_error(req, "404 Not Found", "no such node");

    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    db_node_t n = *cur;
    const char *ts = NULL;
    if (json_str(j, "type", &ts)) {
        db_node_type_t type;
        if (!node_type_from_str(ts, &type)) {
            cJSON_Delete(j);
            return send_error(req, "400 Bad Request", "unknown node type");
        }
        n.type = (uint8_t)type;
    }
    node_apply_json(&n, j);
    cJSON_Delete(j);
    n.id = id;

    esp_err_t err = db_graph_update_node(&n);
    if (err == ESP_OK) { db_graph_apply_gpio_inputs(); db_mqtt_on_graph_changed(); }
    if (err != ESP_OK) return send_esp_err(req, err, "could not update the node");
    db_graph_apply_gpio_inputs();   /* idempotent; releases pins no longer used */

    const db_node_t *updated = db_graph_node(id);
    if (!updated) return send_error(req, "404 Not Found", "no such node");
    return send_json(req, node_json(updated), "200 OK");
}

static esp_err_t api_node_delete(httpd_req_t *req, uint16_t id)
{
    if (!db_graph_node(id)) return send_error(req, "404 Not Found", "no such node");
    esp_err_t err = db_graph_delete_node(id);
    if (err == ESP_OK) { db_graph_apply_gpio_inputs(); db_mqtt_on_graph_changed(); }
    if (err != ESP_OK) return send_esp_err(req, err, "could not delete the node");
    db_graph_apply_gpio_inputs();   /* frees the pin if that node owned one */
    return send_ok(req);
}

/*
 * POST /api/graph/nodes/<id>/fire — test-fire, or trigger a source.virtual.
 *
 * Same ~200 ms inline-blocking rationale as api_signal_transmit: a traversal
 * that reaches a sink.transmit keys the radio synchronously on this worker.
 * The radio-presence check is unconditional rather than "only if the chain
 * contains a transmit sink": the traversal's shape is not known until it runs,
 * a doorbell chain almost always ends in a transmit, and a clear 503 is far more
 * useful to the user than a 200 for a fire that silently did nothing.
 */
static esp_err_t api_node_fire(httpd_req_t *req, uint16_t id)
{
    if (!db_graph_node(id)) return send_error(req, "404 Not Found", "no such node");
    if (!rf_service_radio_present())
        return send_error(req, "503 Service Unavailable",
                          "no CC1101 detected — see /api/diagnostics");

    esp_err_t err = db_graph_fire_node(id);
    if (err != ESP_OK) return send_esp_err(req, err, "could not fire the node");
    return send_ok(req);
}

/* Both link routes take {"from":1,"to":2}; DELETE carries it in the body too. */
static esp_err_t api_link_edit(httpd_req_t *req, bool add)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    double f, t;
    if (!json_num(j, "from", &f) || !json_num(j, "to", &t)) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "from and to are required node ids");
    }
    uint16_t from = (uint16_t)clampl(f, 0, 0xFFFF);
    uint16_t to = (uint16_t)clampl(t, 0, 0xFFFF);
    cJSON_Delete(j);

    esp_err_t err = add ? db_graph_add_link(from, to) : db_graph_delete_link(from, to);
    if (err == ESP_OK) db_mqtt_on_graph_changed();
    if (err != ESP_OK)
        return send_esp_err(req, err, add ? "could not add the link" : "could not remove the link");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "from", from);
    cJSON_AddNumberToObject(o, "to", to);
    return send_json(req, o, "200 OK");
}

/* ------------------------------------------------------------------ gpio picker */

/*
 * GET /api/gpio/available — which pins the UI may offer for a source.gpio node.
 *
 * The wired-button feature is optional and its pin is chosen HERE, at runtime,
 * not compiled in (node_graph.h). That makes this list a safety device: offering
 * a pin that carries the SPI flash clock would let a user brick the box from a
 * dropdown. The exclusions, in order of how badly they hurt:
 *
 *   26-32  SPI flash. Driving one of these hangs or bricks the module.
 *   33-37  the octal PSRAM of this N16R8 module. Same class of damage.
 *   19,20  native USB D-/D+. Usable as GPIO, but claiming them kills USB-serial-
 *          JTAG — i.e. the console you would need to recover the box.
 *   22-25  do not exist on the ESP32-S3 at all.
 *   0,45,46  strapping pins sampled at reset; a button pulling one low at the
 *          wrong moment changes the boot mode.
 *   43,44  UART0 console.
 *   38-42,47,48  present on the dev board, NOT broken out on the ESP32-S3 Zero.
 *          board_pins.h's portability rule says the firmware must move to that
 *          board unchanged, so offering them would produce configurations that
 *          silently stop working after a hardware swap.
 *
 * The radio pins are reported separately as `in_use`, because "the doorbell
 * radio has it" is a different answer from "that pin does not exist" and the UI
 * says so. Pins already bound to an existing gpio node are deliberately NOT
 * excluded: a node being edited must still see its own pin in the list.
 */
#define GPIO_MAX_PIN 48

static bool gpio_is_radio_pin(int pin)
{
    return pin == DB_PIN_CC1101_GDO0 || pin == DB_PIN_CC1101_GDO2 ||
           pin == DB_PIN_CC1101_CS   || pin == DB_PIN_CC1101_MOSI ||
           pin == DB_PIN_CC1101_SCK  || pin == DB_PIN_CC1101_MISO;
}

static bool gpio_is_offerable(int pin)
{
    if (pin < 0 || pin > GPIO_MAX_PIN) return false;
    if (gpio_is_radio_pin(pin)) return false;
    if (pin == 0 || pin == 45 || pin == 46) return false;    /* strapping */
    if (pin >= 19 && pin <= 20) return false;                /* native USB */
    if (pin >= 22 && pin <= 25) return false;                /* not on the S3 */
    if (pin >= 26 && pin <= 32) return false;                /* SPI flash */
    if (pin >= 33 && pin <= 37) return false;                /* octal PSRAM */
    if (pin >= 38) return false;                             /* UART0 + not on the S3 Zero */
    return true;
}

static esp_err_t api_gpio_available(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *suggested = cJSON_AddArrayToObject(root, "suggested");
    const int defaults[] = { DB_PIN_WIRED_IN_0, DB_PIN_WIRED_IN_1 };
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); i++)
        if (gpio_is_offerable(defaults[i]))
            cJSON_AddItemToArray(suggested, cJSON_CreateNumber(defaults[i]));

    cJSON *avail = cJSON_AddArrayToObject(root, "available");
    for (int p = 0; p <= GPIO_MAX_PIN; p++)
        if (gpio_is_offerable(p))
            cJSON_AddItemToArray(avail, cJSON_CreateNumber(p));

    cJSON *in_use = cJSON_AddArrayToObject(root, "in_use");
    for (int p = 0; p <= GPIO_MAX_PIN; p++)
        if (gpio_is_radio_pin(p))
            cJSON_AddItemToArray(in_use, cJSON_CreateNumber(p));

    return send_json(req, root, "200 OK");
}

/* ------------------------------------------------------------------ events */

static const char *event_kind_str(uint8_t kind)
{
    switch (kind) {
    case DB_EV_RF_UNMATCHED: return "rf_unmatched";
    case DB_EV_BUTTON_PRESS: return "button_press";
    case DB_EV_WIRED_PRESS:  return "wired_press";
    case DB_EV_NODE_FIRED:   return "node_fired";
    case DB_EV_TRANSMIT:     return "transmit";
    case DB_EV_LEARN:        return "learn";
    default:                 return "system";
    }
}

/*
 * GET /api/events?since=<serial>
 *
 * The serial is the cheap-polling contract: a UI polls every second or two, and
 * when the serial it already holds is still current the reply is a handful of
 * bytes and it re-renders nothing. The ring itself is ~4.6 KB, so it is copied
 * to the heap rather than onto the httpd worker's stack.
 */
static esp_err_t api_events(httpd_req_t *req)
{
    uint32_t serial = db_events_serial();
    bool have_since = false;
    uint32_t since = 0;

    char query[64], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", val, sizeof(val)) == ESP_OK) {
        since = (uint32_t)strtoul(val, NULL, 10);
        have_since = true;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "serial", (double)serial);
    cJSON *arr = cJSON_AddArrayToObject(root, "events");

    if (have_since && since == serial)
        return send_json(req, root, "200 OK");   /* nothing new — skip the render */

    db_event_t *buf = calloc(DB_EVENT_RING, sizeof(*buf));
    if (!buf) {
        cJSON_Delete(root);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int n = db_events_get(buf, DB_EVENT_RING);
    for (int i = 0; i < n; i++) {       /* db_events_get already returns newest first */
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ts_s", (double)(buf[i].ts_us / 1000000));
        cJSON_AddStringToObject(e, "kind", event_kind_str(buf[i].kind));
        cJSON_AddNumberToObject(e, "signal_id", buf[i].signal_id);
        cJSON_AddNumberToObject(e, "node_id", buf[i].node_id);
        cJSON_AddNumberToObject(e, "rssi_dbm", buf[i].rssi_dbm);
        cJSON_AddNumberToObject(e, "repeats", buf[i].repeats);
        cJSON_AddStringToObject(e, "text", buf[i].text);
        cJSON_AddItemToArray(arr, e);
    }
    free(buf);
    return send_json(req, root, "200 OK");
}

/* ------------------------------------------------------------------ config */

/* GET /api/config — NON-SECRET config only. Passwords are reported as presence
 * booleans; see the file header for why this must stay that way. */
static esp_err_t api_config_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "hostname", s_cfg->hostname);

    cJSON *sta = cJSON_AddObjectToObject(root, "sta");
    cJSON *nets = cJSON_AddArrayToObject(sta, "networks");
    for (int i = 0; i < DB_STA_MAX; i++) {
        cJSON *n = cJSON_CreateObject();
        cJSON_AddStringToObject(n, "ssid", s_cfg->sta[i].ssid);
        cJSON_AddBoolToObject(n, "has_pass", s_cfg->sta[i].pass[0] != '\0');
        cJSON_AddItemToArray(nets, n);
    }

    cJSON *mqtt = cJSON_AddObjectToObject(root, "mqtt");
    cJSON_AddBoolToObject(mqtt, "enabled", s_cfg->mqtt_enabled);
    cJSON_AddStringToObject(mqtt, "host", s_cfg->mqtt_host);
    cJSON_AddNumberToObject(mqtt, "port", s_cfg->mqtt_port);
    cJSON_AddStringToObject(mqtt, "user", s_cfg->mqtt_user);
    cJSON_AddBoolToObject(mqtt, "has_pass", s_cfg->mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(mqtt, "base_topic", s_cfg->mqtt_base_topic);
    cJSON_AddBoolToObject(mqtt, "homeassistant", s_cfg->mqtt_homeassistant);
    cJSON_AddStringToObject(mqtt, "discovery_prefix", s_cfg->mqtt_discovery_prefix);

    cJSON *ota = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddStringToObject(ota, "url", s_cfg->ota_url);

    return send_json(req, root, "200 OK");
}

/*
 * POST /api/config — a partial update of the same shape.
 *
 * The password rule, applied uniformly: an absent or EMPTY "pass" leaves the
 * stored secret untouched, so a UI that never received it can still submit the
 * form. Changing a slot's SSID clears that slot's password first — carrying a
 * passphrase across to a different network would be both wrong and confusing.
 */
static esp_err_t api_config_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    const char *s;
    double d;
    bool b;
    bool sta_changed = false;

    if (json_str(j, "hostname", &s) && s[0])
        strlcpy(s_cfg->hostname, s, sizeof(s_cfg->hostname));

    cJSON *sta = cJSON_GetObjectItem(j, "sta");
    if (cJSON_IsObject(sta)) {
        cJSON *nets = cJSON_GetObjectItem(sta, "networks");
        if (cJSON_IsArray(nets)) {
            int slot = 0;
            cJSON *e;
            cJSON_ArrayForEach(e, nets) {
                if (slot >= DB_STA_MAX) break;
                if (!cJSON_IsObject(e)) { slot++; continue; }
                db_sta_net_t *n = &s_cfg->sta[slot];
                if (json_str(e, "ssid", &s) && strcmp(s, n->ssid) != 0) {
                    strlcpy(n->ssid, s, sizeof(n->ssid));
                    n->pass[0] = '\0';        /* different network, different key */
                    sta_changed = true;
                }
                /* "" == leave unchanged (the write-only rule).
                 * Accept BOTH "password" (canonical, and what POST /api/wifi
                 * uses) and "pass" (the shorter legacy spelling). The two
                 * endpoints originally disagreed, which made saving a Wi-Fi key
                 * from the settings screen fail SILENTLY — the request
                 * succeeded, the password was simply dropped. Taking either key
                 * costs one line and removes a whole class of that bug. */
                if ((json_str(e, "password", &s) || json_str(e, "pass", &s)) && s[0]) {
                    strlcpy(n->pass, s, sizeof(n->pass));
                    sta_changed = true;
                }
                slot++;
            }
        }
    }

    cJSON *mqtt = cJSON_GetObjectItem(j, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        if (json_bool(mqtt, "enabled", &b)) s_cfg->mqtt_enabled = b;
        if (json_str(mqtt, "host", &s))     strlcpy(s_cfg->mqtt_host, s, sizeof(s_cfg->mqtt_host));
        if (json_num(mqtt, "port", &d))     s_cfg->mqtt_port = (uint16_t)clampl(d, 1, 65535);
        if (json_str(mqtt, "user", &s))     strlcpy(s_cfg->mqtt_user, s, sizeof(s_cfg->mqtt_user));
        if ((json_str(mqtt, "password", &s) || json_str(mqtt, "pass", &s)) && s[0])
            strlcpy(s_cfg->mqtt_pass, s, sizeof(s_cfg->mqtt_pass));
        if (json_str(mqtt, "base_topic", &s))
            strlcpy(s_cfg->mqtt_base_topic, s, sizeof(s_cfg->mqtt_base_topic));
        if (json_bool(mqtt, "homeassistant", &b)) s_cfg->mqtt_homeassistant = b;
        if (json_str(mqtt, "discovery_prefix", &s))
            strlcpy(s_cfg->mqtt_discovery_prefix, s, sizeof(s_cfg->mqtt_discovery_prefix));
    }

    cJSON *ota = cJSON_GetObjectItem(j, "ota");
    if (cJSON_IsObject(ota) && json_str(ota, "url", &s))
        strlcpy(s_cfg->ota_url, s, sizeof(s_cfg->ota_url));

    cJSON_Delete(j);
    db_config_save(s_cfg);
    /* Credentials apply live — no reboot needed to join a new home network. */
    if (sta_changed) db_wifi_retry_sta();
    return api_config_get(req);
}

/* ------------------------------------------------------------------ softAP */

static esp_err_t api_ap_get(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", s_cfg->ap_ssid);
    cJSON_AddNumberToObject(o, "security", s_cfg->ap_security);
    cJSON_AddNumberToObject(o, "channel", s_cfg->ap_channel);

    /* The EFFECTIVE address, which may differ from the stored one after a
     * subnet-collision hop (wifi_mgr.h); falling back to the stored value before
     * the AP netif is up. */
    char ip[16];
    db_wifi_ap_ip(ip);
    cJSON_AddStringToObject(o, "ip", ip[0] ? ip : s_cfg->ap_ip);

    cJSON_AddBoolToObject(o, "enabled", s_cfg->ap_enabled);
    cJSON_AddBoolToObject(o, "fallback_enabled", s_cfg->ap_fallback_enabled);
    cJSON_AddBoolToObject(o, "has_recovery_pass", s_cfg->recovery_ap_pass[0] != '\0');
    return send_json(req, o, "200 OK");
}

/*
 * POST /api/ap — validate EVERYTHING before mutating anything.
 *
 * The failure this guards against is specific and was learned the hard way in
 * the reference firmware: persisting a 5-character WPA2 passphrase makes the
 * softAP refuse to start on the NEXT boot, i.e. the box becomes unreachable
 * long after the request that broke it.
 */
static esp_err_t api_ap_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    const char *s;
    double d;
    bool b;
    const char *err = NULL;

    char ssid[DB_STR_SSID] = "";
    bool has_ssid = false;
    if (json_str(j, "ssid", &s)) {
        trim_copy(s, ssid, sizeof(ssid));
        has_ssid = true;
        if (!ssid[0]) err = "ssid must be a non-empty string";
    }

    int sec = -1;
    if (!err && json_num(j, "security", &d)) {
        sec = (int)d;
        if (sec != 0 && sec != 2) err = "security must be 0 (open) or 2 (WPA2)";
    }
    int eff_sec = (sec >= 0) ? sec : s_cfg->ap_security;

    const char *pass = NULL;
    if (!err && json_str(j, "pass", &s) && s[0]) {   /* "" == leave unchanged */
        pass = s;
        size_t pl = strlen(pass);
        if (eff_sec == 0) err = "an open network takes no passphrase";
        else if (pl < 8 || pl > 63) err = "the WPA2 passphrase must be 8..63 characters";
    }

    const char *rpass = NULL;
    if (!err && json_str(j, "recovery_pass", &s) && s[0]) {
        rpass = s;
        size_t pl = strlen(rpass);
        if (pl < 8 || pl > 63)
            err = "the recovery passphrase must be 8..63 characters (or empty to keep the current one)";
    }

    int chan = -1;
    if (!err && json_num(j, "channel", &d)) {
        chan = (int)d;
        if (chan < 1 || chan > 13) err = "channel must be 1..13";
    }

    char ip[16] = "";
    bool has_ip = false;
    if (!err && json_str(j, "ip", &s) && s[0]) {
        trim_copy(s, ip, sizeof(ip));
        has_ip = true;
        int a, bb, c, dd;
        if (sscanf(ip, "%d.%d.%d.%d", &a, &bb, &c, &dd) != 4 ||
            a < 1 || a > 254 || bb < 0 || bb > 255 || c < 0 || c > 255 || dd < 1 || dd > 254)
            err = "ip must be a dotted IPv4 host address";
    }

    if (err) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", err);
    }

    /* Everything validated — now commit. */
    if (has_ssid) strlcpy(s_cfg->ap_ssid, ssid, sizeof(s_cfg->ap_ssid));
    if (pass)     strlcpy(s_cfg->ap_pass, pass, sizeof(s_cfg->ap_pass));
    if (rpass)    strlcpy(s_cfg->recovery_ap_pass, rpass, sizeof(s_cfg->recovery_ap_pass));
    if (sec >= 0) s_cfg->ap_security = (uint8_t)sec;
    if (chan > 0) s_cfg->ap_channel = (uint8_t)chan;
    if (has_ip)   strlcpy(s_cfg->ap_ip, ip, sizeof(s_cfg->ap_ip));
    if (json_bool(j, "fallback_enabled", &b)) s_cfg->ap_fallback_enabled = b;

    bool en_changed = false;
    if (json_bool(j, "enabled", &b)) {
        en_changed = (b != s_cfg->ap_enabled);
        s_cfg->ap_enabled = b;
    }
    cJSON_Delete(j);

    db_config_save(s_cfg);
    /* Only the on/off flag applies live (wifi_mgr keeps its no-STA safety
     * exemption); every other radio change needs a clean bring-up. */
    if (en_changed) db_wifi_apply_ap_enabled(s_cfg);
    return api_ap_get(req);
}

/* ------------------------------------------------------------------ wi-fi wizard */

#define SCAN_MAX_APS 32

static esp_err_t api_wifi_scan(httpd_req_t *req)
{
    db_wifi_ap_t *aps = calloc(SCAN_MAX_APS, sizeof(*aps));
    if (!aps) return send_error(req, "500 Internal Server Error", "out of memory");

    size_t found = 0;
    /* Blocking for a second or two, and it briefly interrupts AP traffic — hence
     * "on demand", never polled (wifi_mgr.h). */
    esp_err_t err = db_wifi_scan(aps, SCAN_MAX_APS, &found);
    if (err != ESP_OK) {
        free(aps);
        return send_error(req, "503 Service Unavailable",
                          "the radio was busy — try the scan again in a moment");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < found; i++) {
        bool known = false;
        for (int k = 0; k < DB_STA_MAX; k++)
            if (s_cfg->sta[k].ssid[0] && strcmp(s_cfg->sta[k].ssid, aps[i].ssid) == 0)
                known = true;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(e, "auth", aps[i].authmode);
        cJSON_AddNumberToObject(e, "channel", aps[i].channel);
        cJSON_AddBoolToObject(e, "known", known);
        cJSON_AddItemToArray(arr, e);
    }
    free(aps);
    return send_json(req, root, "200 OK");
}

/*
 * POST /api/wifi — the recovery wizard's save.
 *
 * API.md specifies this as "saves and reboots into normal mode", and the reboot
 * is the honest implementation rather than a shortcut: recovery is a BOOT-TIME
 * decision (wifi_mgr.h), so the only way to leave the portal deterministically —
 * dropping the captive DNS, restoring the operational AP personality and running
 * the ordinary boot connect sequence — is to start over.
 */
static esp_err_t api_wifi_post(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");

    double d;
    int slot = 0;
    if (json_num(j, "slot", &d)) slot = (int)d;
    if (slot < 0 || slot >= DB_STA_MAX) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "slot must be 0..2");
    }
    const char *ssid = NULL;
    if (!json_str(j, "ssid", &ssid) || !ssid[0]) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "ssid must be a non-empty string");
    }
    const char *pass = NULL;
    if (!json_str(j, "password", &pass)) pass = "";   /* "" == open network here:
                                                       * a brand-new SSID has no
                                                       * stored key to preserve */

    strlcpy(s_cfg->sta[slot].ssid, ssid, sizeof(s_cfg->sta[slot].ssid));
    strlcpy(s_cfg->sta[slot].pass, pass, sizeof(s_cfg->sta[slot].pass));
    cJSON_Delete(j);
    db_config_save(s_cfg);

    ESP_LOGI(TAG, "wizard: saved \"%s\" into slot %d — rebooting", s_cfg->sta[slot].ssid, slot);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "slot", slot);
    cJSON_AddStringToObject(o, "ssid", s_cfg->sta[slot].ssid);
    cJSON_AddBoolToObject(o, "restarting", true);
    send_json(req, o, "200 OK");

    vTaskDelay(pdMS_TO_TICKS(800));   /* the phone must receive the reply first */
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ OTA */

static esp_err_t api_ota_url(httpd_req_t *req, bool webui)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    const char *url = NULL;
    if (!json_str(j, "url", &url) || !url[0]) {
        /* The app OTA may fall back to the stored default; the web-UI one has no
         * sensible default, since it is a different image entirely. */
        url = webui ? NULL : (s_cfg->ota_url[0] ? s_cfg->ota_url : NULL);
    }
    if (!url) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", "url required");
    }

    char copy[DB_STR_URL];
    strlcpy(copy, url, sizeof(copy));
    cJSON_Delete(j);

    if (!webui) {   /* remember it as the new default */
        strlcpy(s_cfg->ota_url, copy, sizeof(s_cfg->ota_url));
        db_config_save(s_cfg);
    }

    esp_err_t err = webui ? db_ota_webui_from_url(copy) : db_ota_start(copy);
    if (err != ESP_OK)
        return send_error(req, "409 Conflict",
                          "another update is already running, or the home network is down");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "status", webui
        ? "web UI update started; the device reboots to remount it"
        : "firmware update started; the device reboots on success");
    return send_json(req, o, "200 OK");
}

/*
 * POST /api/ota/upload and /api/ota/webui/upload — the raw .bin as the body.
 *
 * Streamed straight into flash through ota.c's upload session, so the image size
 * is bounded by the partition rather than the heap. Content-Length is required:
 * the partition-fit check has to happen BEFORE the first byte is written, and
 * for a web-UI image "before" also means before the partition is erased.
 *
 * Unlike the URL routes this needs no internet uplink, so it is the update path
 * that works from the recovery portal.
 */
#define OTA_UPLOAD_CHUNK      4096
#define OTA_UPLOAD_RECV_RETRY 5

static esp_err_t api_ota_upload(httpd_req_t *req, bool webui)
{
    const char *emsg = NULL;
    int total = req->content_len;
    if (total <= 0)
        return send_error(req, "411 Length Required",
                          "send the raw .bin as the request body (Content-Length required)");

    esp_err_t err = db_ota_upload_begin(webui ? DB_OTA_UPLOAD_WEBUI : DB_OTA_UPLOAD_APP,
                                        (size_t)total, &emsg);
    if (err != ESP_OK) {
        bool reboot = db_ota_upload_abort();
        const char *status = (err == ESP_ERR_INVALID_STATE) ? "409 Conflict"
                           : (err == ESP_ERR_INVALID_SIZE)  ? "413 Payload Too Large"
                                                            : "400 Bad Request";
        send_error(req, status, emsg ? emsg : "could not start the update");
        if (reboot) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
        return ESP_OK;
    }

    char *buf = malloc(OTA_UPLOAD_CHUNK);
    if (!buf) { emsg = "out of memory for the upload buffer"; goto fail; }

    ESP_LOGI(TAG, "OTA upload (%s): receiving %d bytes", webui ? "web UI" : "app", total);
    int off = 0, timeouts = 0;
    while (off < total) {
        int want = total - off;
        if (want > OTA_UPLOAD_CHUNK) want = OTA_UPLOAD_CHUNK;
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= OTA_UPLOAD_RECV_RETRY) continue;
        if (r <= 0) { emsg = "upload interrupted (connection lost)"; goto fail; }
        timeouts = 0;
        if (db_ota_upload_write(buf, (size_t)r, &emsg) != ESP_OK) goto fail;
        off += r;
    }
    free(buf);
    buf = NULL;

    if (db_ota_upload_finish(&emsg) != ESP_OK)
        return send_error(req, "400 Bad Request", emsg ? emsg : "image validation failed");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "bytes", total);
    cJSON_AddStringToObject(o, "status", webui
        ? "web UI image flashed; rebooting to remount it"
        : "firmware flashed to the inactive slot; rebooting into it");
    send_json(req, o, "200 OK");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;

fail:
    free(buf);
    {
        /* A web-UI upload that failed AFTER the erase leaves no UI in flash; the
         * reboot at least remounts a consistent (empty) filesystem. */
        bool reboot = db_ota_upload_abort();
        ESP_LOGE(TAG, "OTA upload failed: %s", emsg ? emsg : "?");
        send_error(req, "500 Internal Server Error", emsg ? emsg : "upload failed");
        if (reboot) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ update check
 *
 * The read side of update_check.h. All three routes answer from the RAM cache,
 * so none of them ever waits on GitHub: /api/update/check merely ASKS for a
 * refresh and returns the same object the GET returns, with `checking` true
 * while the fetch task runs. A UI polls the GET (cheaply, locally) until
 * `checking` goes false — it must never poll /api/update/check itself, which is
 * why the rate limit lives in update_check.c rather than being a UI convention.
 *
 * `checked_at_s` is device-uptime seconds, the SAME monotonic clock as an
 * event's `ts_s`, so the UI can render "checked 3 minutes ago" with the machinery
 * it already has. 0 means "never checked".
 */
static esp_err_t api_update_get(httpd_req_t *req)
{
    db_update_status_t st;
    db_update_get(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "valid", st.valid);
    cJSON_AddBoolToObject(o, "checking", st.checking);
    cJSON_AddBoolToObject(o, "update_available", st.update_available);
    cJSON_AddStringToObject(o, "current", st.current);
    cJSON_AddStringToObject(o, "latest", st.latest);
    cJSON_AddStringToObject(o, "published_at", st.published_at);
    cJSON_AddStringToObject(o, "html_url", st.html_url);
    cJSON_AddStringToObject(o, "app_url", st.app_url);
    cJSON_AddStringToObject(o, "webui_url", st.webui_url);
    cJSON_AddStringToObject(o, "error", st.error);
    cJSON_AddNumberToObject(o, "checked_at_s", (double)(st.checked_at_us / 1000000));
    /* The client cannot know the cache policy otherwise, and a UI that shows a
     * disabled "Check" button needs to say for how long. */
    cJSON_AddNumberToObject(o, "min_interval_s", (double)DB_UPDATE_MIN_INTERVAL_S);
    cJSON_AddBoolToObject(o, "sta_connected", db_wifi_sta_connected());
    return send_json(req, o, "200 OK");
}

static esp_err_t api_update_check(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    bool force = false;
    json_bool(j, "force", &force);
    cJSON_Delete(j);

    /* 503, not 409: the request is correct and the endpoint exists, the box
     * simply has no uplink — the same reading api_signal_transmit gives a
     * missing radio, and what the UI keys its explanation off. */
    if (!db_wifi_sta_connected())
        return send_error(req, "503 Service Unavailable",
                          "not on the home Wi-Fi — an update check needs internet access");

    esp_err_t err = db_update_check(force);
    if (err == ESP_ERR_INVALID_STATE)
        return send_error(req, "503 Service Unavailable",
                          "not on the home Wi-Fi — an update check needs internet access");
    if (err != ESP_OK)
        return send_esp_err(req, err, "could not start the update check");
    return api_update_get(req);
}

static esp_err_t api_update_install(httpd_req_t *req)
{
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "invalid JSON body");
    bool webui = false;
    json_bool(j, "webui", &webui);
    cJSON_Delete(j);

    if (!db_wifi_sta_connected())
        return send_error(req, "503 Service Unavailable",
                          "the home Wi-Fi is down — use the browser upload instead");

    const char *emsg = NULL;
    esp_err_t err = db_update_install(webui, &emsg);
    if (err == ESP_ERR_NOT_FOUND)
        return send_error(req, "409 Conflict", emsg ? emsg : "no such asset in the release");
    if (err != ESP_OK)
        return send_error(req, "409 Conflict",
                          emsg ? emsg : "no update is available to install");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "webui", webui);
    cJSON_AddStringToObject(o, "status", webui
        ? "web UI update started; the device reboots to remount it"
        : "firmware update started; the device reboots on success");
    return send_json(req, o, "200 OK");
}

/* ------------------------------------------------------------------ routers */

static esp_err_t get_router(httpd_req_t *req)
{
    const char *u = req->uri;
    char tail[24];

    if (uri_is(u, "/api/system"))         return api_system_get(req);
    if (uri_is(u, "/api/radio"))          return api_radio_get(req);
    if (uri_is(u, "/api/diagnostics"))    return api_diagnostics(req);
    if (uri_is(u, "/api/signals"))        return api_signals_list(req);
    if (uri_starts(u, "/api/signals/")) {
        uint16_t id = path_id(u, "/api/signals/", tail, sizeof(tail));
        if (id && !tail[0]) return api_signal_detail(req, id);
        return send_error(req, "404 Not Found", "no such endpoint");
    }
    if (uri_is(u, "/api/learn"))          return api_learn_get(req);
    if (uri_is(u, "/api/graph"))          return api_graph_get(req);
    if (uri_is(u, "/api/gpio/available")) return api_gpio_available(req);
    if (uri_is(u, "/api/events"))         return api_events(req);
    if (uri_is(u, "/api/config"))         return api_config_get(req);
    if (uri_is(u, "/api/ap"))             return api_ap_get(req);
    if (uri_is(u, "/api/wifi/scan"))      return api_wifi_scan(req);
    if (uri_is(u, "/api/update"))         return api_update_get(req);
    return send_error(req, "404 Not Found", "no such endpoint");
}

static esp_err_t post_router(httpd_req_t *req)
{
    const char *u = req->uri;
    char tail[24];

    /* Longest-prefix first throughout: "/api/ota" is a prefix of every other OTA
     * route, and "/api/signals/virtual" must not be read as an id. */
    if (uri_is(u, "/api/ota/webui/upload")) return api_ota_upload(req, true);
    if (uri_is(u, "/api/ota/upload"))       return api_ota_upload(req, false);
    if (uri_is(u, "/api/ota/webui"))        return api_ota_url(req, true);
    if (uri_is(u, "/api/ota"))              return api_ota_url(req, false);

    if (uri_is(u, "/api/update/install"))   return api_update_install(req);
    if (uri_is(u, "/api/update/check"))     return api_update_check(req);

    if (uri_is(u, "/api/system/hostname"))  return api_system_hostname(req);
    if (uri_is(u, "/api/restart"))          return api_restart(req);
    if (uri_is(u, "/api/radio"))            return api_radio_post(req);

    if (uri_is(u, "/api/signals/virtual"))  return api_signal_virtual(req);
    if (uri_starts(u, "/api/signals/")) {
        uint16_t id = path_id(u, "/api/signals/", tail, sizeof(tail));
        if (id && !tail[0])                       return api_signal_rename(req, id);
        if (id && strcmp(tail, "/transmit") == 0) return api_signal_transmit(req, id);
        return send_error(req, "404 Not Found", "no such endpoint");
    }

    if (uri_is(u, "/api/learn/arm"))    return api_learn_arm(req);
    if (uri_is(u, "/api/learn/cancel")) return api_learn_cancel(req);
    if (uri_is(u, "/api/learn/accept")) return api_learn_accept(req);

    if (uri_is(u, "/api/graph/nodes"))  return api_node_create(req);
    if (uri_is(u, "/api/graph/links"))  return api_link_edit(req, true);
    if (uri_starts(u, "/api/graph/nodes/")) {
        uint16_t id = path_id(u, "/api/graph/nodes/", tail, sizeof(tail));
        if (id && !tail[0])                   return api_node_update(req, id);
        if (id && strcmp(tail, "/fire") == 0) return api_node_fire(req, id);
        return send_error(req, "404 Not Found", "no such endpoint");
    }

    if (uri_is(u, "/api/config"))    return api_config_post(req);
    if (uri_is(u, "/api/ap"))        return api_ap_post(req);
    if (uri_is(u, "/api/wifi"))      return api_wifi_post(req);
    return send_error(req, "404 Not Found", "no such endpoint");
}

static esp_err_t delete_router(httpd_req_t *req)
{
    const char *u = req->uri;
    char tail[24];

    if (uri_is(u, "/api/graph/links")) return api_link_edit(req, false);
    if (uri_starts(u, "/api/signals/")) {
        uint16_t id = path_id(u, "/api/signals/", tail, sizeof(tail));
        if (id && !tail[0]) return api_signal_delete(req, id);
    }
    if (uri_starts(u, "/api/graph/nodes/")) {
        uint16_t id = path_id(u, "/api/graph/nodes/", tail, sizeof(tail));
        if (id && !tail[0]) return api_node_delete(req, id);
    }
    return send_error(req, "404 Not Found", "no such endpoint");
}

/* ------------------------------------------------------------------ static files */

static const char *content_type_for(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot) return "text/plain";
    if (!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".js"))   return "application/javascript";
    if (!strcasecmp(dot, ".mjs"))  return "application/javascript";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".map"))  return "application/json";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".jpg"))  return "image/jpeg";
    if (!strcasecmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(dot, ".webp")) return "image/webp";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    if (!strcasecmp(dot, ".woff2"))return "font/woff2";
    if (!strcasecmp(dot, ".txt"))  return "text/plain";
    if (!strcasecmp(dot, ".webmanifest")) return "application/manifest+json";
    return "text/plain";
}

/*
 * Captive-portal detection, by HOST rather than by path.
 *
 * The obvious implementation — a list of known probe paths to redirect — is what
 * this originally did, and it FLAPPED on iOS: the sheet opened and closed in a
 * loop. The reason is that every OS probes several URLs, and Apple alone uses
 * both /hotspot-detect.html and /library/test/success.html. A path list that
 * catches one but not the other answers 302 (captive!) to some probes and 200
 * with the SPA (not captive!) to others, and the phone oscillates between those
 * two verdicts forever.
 *
 * Matching the Host header instead is exhaustive by construction: any request
 * NOT addressed to this box is, by definition, a probe for somewhere on the real
 * internet, and gets redirected — no list to keep current, and every OS (iOS,
 * Android's generate_204, Windows NCSI) is handled by the same three lines.
 * Requests addressed to us are served normally, which is what lets the portal
 * page itself and its CSS/JS load once the sheet is open.
 */
static bool host_is_this_box(httpd_req_t *req, const char *ap_ip)
{
    char host[80];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK)
        return true;   /* no Host (HTTP/1.0): assume ours, never redirect blindly */

    char *colon = strchr(host, ':');   /* strip :port */
    if (colon) *colon = '\0';

    if (ap_ip && strcmp(host, ap_ip) == 0)
        return true;

    /* Reachable by mDNS name too, on the LAN side. */
    char mdns_name[LE_HOSTNAME_CMP_MAX];
    snprintf(mdns_name, sizeof(mdns_name), "%s.local",
             s_cfg->hostname[0] ? s_cfg->hostname : "klingelbox");
    if (strcasecmp(host, mdns_name) == 0)
        return true;

    char sta_ip[16];
    db_wifi_sta_ip(sta_ip);
    if (sta_ip[0] && strcmp(host, sta_ip) == 0)
        return true;

    return false;
}

static bool client_accepts_gzip(httpd_req_t *req)
{
    char enc[64];
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", enc, sizeof(enc)) != ESP_OK)
        return false;
    return strstr(enc, "gzip") != NULL;
}

/* Open `base`, preferring a pre-compressed "<base>.gz" when the client can take
 * it. Shipping the UI gzipped roughly triples what fits in the 1 MB `storage`
 * partition and cuts transfer time over the softAP. */
static FILE *open_asset(const char *base, bool gz_ok, bool *is_gz)
{
    char p[320];
    *is_gz = false;
    if (gz_ok) {
        snprintf(p, sizeof(p), "%s.gz", base);
        FILE *f = fopen(p, "r");
        if (f) { *is_gz = true; return f; }
    }
    return fopen(base, "r");
}

/*
 * Everything that is not an /api path: the SPA.
 *
 * Unknown paths fall back to index.html rather than 404ing, because the UI is a
 * client-routed single page — a reload on /signals/3 must still hand the browser
 * the app, which then routes itself. Only the /api tree is exempt, and it is
 * handled by the routers above: they are registered first and so match first.
 */
static esp_err_t static_router(httpd_req_t *req)
{
    if (db_wifi_mode() == DB_WIFI_RECOVERY) {
        char ip[16];
        db_wifi_ap_ip(ip);
        const char *ap_ip = ip[0] ? ip : s_cfg->ap_ip;
        if (!host_is_this_box(req, ap_ip)) {
            char loc[40];
            snprintf(loc, sizeof(loc), "http://%s/", ap_ip);
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", loc);
            /* No-store: a cached 302 would keep redirecting the phone to the
             * portal long after it left the recovery network. */
            httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
            httpd_resp_sendstr(req, "");
            return ESP_OK;
        }
    }

    char clean[192];
    strlcpy(clean, req->uri[0] ? req->uri : "/", sizeof(clean));
    char *q = strchr(clean, '?');
    if (q) *q = '\0';
    if (strcmp(clean, "/") == 0) strlcpy(clean, "/index.html", sizeof(clean));
    if (strstr(clean, ".."))     /* no traversal out of /spiffs */
        return send_error(req, "400 Bad Request", "bad path");

    char path[320];
    snprintf(path, sizeof(path), "/spiffs%s", clean);

    bool gz_ok = client_accepts_gzip(req);
    bool is_gz = false;
    FILE *f = open_asset(path, gz_ok, &is_gz);
    if (!f) {
        /* SPA fallback. A missing index.html means the storage partition is
         * blank or wrong — say so, because the API is still reachable and a
         * web-UI OTA is the fix. */
        f = open_asset("/spiffs/index.html", gz_ok, &is_gz);
        if (!f)
            return send_error(req, "404 Not Found",
                              "no web UI in the storage partition — flash one with "
                              "POST /api/ota/webui/upload");
        strlcpy(clean, "/index.html", sizeof(clean));
    }

    httpd_resp_set_type(req, content_type_for(clean));
    if (is_gz) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char chunk[1024];
    size_t r;
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, r) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------------------------ start */

static void mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 6,
        /* Never format on a failed mount: a corrupt image is recoverable with a
         * web-UI OTA, but silently erasing the partition destroys the evidence. */
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s) — the web UI is unavailable, "
                      "but the REST API is up and can be used to flash one",
                 esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
}

esp_err_t db_http_start(db_config_t *cfg)
{
    s_cfg = cfg;
    mount_spiffs();

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    /* Four routers are registered: GET, POST and DELETE on the /api wildcard,
     * plus GET on the root wildcard. The surface grows by endpoint rather than
     * by handler, but the headroom costs a few bytes and saves a silent
     * registration failure later. */
    hc.max_uri_handlers = 8;
    /* Wildcards are mandatory here: without them httpd matches literal URIs and
     * "/api/signals/3" could never be routed at all. Ids are parsed in path_id. */
    hc.uri_match_fn = httpd_uri_match_wildcard;
    hc.lru_purge_enable = true;
    /* cJSON recursion plus the graph/signal serializers on one worker stack;
     * 4 KB (the default) is not enough for the /api/graph and /api/diagnostics
     * trees, and a stack overflow here looks like a random reboot. */
    hc.stack_size = 10240;
    /* Browser OTA uploads push megabytes through one socket; the default 5 s
     * window is tight over a busy softAP. */
    hc.recv_wait_timeout = 15;
    hc.send_wait_timeout = 15;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &hc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Registration order IS match order: the /api routers must precede the
     * catch-all static handler, or every API call would be answered with the
     * SPA's index.html. */
    httpd_uri_t routes[] = {
        { .uri = "/api/*", .method = HTTP_GET,    .handler = get_router },
        { .uri = "/api/*", .method = HTTP_POST,   .handler = post_router },
        { .uri = "/api/*", .method = HTTP_DELETE, .handler = delete_router },
        { .uri = "/*",     .method = HTTP_GET,    .handler = static_router },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(srv, &routes[i]));

    ESP_LOGI(TAG, "HTTP server + REST API on :80 (http://%s.local/)", s_cfg->hostname);
    return ESP_OK;
}
