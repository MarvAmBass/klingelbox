/*
 * event_log.c - The in-RAM activity ring (see event_log.h).
 *
 * Three properties are worth stating explicitly, because they are what let every
 * other module log into this thing without thinking about it:
 *
 * 1. NO ALLOCATION, EVER. The ring is a fixed array of fixed-size records with
 *    an inline text buffer. A doorbell press must never fail, or fragment the
 *    heap, because someone wanted a log line. The cost is that `text` is capped
 *    at DB_EVENT_TEXT_MAX and long messages are truncated — the right trade for
 *    a "what just happened?" view.
 *
 * 2. WRITES OVERWRITE THE OLDEST. A full ring is the normal state, not an error:
 *    the whole point is that only recent activity matters. The write index wraps
 *    and `s_total` counts everything ever written, which is what makes
 *    newest-first reads a simple bit of index arithmetic instead of a sort.
 *
 * 3. THE SERIAL IS THE TOTAL. db_events_serial() returns the number of events
 *    ever pushed, so the UI can poll GET /api/events?since=<serial> and skip
 *    re-rendering when the number has not moved. It is monotonic for the life of
 *    the boot and never reused.
 *
 * TASK SAFETY, NOT ISR SAFETY. A FreeRTOS mutex guards the ring, so any task may
 * push. It deliberately must NOT be called from an interrupt (a mutex cannot be
 * taken there) — ISRs in this firmware notify a task and the task logs, which is
 * what node_graph.c's GPIO path does.
 */
#include "event_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "db_events";

static db_event_t        s_ring[DB_EVENT_RING];
static uint32_t          s_write;      /* next slot to write                    */
static uint32_t          s_total;      /* events ever pushed == the serial      */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

void db_events_init(void)
{
    if (s_lock)
        return;   /* idempotent: a second init must not drop the ring */

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    memset(s_ring, 0, sizeof(s_ring));
    s_write = 0;
    s_total = 0;
    ESP_LOGI(TAG, "event ring ready (%d entries, %u bytes)",
             DB_EVENT_RING, (unsigned)sizeof(s_ring));
}

void db_events_push(db_event_kind_t kind, uint16_t signal_id, uint16_t node_id,
                    int rssi_dbm, uint8_t repeats, const char *fmt, ...)
{
    if (!s_lock)
        db_events_init();   /* a module logging before app_main got to us */

    /* Format outside the lock: vsnprintf into a local costs a little stack but
     * keeps the critical section down to a memcpy. */
    char text[DB_EVENT_TEXT_MAX];
    text[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof(text), fmt, ap);
        va_end(ap);
        text[sizeof(text) - 1] = '\0';
    }

    /* Clamp rather than wrap: an out-of-range RSSI is a bug elsewhere, and a
     * silently truncated int16 would hide it. */
    if (rssi_dbm > 32767)  rssi_dbm = 32767;
    if (rssi_dbm < -32768) rssi_dbm = -32768;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    db_event_t *e = &s_ring[s_write];
    e->ts_us     = esp_timer_get_time();
    e->kind      = (uint8_t)kind;
    e->signal_id = signal_id;
    e->node_id   = node_id;
    e->rssi_dbm  = (int16_t)rssi_dbm;
    e->repeats   = repeats;
    memcpy(e->text, text, sizeof(e->text));
    e->text[sizeof(e->text) - 1] = '\0';

    s_write = (s_write + 1u) % DB_EVENT_RING;
    s_total++;

    xSemaphoreGive(s_lock);
}

int db_events_get(db_event_t *out, int max)
{
    if (!out || max <= 0 || !s_lock)
        return 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    int have = (s_total < (uint32_t)DB_EVENT_RING) ? (int)s_total : DB_EVENT_RING;
    int n = (max < have) ? max : have;

    /* Newest first: walk backwards from the slot before the write cursor. */
    uint32_t idx = s_write;
    for (int i = 0; i < n; i++) {
        idx = (idx + DB_EVENT_RING - 1u) % DB_EVENT_RING;
        out[i] = s_ring[idx];
    }

    xSemaphoreGive(s_lock);
    return n;
}

uint32_t db_events_serial(void)
{
    /* A 32-bit aligned load is atomic on this target; taking the mutex here
     * would put a lock in the UI's cheap poll path for no added correctness. */
    return s_total;
}
