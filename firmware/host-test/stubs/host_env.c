/*
 * host_env.c - the fake ESP environment behind the stub headers.
 *
 * THE FAKE NVS. A flat table of {namespace, key, bytes}, with the two
 * behaviours the persistence tests actually hinge on modelled faithfully:
 *
 *  - ENTRY ACCOUNTING. Real NVS charges 32-byte entries: data rounded up,
 *    plus per-chunk and index overhead. cost() below uses the same shape
 *    (3 + ceil(len/32)) so a test can dial the capacity to just under what an
 *    operation needs and watch the firmware's own budget check fire first.
 *
 *  - REPLACE NEEDS ROOM FOR BOTH COPIES. Real NVS writes the new value before
 *    erasing the old, so overwriting a blob on a full partition fails even
 *    when the new value is SMALLER. That property is why db_signals_delete()
 *    grew its erase-frame-first fallback, and a fake that quietly replaced
 *    in place would have hidden the entire bug class this suite exists for.
 *
 * FAILURE INJECTION is a one-shot countdown per operation type: precise
 * enough to hit "the second nvs_set_blob of this mutation" (the index write
 * after the frame write), and self-disarming so the test can then prove the
 * rolled-back state is retryable.
 */
#include "host_env.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_diag.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "event_log.h"
#include "nvs.h"

/* ---- fake flash ----------------------------------------------------------- */

#define HOST_NVS_MAX_ITEMS 256
#define HOST_NVS_MAX_NS    8

typedef struct {
    char    ns[NVS_NS_NAME_MAX_SIZE];
    char    key[NVS_KEY_NAME_MAX_SIZE];
    uint8_t *data;
    size_t  len;
    int     used;
} item_t;

static item_t s_items[HOST_NVS_MAX_ITEMS];
static char   s_namespaces[HOST_NVS_MAX_NS][NVS_NS_NAME_MAX_SIZE];
static int    s_ns_count;

static size_t s_capacity;          /* 0 = unlimited */
static int    s_fail_set_countdown, s_fail_set_err;
static int    s_fail_erase_countdown, s_fail_erase_err;

static size_t cost(size_t len) { return 3u + (len + 31u) / 32u; }

static item_t *find_item(const char *ns, const char *key)
{
    for (int i = 0; i < HOST_NVS_MAX_ITEMS; i++)
        if (s_items[i].used && strcmp(s_items[i].ns, ns) == 0 &&
            strcmp(s_items[i].key, key) == 0)
            return &s_items[i];
    return NULL;
}

static const char *ns_of_handle(nvs_handle_t h)
{
    int idx = (int)h - 1;
    return (idx >= 0 && idx < s_ns_count) ? s_namespaces[idx] : NULL;
}

void host_nvs_reset(void)
{
    for (int i = 0; i < HOST_NVS_MAX_ITEMS; i++) {
        free(s_items[i].data);
        memset(&s_items[i], 0, sizeof(s_items[i]));
    }
    memset(s_namespaces, 0, sizeof(s_namespaces));
    s_ns_count = 0;
    s_capacity = 0;
    s_fail_set_countdown = s_fail_erase_countdown = 0;
}

void host_nvs_set_capacity(size_t entries) { s_capacity = entries; }

size_t host_nvs_used_entries(void)
{
    size_t used = 0;
    for (int i = 0; i < HOST_NVS_MAX_ITEMS; i++)
        if (s_items[i].used)
            used += cost(s_items[i].len);
    return used;
}

size_t host_nvs_available_entries(void)
{
    if (s_capacity == 0)
        return 100000;   /* "unlimited": far past any budget the firmware asks for */
    size_t used = host_nvs_used_entries();
    return (s_capacity > used) ? s_capacity - used : 0;
}

void host_nvs_fail_set_blob(int countdown, int err)
{
    s_fail_set_countdown = countdown;
    s_fail_set_err = err;
}

void host_nvs_fail_erase(int countdown, int err)
{
    s_fail_erase_countdown = countdown;
    s_fail_erase_err = err;
}

/* ---- the nvs.h surface ---------------------------------------------------- */

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t mode,
                   nvs_handle_t *out_handle)
{
    if (!namespace_name || !out_handle)
        return ESP_ERR_INVALID_ARG;

    for (int i = 0; i < s_ns_count; i++) {
        if (strcmp(s_namespaces[i], namespace_name) == 0) {
            *out_handle = (nvs_handle_t)(i + 1);
            return ESP_OK;
        }
    }
    /* Real NVS: a namespace does not exist until something is written to it,
     * and opening a missing one read-only reports NOT_FOUND — the code path
     * signal_store's "no stored signals yet" boot relies on. */
    if (mode == NVS_READONLY)
        return ESP_ERR_NVS_NOT_FOUND;
    if (s_ns_count >= HOST_NVS_MAX_NS)
        return ESP_ERR_NO_MEM;
    snprintf(s_namespaces[s_ns_count], sizeof(s_namespaces[0]), "%s",
             namespace_name);
    *out_handle = (nvs_handle_t)(++s_ns_count);
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { (void)handle; }

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    const char *ns = ns_of_handle(handle);
    if (!ns || !key || (!value && length))
        return ESP_ERR_INVALID_ARG;

    if (s_fail_set_countdown > 0 && --s_fail_set_countdown == 0)
        return (esp_err_t)s_fail_set_err;

    /* Both copies coexist until the commit — see the file header. */
    if (s_capacity != 0 &&
        host_nvs_used_entries() + cost(length) > s_capacity)
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;

    uint8_t *copy = malloc(length ? length : 1);
    if (!copy)
        return ESP_ERR_NO_MEM;
    memcpy(copy, value, length);

    item_t *it = find_item(ns, key);
    if (!it) {
        for (int i = 0; i < HOST_NVS_MAX_ITEMS; i++)
            if (!s_items[i].used) { it = &s_items[i]; break; }
        if (!it) {
            free(copy);
            return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
        }
        it->used = 1;
        snprintf(it->ns, sizeof(it->ns), "%s", ns);
        snprintf(it->key, sizeof(it->key), "%s", key);
        it->data = NULL;
    }
    free(it->data);
    it->data = copy;
    it->len  = length;
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    const char *ns = ns_of_handle(handle);
    if (!ns || !key || !length)
        return ESP_ERR_INVALID_ARG;

    item_t *it = find_item(ns, key);
    if (!it)
        return ESP_ERR_NVS_NOT_FOUND;
    if (!out_value) {
        *length = it->len;
        return ESP_OK;
    }
    if (*length < it->len)
        return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out_value, it->data, it->len);
    *length = it->len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key)
{
    const char *ns = ns_of_handle(handle);
    if (!ns || !key)
        return ESP_ERR_INVALID_ARG;

    if (s_fail_erase_countdown > 0 && --s_fail_erase_countdown == 0)
        return (esp_err_t)s_fail_erase_err;

    item_t *it = find_item(ns, key);
    if (!it)
        return ESP_ERR_NVS_NOT_FOUND;
    free(it->data);
    memset(it, 0, sizeof(*it));
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t nvs_find_key(nvs_handle_t handle, const char *key, nvs_type_t *out_type)
{
    const char *ns = ns_of_handle(handle);
    if (!ns || !key)
        return ESP_ERR_INVALID_ARG;
    if (!find_item(ns, key))
        return ESP_ERR_NVS_NOT_FOUND;
    if (out_type)
        *out_type = NVS_TYPE_BLOB;
    return ESP_OK;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *nvs_stats)
{
    (void)part_name;
    if (!nvs_stats)
        return ESP_ERR_INVALID_ARG;
    size_t used = host_nvs_used_entries();
    size_t avail = host_nvs_available_entries();
    nvs_stats->used_entries      = used;
    nvs_stats->available_entries = avail;
    nvs_stats->free_entries      = avail;
    nvs_stats->total_entries     = used + avail;
    nvs_stats->namespace_count   = (size_t)s_ns_count;
    return ESP_OK;
}

/* ---- iteration ------------------------------------------------------------ */

struct nvs_opaque_iterator_t {
    char ns[NVS_NS_NAME_MAX_SIZE];
    int  idx;   /* index of the item currently pointed at */
};

static int next_index(const char *ns, int from)
{
    for (int i = from; i < HOST_NVS_MAX_ITEMS; i++)
        if (s_items[i].used && strcmp(s_items[i].ns, ns) == 0)
            return i;
    return -1;
}

esp_err_t nvs_entry_find(const char *part_name, const char *namespace_name,
                         nvs_type_t type, nvs_iterator_t *output_iterator)
{
    (void)part_name; (void)type;   /* everything stored here is a blob */
    if (!namespace_name || !output_iterator)
        return ESP_ERR_INVALID_ARG;
    *output_iterator = NULL;

    int idx = next_index(namespace_name, 0);
    if (idx < 0)
        return ESP_ERR_NVS_NOT_FOUND;

    nvs_iterator_t it = malloc(sizeof(*it));
    if (!it)
        return ESP_ERR_NO_MEM;
    snprintf(it->ns, sizeof(it->ns), "%s", namespace_name);
    it->idx = idx;
    *output_iterator = it;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *iterator)
{
    if (!iterator || !*iterator)
        return ESP_ERR_INVALID_ARG;
    int idx = next_index((*iterator)->ns, (*iterator)->idx + 1);
    if (idx < 0) {
        /* Real behaviour: the exhausted iterator is released and NULLed. */
        free(*iterator);
        *iterator = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    (*iterator)->idx = idx;
    return ESP_OK;
}

esp_err_t nvs_entry_info(nvs_iterator_t iterator, nvs_entry_info_t *out_info)
{
    if (!iterator || !out_info)
        return ESP_ERR_INVALID_ARG;
    const item_t *it = &s_items[iterator->idx];
    snprintf(out_info->namespace_name, sizeof(out_info->namespace_name), "%s", it->ns);
    snprintf(out_info->key, sizeof(out_info->key), "%s", it->key);
    out_info->type = NVS_TYPE_BLOB;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t iterator) { free(iterator); }

/* ---- clock, RNG, logging, sinks ------------------------------------------- */

static int64_t s_now_us = 1000000;   /* non-zero: 0 often means "never" */

int64_t esp_timer_get_time(void) { return s_now_us; }
void host_time_advance_us(int64_t us) { s_now_us += us; }

uint32_t esp_random(void)
{
    /* xorshift32: deterministic across runs, unlike rand() across libcs. */
    static uint32_t x = 0x12345678u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

void host_log(const char *tag, const char *fmt, ...)
{
    if (!getenv("HOSTTEST_LOG"))
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK:                       return "ESP_OK";
    case ESP_FAIL:                     return "ESP_FAIL";
    case ESP_ERR_NO_MEM:               return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG:          return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE:        return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE:         return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND:            return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NVS_NOT_FOUND:        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    default:                           return "ESP_ERR_?";
    }
}

/* The event ring and the diag table are observability, not logic — the tests
 * assert on stored state, so both sinks reduce to the (optional) debug log. */
void db_events_push(db_event_kind_t kind, uint16_t signal_id, uint16_t node_id,
                    int rssi_dbm, uint8_t repeats, const char *fmt, ...)
{
    (void)kind; (void)signal_id; (void)node_id; (void)rssi_dbm; (void)repeats;
    if (!getenv("HOSTTEST_LOG") || !fmt)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[event] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void db_diag_report(db_diag_t state, const char *fmt, ...)
{
    (void)state;
    if (!getenv("HOSTTEST_LOG") || !fmt)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[diag] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}
