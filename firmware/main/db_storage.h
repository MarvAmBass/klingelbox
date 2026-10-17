/*
 * db_storage.h - The flash-storage black box recorder.
 *
 * WHY THIS EXISTS. The live box once "stopped working" with what looked like a
 * full NVS partition — and a restart cleared it, taking every trace of the
 * failure with it, because the event ring is deliberately RAM-only. Nothing on
 * the box could say afterwards WHICH module hit the wall, with WHICH error, or
 * how long the box had been up when it happened. This module is that missing
 * record: when any of our NVS writers fails with a storage-exhaustion error,
 * one tiny breadcrumb is persisted so the NEXT boot can explain the LAST one.
 *
 * WHAT IT IS NOT. Not a log (one record, overwritten, never appended — nothing
 * here can grow), not a retry mechanism (the per-site handling stays exactly
 * as it was; this only records), and not a gate on anything: a box whose
 * breadcrumb cannot even be written keeps ringing exactly as before, with the
 * trouble held in RAM and the event ring for as long as this boot lasts.
 *
 * THE RECORD IS TINY ON PURPOSE. It has to be written into the very partition
 * that just reported "full", so it is under 64 bytes, written AT MOST ONCE per
 * boot (the FIRST failure is the evidence closest to the root cause), and the
 * previous breadcrumb is erased before the new one is set — see the reasoning
 * at note_write_failure() in db_storage.c.
 */
#ifndef DB_STORAGE_H
#define DB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A module tag is a short lowercase word ("config", "signals", "graph",
 * "tls") — part of the on-flash record, so keep existing tags stable. */
#define DB_STORAGE_MODULE_MAX 16

/* The standing trouble record, for GET /api/system's storage.last_trouble. */
typedef struct {
    bool      prev_boot;                    /* true: read back from NVS at boot,
                                               i.e. the trouble was LAST boot's;
                                               false: it happened this boot    */
    char      module[DB_STORAGE_MODULE_MAX];
    esp_err_t err;                          /* the failing writer's verdict    */
    uint8_t   bucket;                       /* uptime bucket at failure time —
                                               see db_storage_bucket_name()   */
} db_storage_trouble_t;

/* Load any breadcrumb a previous boot left behind and, if one exists, push a
 * "storage trouble last boot: <module>, <err>" event so the activity feed
 * explains the outage the user is probably investigating. Call once at boot,
 * right after db_events_init(); a write failure noted BEFORE init (config
 * save during early boot) wins over the stored record — it is newer. */
void db_storage_init(void);

/* Record that an NVS write in `module` failed with `err`. Acts only on the
 * two storage-exhaustion verdicts (ESP_ERR_NVS_NOT_ENOUGH_SPACE,
 * ESP_ERR_NVS_NO_FREE_PAGES) so call sites can report every failure
 * unconditionally; anything else is a different bug with its own handling.
 * Safe from any task, never blocks on flash trouble, never recurses — a
 * failed breadcrumb write is logged and dropped. */
void db_storage_note_write_failure(const char *module, esp_err_t err);

/* The standing trouble record, if any. Returns false (out untouched) when
 * none stands. Safe from any task. */
bool db_storage_get_trouble(db_storage_trouble_t *out);

/* Dismiss the record: RAM and the persisted breadcrumb both. The DELETE
 * /api/storage/trouble acknowledgment — same contract as
 * db_tls_ack_rejection(): idempotent, and the dismissal outlives a reboot
 * because the NVS record goes with it. */
esp_err_t db_storage_clear_trouble(void);

/* Human/API name of an uptime bucket ("<1m" .. ">24h"). A bucket rather than
 * a timestamp because the box has no clock and an exact uptime would bloat
 * the record for precision nobody needs — "it died within a minute of boot"
 * versus "after a day" is the whole diagnostic question. */
const char *db_storage_bucket_name(uint8_t bucket);

#ifdef DB_HOSTTEST
/* Host tests only: forget the resident state (not the fake flash) so
 * db_storage_init() can run again, simulating a reboot. */
void db_storage_hosttest_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DB_STORAGE_H */
