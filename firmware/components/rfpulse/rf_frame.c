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

/* The per-pulse tolerance test, over the first `count` pulses of both frames.
 * Shared by the two comparisons below; the caller has already decided how many
 * pulses the two frames are required to have. */
static bool durations_similar(const rf_frame_t *a, const rf_frame_t *b,
                              uint16_t count, uint8_t tolerance_pct,
                              uint16_t tolerance_us)
{
    /* NOTE: first_level is deliberately NOT compared. rf_capture arms on whatever
     * edge the receiver happens to see first, so the same transmission can be
     * recorded starting on either level; requiring them to agree would make
     * repeat detection fail at random. The duration sequence carries the
     * identity, the starting level does not. */
    for (uint16_t i = 0; i < count; i++) {
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
    return durations_similar(a, b, a->count, tolerance_pct, tolerance_us);
}

bool rf_frame_similar_tx_echo(const rf_frame_t *sent, const rf_frame_t *heard,
                              uint8_t tolerance_pct, uint16_t tolerance_us,
                              uint32_t capture_idle_us)
{
    if (sent == NULL || heard == NULL) {
        return false;
    }
    /* The verbatim comparison first: an echo captured under an idle threshold
     * LONGER than the sent frame's trailing gap (a relaxed raw session, or a
     * frame with no framing gap at all) arrives with the full pulse count. */
    if (rf_frame_similar(sent, heard, tolerance_pct, tolerance_us)) {
        return true;
    }

    /* The as-heard form. When the sent frame ends in a low at least as long as
     * the receiver's idle threshold, an echo of it can NEVER be captured whole:
     * that low is precisely what terminates the reception, and the capture path
     * drops the terminating silence. The echo therefore arrives exactly one
     * pulse short of what we sent, and the honest comparison is against the
     * sent frame with that trailing gap stripped. */
    if (capture_idle_us == 0u ||                       /* stripping disabled */
        sent->count == 0u || sent->count > RF_FRAME_MAX_PULSES ||
        (uint32_t)heard->count + 1u != (uint32_t)sent->count) {
        return false;
    }
    {
        uint16_t last = (uint16_t)(sent->count - 1u);
        uint32_t gap, allow;

        if (rf_frame_level_at(sent, last) != 0u) {
            return false;   /* ends with carrier on: there is no gap to strip */
        }
        /* Would this gap have tripped the idle threshold? Judge it with the
         * same tolerance window as any other pulse: the echoing device re-times
         * our gap with its own drifting oscillator, so a sent gap slightly
         * BELOW the threshold can still come back above it and be truncated.
         * Widening here cannot cause false suppression — a genuinely different
         * frame still has to match every one of the remaining pulses. */
        gap   = sent->durations_us[last];
        allow = (gap * (uint32_t)tolerance_pct) / 100u;
        if (allow < (uint32_t)tolerance_us) {
            allow = (uint32_t)tolerance_us;
        }
        if (gap + allow < capture_idle_us) {
            return false;   /* too short to have ended the echo's reception */
        }
    }
    return durations_similar(sent, heard, heard->count, tolerance_pct, tolerance_us);
}
