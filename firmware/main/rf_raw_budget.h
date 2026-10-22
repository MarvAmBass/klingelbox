/*
 * rf_raw_budget.h - The memory-budget arithmetic of a raw capture session,
 *                   and nothing else.
 *
 * WHY A HEADER OF ITS OWN. rf_raw.c decides twice whether the heap can afford
 * something: once when a session starts (the scratch plus a small floor of
 * slots) and once per captured frame (one more slot). Both decisions used to be
 * inline comparisons, and the start-time one was wrong in the way that matters
 * most — it demanded room for all 32 slots up front, so a box that had drifted
 * down to ~86 KB free refused to record at all when it could have served a
 * smaller session perfectly well. The arithmetic is now in one place, pure
 * (stdint/stddef only, no IDF), so the host suite can pin the boundary cases
 * that are hardest to reproduce on hardware: exactly-at-the-reserve,
 * one-byte-short, and the overflow-free ordering of the comparisons.
 *
 * THE RESERVE IS THE CONTRACT. Every function here answers "may I take N bytes
 * and still leave `reserve` untouched?" — the reserve being what Wi-Fi, lwIP
 * and the open HTTP connection need to keep the box alive while somebody is
 * using it to diagnose a radio fault. All comparisons are written as
 * subtractions from `free_heap` AFTER a >= guard, never as additions on the
 * need side, so they cannot wrap on a size_t no matter what sizes are fed in.
 */
#ifndef DB_RF_RAW_BUDGET_H
#define DB_RF_RAW_BUDGET_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* May a session START: the analysis scratch plus `floor_slots` slots, with
 * `reserve` left over. This is the only gate that can refuse a session now —
 * and it asks for ~12 KB instead of the ~43 KB the old all-32 gate demanded. */
static inline bool db_raw_budget_start_ok(size_t free_heap, size_t scratch_bytes,
                                          size_t slot_bytes, int floor_slots,
                                          size_t reserve)
{
    size_t need = scratch_bytes + slot_bytes * (size_t)(floor_slots > 0 ? floor_slots : 0);
    return free_heap >= reserve && free_heap - reserve >= need;
}

/* May the session GROW by one slot right now. Asked per arriving frame once the
 * allocated slots are full; "no" is not final — the heap may free up (a closed
 * TLS connection, a finished OTA check) and the next frame asks again. */
static inline bool db_raw_budget_grow_ok(size_t free_heap, size_t slot_bytes,
                                         size_t reserve)
{
    return free_heap >= reserve && free_heap - reserve >= slot_bytes;
}

/* How many slots this session could reach IF the heap stays as it is: the ones
 * it already has plus what the budget would let it add, capped at `max_slots`.
 * This is what the UI's "recording (N of 32 frame slots available)" line shows
 * — a projection, honest to within the heap's natural wobble, which is why the
 * UI only shows it when it is below the maximum. */
static inline int db_raw_budget_slots_possible(size_t free_heap, size_t slot_bytes,
                                               size_t reserve, int have_slots,
                                               int max_slots)
{
    if (have_slots >= max_slots)
        return max_slots;
    if (free_heap < reserve || slot_bytes == 0)
        return have_slots;
    size_t room = (free_heap - reserve) / slot_bytes;
    if (room > (size_t)(max_slots - have_slots))
        room = (size_t)(max_slots - have_slots);
    return have_slots + (int)room;
}

#ifdef __cplusplus
}
#endif

#endif /* DB_RF_RAW_BUDGET_H */
