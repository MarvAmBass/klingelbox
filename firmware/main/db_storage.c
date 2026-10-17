/*
 * db_storage.c - see db_storage.h.
 *
 * NVS LAYOUT ("dbstor" namespace)
 *
 *     "trouble" -> [ trouble_rec_t ]   one fixed record, <64 bytes
 *
 * One record, one key. Overwritten by a newer failure, erased by the explicit
 * dismissal — it can never grow, which is a requirement and not an accident:
 * this module writes into the partition whose exhaustion it reports.
 */
#include "db_storage.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "db_storage";

#define DB_STOR_NS      "dbstor"
#define DB_STOR_KEY     "trouble"
#define DB_STOR_VERSION 1u

/* The on-flash record. Fixed-width fields, no pointers, 24 bytes — the whole
 * budget argument for being writable on a full partition rests on this staying
 * tiny, so think twice before adding a field. */
typedef struct {
    uint8_t version;                       /* DB_STOR_VERSION                 */
    uint8_t bucket;                        /* uptime bucket at failure time   */
    uint8_t reserved[2];
    int32_t err;                           /* esp_err_t of the failed write   */
    char    module[DB_STORAGE_MODULE_MAX]; /* NUL-terminated short tag        */
} trouble_rec_t;

/* Resident mirror of the record plus this boot's bookkeeping. */
static db_storage_trouble_t s_trouble;
static bool                 s_standing;      /* s_trouble is meaningful       */
static bool                 s_noted_this_boot;
static SemaphoreHandle_t    s_lock;
static StaticSemaphore_t    s_lock_buf;

/* Lazy like event_log's: a config save can fail (and want noting) before
 * app_main reaches db_storage_init(). */
static void lock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    xSemaphoreTake(s_lock, portMAX_DELAY);
}
static void unlock(void) { xSemaphoreGive(s_lock); }

/* Four decades of uptime, because "how long had it been running?" is the
 * question that separates "fills up over weeks" from "wedged at boot". */
static uint8_t bucket_now(void)
{
    int64_t up_s = esp_timer_get_time() / 1000000;
    if (up_s < 60)        return 0;
    if (up_s < 600)       return 1;
    if (up_s < 3600)      return 2;
    if (up_s < 86400)     return 3;
    return 4;
}

const char *db_storage_bucket_name(uint8_t bucket)
{
    switch (bucket) {
    case 0:  return "<1m";
    case 1:  return "1-10m";
    case 2:  return "10m-1h";
    case 3:  return "1-24h";
    case 4:  return ">24h";
    default: return "?";
    }
}

/*
 * Persist the breadcrumb, best-effort.
 *
 * WRITING INTO THE VERY NVS THAT IS FULL. This is expected to fail sometimes,
 * and the design reserves it the best possible odds instead of pretending:
 * the record is tiny (<64 B, a single NVS entry pair), and the PREVIOUS
 * breadcrumb is erased FIRST — NVS keeps the old and new copy of a key alive
 * until the commit, so overwriting in place needs room for both, while
 * erase-then-write needs room for one, and the erase itself is the one
 * operation a full partition can always absorb (it frees space, it never
 * consumes it). If the set still fails, the trouble stays in RAM and the
 * event ring for this boot and that is THE END OF IT: logged and dropped,
 * never retried, never reported back into this module — a breadcrumb writer
 * that notes its own failure is an infinite loop wearing out flash.
 */
static void persist_trouble(const trouble_rec_t *rec)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_STOR_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, DB_STOR_KEY);   /* make room; NOT_FOUND is fine */
        err = nvs_set_blob(h, DB_STOR_KEY, rec, sizeof(*rec));
        if (err == ESP_OK)
            err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK)
        ESP_LOGW(TAG, "breadcrumb could not be persisted (%s) — it survives "
                      "this boot in RAM only", esp_err_to_name(err));
}

void db_storage_init(void)
{
    nvs_handle_t h;
    if (nvs_open(DB_STOR_NS, NVS_READONLY, &h) != ESP_OK)
        return;   /* no namespace yet — no trouble ever recorded */

    trouble_rec_t rec;
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(h, DB_STOR_KEY, &rec, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(rec) || rec.version != DB_STOR_VERSION)
        return;   /* absent, or a future layout — never guess at one */
    rec.module[sizeof(rec.module) - 1] = '\0';

    lock();
    /* A failure noted BEFORE init (an early-boot config save) is newer than
     * anything a previous boot left behind — keep it. */
    if (!s_noted_this_boot) {
        s_trouble.prev_boot = true;
        snprintf(s_trouble.module, sizeof(s_trouble.module), "%s", rec.module);
        s_trouble.err    = (esp_err_t)rec.err;
        s_trouble.bucket = rec.bucket;
        s_standing = true;
    }
    bool adopted = !s_noted_this_boot;
    unlock();

    if (adopted) {
        ESP_LOGW(TAG, "storage trouble last boot: %s failed with %s (uptime %s)",
                 s_trouble.module, esp_err_to_name(s_trouble.err),
                 db_storage_bucket_name(s_trouble.bucket));
        /* The black-box readout: the boot after the outage says what the
         * outage was. Pushed on every boot the record stands — the record is
         * the user's to dismiss, and a feed that mentions it only once (on a
         * boot nobody was watching) would hide exactly what it exists to
         * show. */
        db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0,
                       "storage trouble last boot: %s, %s",
                       s_trouble.module, esp_err_to_name(s_trouble.err));
    }
}

void db_storage_note_write_failure(const char *module, esp_err_t err)
{
    /* Only the two storage-exhaustion verdicts. Everything else (transient
     * driver trouble, a bug) has its own per-site handling and would only
     * dilute the one record this module keeps. */
    if (err != ESP_ERR_NVS_NOT_ENOUGH_SPACE && err != ESP_ERR_NVS_NO_FREE_PAGES)
        return;
    if (!module || !module[0])
        module = "?";

    lock();
    if (s_noted_this_boot) {
        /* AT MOST ONCE PER BOOT, and the FIRST failure wins: once the
         * partition is full every writer starts failing, and the breadcrumb
         * should name the one that hit the wall first — the closest evidence
         * to the root cause — not whichever happened to fail last. */
        unlock();
        ESP_LOGD(TAG, "further storage failure (%s, %s) — breadcrumb already "
                      "written this boot", module, esp_err_to_name(err));
        return;
    }
    s_noted_this_boot = true;
    s_trouble.prev_boot = false;
    snprintf(s_trouble.module, sizeof(s_trouble.module), "%s", module);
    s_trouble.err    = err;
    s_trouble.bucket = bucket_now();
    s_standing = true;

    trouble_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.version = DB_STOR_VERSION;
    rec.bucket  = s_trouble.bucket;
    rec.err     = (int32_t)err;
    snprintf(rec.module, sizeof(rec.module), "%s", s_trouble.module);
    unlock();

    ESP_LOGE(TAG, "storage full: %s could not write (%s) — recording the "
                  "breadcrumb", rec.module, esp_err_to_name(err));
    db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0,
                   "storage full: %s could not write (%s)",
                   rec.module, esp_err_to_name(err));

    persist_trouble(&rec);
}

bool db_storage_get_trouble(db_storage_trouble_t *out)
{
    if (!out)
        return false;
    lock();
    bool standing = s_standing;
    if (standing)
        *out = s_trouble;
    unlock();
    return standing;
}

esp_err_t db_storage_clear_trouble(void)
{
    lock();
    s_standing = false;
    memset(&s_trouble, 0, sizeof(s_trouble));
    /* s_noted_this_boot deliberately stays: the once-per-boot budget is about
     * flash writes, and a dismissal must not re-arm a breadcrumb writer on a
     * partition that is still full. */
    unlock();

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_STOR_NS, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return ESP_OK;   /* no namespace, no record — already the asked state */
    err = nvs_erase_key(h, DB_STOR_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND)
        err = ESP_OK;
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* ---- host-test hook ------------------------------------------------------- */

#ifdef DB_HOSTTEST
/* Compiled ONLY into host-test/test_node_graph. Forgetting the resident state
 * — but not the fake flash — is what lets one test binary reboot and prove
 * the breadcrumb round-trips NVS. */
void db_storage_hosttest_reset(void)
{
    s_standing = false;
    s_noted_this_boot = false;
    memset(&s_trouble, 0, sizeof(s_trouble));
}
#endif
