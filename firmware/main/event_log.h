/*
 * event_log.h - A small in-RAM ring of recent activity, for the UI.
 *
 * Not persisted, and deliberately so: this exists to answer "what just
 * happened?" while someone is looking at the page — did my press arrive, did the
 * rule fire, did the transmit go out. Writing every doorbell press to flash
 * would wear NVS for data nobody reads a day later.
 *
 * The ring is the replacement for the M2 bring-up behaviour of dumping every
 * frame to the console.
 */
#ifndef DB_EVENT_LOG_H
#define DB_EVENT_LOG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DB_EVENT_RING     48
#define DB_EVENT_TEXT_MAX 72

typedef enum {
    DB_EV_RF_UNMATCHED = 0, /* a burst arrived that matches no stored signal */
    DB_EV_BUTTON_PRESS,     /* a stored signal was recognized               */
    DB_EV_WIRED_PRESS,      /* a wired GPIO input was pressed               */
    DB_EV_NODE_FIRED,       /* a graph node produced output                 */
    DB_EV_TRANSMIT,         /* a frame was sent                             */
    DB_EV_LEARN,            /* learn armed / candidate / accepted           */
    DB_EV_SYSTEM,           /* boot, radio state, errors                    */
} db_event_kind_t;

typedef struct {
    int64_t  ts_us;         /* esp_timer_get_time() */
    uint8_t  kind;          /* db_event_kind_t */
    uint16_t signal_id;     /* 0 if not applicable */
    uint16_t node_id;       /* 0 if not applicable */
    int16_t  rssi_dbm;      /* 0 if not applicable */
    uint8_t  repeats;
    char     text[DB_EVENT_TEXT_MAX];
} db_event_t;

void db_events_init(void);

/* Append an event. Safe from any task. `fmt` may be NULL. */
void db_events_push(db_event_kind_t kind, uint16_t signal_id, uint16_t node_id,
                    int rssi_dbm, uint8_t repeats, const char *fmt, ...);

/* Copy up to `max` events, NEWEST FIRST. Returns how many were written. */
int db_events_get(db_event_t *out, int max);

/* Monotonic counter, so the UI can poll cheaply and skip re-rendering when
 * nothing has changed. */
uint32_t db_events_serial(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_EVENT_LOG_H */
