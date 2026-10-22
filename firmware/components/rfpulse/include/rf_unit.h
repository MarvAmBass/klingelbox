/*
 * rf_unit.h - Finding the repeating word inside a continuous transmission.
 *
 * WHY THIS EXISTS (field data, 2026-10). Some transmitters — the user's
 * "Gartentor" button is the live example — repeat their word CONTINUOUSLY for
 * as long as the button is held, with inter-repeat gaps shorter than any sane
 * capture idle threshold. The RMT framing therefore never splits the stream at
 * a word boundary: every "frame" is an arbitrary multi-word window, cut
 * wherever the max-pulse ceiling or a chance gap happens to land. Two
 * recordings of one press share not a single frame shape (the live box learned
 * the same button as 224 pulses one month and 257 the next), so nothing
 * frame-shaped — fingerprints, whole-frame similarity, prefix matching — can
 * ever be stable for such a stream. The commercial chime paired to that button
 * works fine, because it locks onto the REPEATING UNIT instead of any window
 * of it. This module does the same.
 *
 * Two primitives, both pure and host-tested:
 *
 *   rf_unit_extract()  given one window of the stream, find the smallest
 *                      self-similar repeating unit by scanning candidate
 *                      periods (an autocorrelation over pulse durations under
 *                      the ordinary two-tolerance window). The output unit is
 *                      CANONICAL: each pulse is the median of that pulse's
 *                      occurrences across every observed repeat, which beats
 *                      any single noisy capture — and it is rotated so the
 *                      inter-repeat gap (the longest LOW of the period) sits
 *                      LAST, i.e. the same word-then-framing-gap shape an
 *                      EV1527 frame has, so everything downstream (replay, the
 *                      transmit gap rule, drawing) treats it like any other
 *                      recording.
 *
 *   rf_unit_match()    the inverse: does a freshly captured window consist of
 *                      this unit repeated end to end, starting at ANY phase?
 *                      Phase-free by construction (the comparison is cyclic),
 *                      so it does not matter where the capture layer happened
 *                      to cut. Returns how many FULL aligned repeats the
 *                      window carries, because "how much evidence" is the
 *                      caller's matching policy, not this module's.
 *
 * A SECOND, TIGHTER FRAMING PASS WAS CONSIDERED AND REJECTED. Lowering the
 * idle threshold (globally or as a re-framing pass over the recorded window)
 * could split such a stream at its 1-2 ms inter-repeat gaps — but it would
 * also split EV1527-class frames at their intra-word gaps and destabilize the
 * framing that already works perfectly for them, and it still could not
 * produce a canonical unit (each split piece is one noisy sample). Period
 * detection over the unsplit window needs no threshold change, uses every
 * repeat as evidence, and leaves Haustür-class framing byte-for-byte alone.
 *
 * PORTABILITY. No ESP-IDF, no FreeRTOS, no allocation. Compiled unchanged by
 * host-test/, where the thresholds below are pinned against synthetic streams.
 */
#ifndef RF_UNIT_H
#define RF_UNIT_H

#include <stdbool.h>
#include <stdint.h>

#include "rf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * THE EXTRACTION THRESHOLDS, with provenance. All three must hold before a
 * window is believed to be "one unit, repeating":
 *
 *   RF_UNIT_MIN_PERIOD (8 pulses) — the shortest FUNDAMENTAL period accepted.
 *   A real OOK word carries an address, so even the most trivial protocol is
 *   dozens of pulses; the AGC's own hash and simple interference, by
 *   contrast, are rhythmic at very short periods. The scan always finds the
 *   fundamental (smallest period first) and a window whose fundamental is
 *   below this floor is REJECTED outright rather than rounded up to a
 *   floor-sized pseudo-word — a bare two-pulse rhythm "repeats every 8
 *   pulses" too, and accepting that would hand rhythmic interference a
 *   matchable identity.
 *
 *   >= 2 FULL REPEATS — one copy of anything proves nothing about
 *   repetition; two aligned copies inside one window is the same bar the old
 *   learn gate used for frames, applied where it actually belongs.
 *
 *   RF_UNIT_COVERAGE_PCT (80 %) — the periodic region must explain at least
 *   this much of the window. A window cut from a CONTINUOUS stream is
 *   periodic essentially end to end, so 80 % is generous (it tolerates one
 *   mangled repeat at an edge); a frame that merely CONTAINS a repeated
 *   figure — e.g. a word with internal structure, or the 224-pulse field
 *   stand-in whose mid-frame 12 ms silence caps any periodic run at ~67 % —
 *   stays below it and is correctly left alone. This is also what stops a
 *   sub-period of the word from winning: a too-small period's match run dies
 *   at the first inter-word gap and cannot reach the coverage bar, while the
 *   true period strides across every gap.
 */
#define RF_UNIT_MIN_PERIOD   8u
#define RF_UNIT_COVERAGE_PCT 80u

typedef struct {
    /* The canonical unit: median durations across all observed repeats,
     * rotated so the inter-repeat gap is the LAST pulse. */
    rf_frame_t unit;
    uint16_t   repeats;      /* full repeats observed in the analysed window */
    uint32_t   gap_us;       /* the unit's trailing inter-repeat silence     */
    uint8_t    coverage_pct; /* how much of the window the repeats explain   */
} rf_unit_t;

/*
 * Find the smallest repeating unit in `frame`. `tol_pct`/`tol_us` is the
 * ordinary per-pulse window (rf_frame.h) under which two pulses count as the
 * same pulse again — within ONE capture the oscillator barely moves, so
 * callers pass their "same transmission" window, not their cross-press one.
 *
 * Returns false when the frame contains no unit meeting the thresholds above,
 * which is the normal verdict for every single-word transmission (EV1527
 * included) and must be treated as "learn it as-is", never as an error.
 *
 * The first and last pulse of the frame are allowed to run SHORT against
 * their periodic partners: the capture layer re-arms on an edge and ends a
 * recording by dropping the terminating silence, so a window's endpoints are
 * routinely clipped mid-pulse.
 */
bool rf_unit_extract(const rf_frame_t *frame, uint8_t tol_pct, uint16_t tol_us,
                     rf_unit_t *out);

/*
 * How many full copies of a unit explain `stream` END TO END, starting at any
 * phase. 0 means the stream is NOT this unit repeating — one foreign pulse
 * anywhere refuses the whole window, because a similarity verdict that rings
 * a bell must rest on the entire compared waveform, never on a best effort.
 * The unit is passed as raw durations so callers may keep units in compact
 * side storage; rf_unit_match_frame() below wraps the common case.
 *
 * The stream's first and last pulse get the same clipped-endpoint allowance
 * as in extraction (short is expected, long is a different waveform).
 */
uint16_t rf_unit_match(const rf_frame_t *stream,
                       const uint16_t *unit_durations_us, uint16_t unit_count,
                       uint8_t unit_first_level,
                       uint8_t tol_pct, uint16_t tol_us);

static inline uint16_t rf_unit_match_frame(const rf_frame_t *stream,
                                           const rf_frame_t *unit,
                                           uint8_t tol_pct, uint16_t tol_us)
{
    return rf_unit_match(stream, unit->durations_us, unit->count,
                         unit->first_level, tol_pct, tol_us);
}

#ifdef __cplusplus
}
#endif

#endif /* RF_UNIT_H */
