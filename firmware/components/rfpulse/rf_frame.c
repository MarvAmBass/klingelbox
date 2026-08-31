/*
 * rf_frame.c - The raw pulse-train container and its fuzzy comparison.
 *
 * WHY THIS FILE IS BORING ON PURPOSE. rf_frame_t is the one type every other
 * layer agrees on, so it must have no opinions: no protocol, no bits, no radio.
 * Anything clever added here would leak upward into layers that are supposed to
 * stay ignorant of it. The only non-trivial thing in this file is the comparison,
 * and that is here (rather than in rf_decode.c) precisely because comparing two
 * recordings must remain possible for frames no decoder understands.
 *
 * WHY THE COMPARISON IS FUZZY, AND WHY IT NEEDS *TWO* TOLERANCES. The remotes we
 * care about are driven by an RC oscillator with no crystal. The same button
 * pressed twice, seconds apart, produces pulse widths that differ by a few
 * percent; across a battery's life the drift is worse, and it is a *scale*
 * error, not an offset. A purely relative tolerance is therefore the right shape
 * — but applied to a 200 us pulse a 10 % window is only 20 us, which is inside
 * the quantisation and edge-detection noise of the capture path itself. Hence
 * the absolute floor: whichever window is more permissive wins. Getting this
 * wrong is not a subtle bug, it is "the doorbell never matches".
 *
 * PORTABILITY. No ESP-IDF, no FreeRTOS, no libc beyond <stdint.h>. This file is
 * compiled unchanged by the host test harness (host-test/) and by the firmware.
 */
#include <stddef.h>

#include "rf_frame.h"

bool rf_frame_push(rf_frame_t *f, uint16_t duration_us)
{
    if (f == NULL || f->count >= RF_FRAME_MAX_PULSES) {
        return false;
    }
    f->durations_us[f->count++] = duration_us;
    return true;
}

uint32_t rf_frame_duration_us(const rf_frame_t *f)
{
    uint32_t total = 0;

    if (f == NULL) {
        return 0;
    }
    for (uint16_t i = 0; i < f->count && i < RF_FRAME_MAX_PULSES; i++) {
        total += f->durations_us[i];
    }
    return total;
}

bool rf_frame_similar(const rf_frame_t *a, const rf_frame_t *b,
                      uint8_t tolerance_pct, uint16_t tolerance_us)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    /* Pulse count is the cheap discriminator and also the semantically correct
     * one: two recordings with different edge counts are different waveforms,
     * however close their individual timings happen to be. */
    if (a->count != b->count) {
        return false;
    }
    if (a->count == 0) {
        return true;   /* two empty frames are trivially the same recording */
    }
    if (a->count > RF_FRAME_MAX_PULSES) {
        return false;  /* corrupt input; refuse rather than read out of bounds */
    }

    /* NOTE: first_level is deliberately NOT compared. rf_capture arms on whatever
     * edge the receiver happens to see first, so the same transmission can be
     * recorded starting on either level; requiring them to agree would make
     * repeat detection fail at random. The duration sequence carries the
     * identity, the starting level does not. */
    for (uint16_t i = 0; i < a->count; i++) {
        uint32_t da = a->durations_us[i];
        uint32_t db = b->durations_us[i];
        uint32_t diff = (da > db) ? (da - db) : (db - da);

        /* Relative window is taken against the LARGER of the two so the test is
         * symmetric in its arguments — with the smaller value as reference,
         * similar(a,b) and similar(b,a) could disagree at the boundary. */
        uint32_t ref = (da > db) ? da : db;
        uint32_t allow = (ref * (uint32_t)tolerance_pct) / 100u;

        if (allow < (uint32_t)tolerance_us) {
            allow = (uint32_t)tolerance_us;   /* absolute floor wins when wider */
        }
        if (diff > allow) {
            return false;
        }
    }
    return true;
}
