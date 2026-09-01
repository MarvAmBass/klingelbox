/*
 * rf_group.h - Turning a pile of captured frames into ranked candidates.
 *
 * WHY THIS EXISTS. Registering a button used to require the frame to look like
 * the one protocol this box happens to decode: an admission gate demanded two
 * coalesced repeats AND a normalization confidence of 65 %, both numbers tuned
 * on EV1527-shaped waveforms. A remote that failed either was not merely
 * mis-decoded, it never became a candidate at all — the screen simply stayed
 * empty. That inverted the founding promise, which is that a frame nobody can
 * decode is still a first-class, replayable signal.
 *
 * So the gate is gone and this module replaces it. Detection is permissive and
 * protocol-agnostic: everything the radio hands up becomes a candidate. What
 * changes is the ORDER, and the evidence used to order it is evidence no
 * protocol knowledge is needed to collect:
 *
 *   A REAL REMOTE REPEATS ITSELF. Press a button and the transmitter sends the
 *   same word several times; band noise, by contrast, is never the same twice.
 *   Repetition is therefore the strongest available proof of authenticity, and
 *   it is available for a protocol we have never heard of. It dominates the
 *   score: rf_group_score() is built so that one additional repeat outranks
 *   every decode and confidence bonus combined (see the weights below). A
 *   decoded protocol and a clean base-width estimate only break ties among
 *   equally-repeated candidates.
 *
 * FRAGMENTATION, WHICH IS THE OTHER HALF OF THE PROBLEM. The capture layer ends
 * a frame after `idle_us` of silence. Set that below a transmitter's own
 * inter-word gap and ONE press is chopped into several dissimilar pieces, none
 * of which replays the whole thing — the user is left transmitting fragments one
 * by one hunting for the piece that rings the bell. rf_frag_find_runs() detects
 * exactly that shape, and the discriminator is precise rather than heuristic:
 *
 *   - the silence between the two frames was barely longer than the threshold
 *     that ended the first one (so the threshold cut it, rather than the
 *     transmitter stopping), AND
 *   - the two frames are NOT similar to each other.
 *
 * The second condition is what keeps honest repeats out of it. A remote whose
 * repeats are spaced just under `idle_us` also gets split — but into IDENTICAL
 * pieces, which is a repeat group and exactly what we want. Only dissimilar
 * neighbours indicate a word cut in half.
 *
 * A detected run can be stitched back together by rf_frame_join_run(), which
 * reinserts the silence that was actually measured between the pieces rather
 * than inventing a plausible-looking gap. Getting that wrong would produce a
 * waveform that no receiver reacts to while looking perfectly reasonable on a
 * screen, which is the worst kind of wrong for this feature.
 *
 * PORTABILITY. No ESP-IDF, no FreeRTOS, no allocation, no libc beyond
 * <string.h>. Compiled unchanged by host-test/, where all of the above is
 * exercised against synthetic frames.
 */
#ifndef RF_GROUP_H
#define RF_GROUP_H

#include <stdbool.h>
#include <stdint.h>

#include "rf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The most observations one analysis will consider. Matches the raw session's
 * slot count; callers with more must pass the first RF_GROUP_MAX_OBS. */
#define RF_GROUP_MAX_OBS 32

/*
 * Similarity window for "these two recordings are the same press again".
 *
 * Deliberately wider than the signal store's matching tolerance. The store is
 * answering "is this the button I registered?", where a false positive rings the
 * wrong bell; here the question is only "should these two rows be one row?",
 * where a false positive costs a merged count and a false NEGATIVE costs the
 * user the repeat evidence that ranking runs on. The two errors are not
 * symmetric, so neither is the tolerance.
 */
#define RF_GROUP_TOL_PCT 30
#define RF_GROUP_TOL_US  150

/*
 * How much silence still counts as "the threshold cut this frame".
 *
 * A frame ends after exactly `idle_us` of quiet, so any gap to the next frame is
 * at least that by construction. A gap of ROUGHLY idle_us means the transmitter
 * carried straight on and the boundary landed mid-transmission; a gap of many
 * times idle_us means it genuinely stopped. Two is a deliberately tight factor:
 * a false "fragmented" verdict tells the user to raise a threshold they did not
 * need to touch.
 */
#define RF_FRAG_GAP_FACTOR 2u

/* An inserted gap is one pulse duration, so it has to fit one. Also the RMT
 * capture resolution's ceiling, which is why no real gap we could have measured
 * exceeds it. */
#define RF_GROUP_MAX_GAP_US 32000u

/*
 * One captured frame, as offered to the analysis. `frame` is borrowed and must
 * outlive the call. `index` is whatever stable id the caller uses (the raw
 * session's 1-based frame index); this module only copies it around.
 *
 * `ts_us` is when the frame was COMPLETED, and `airtime_us` its own length, so
 * the silence before it is (ts_us - airtime_us) - previous ts_us. Keeping both
 * is what lets a join reconstruct a measured gap instead of guessing one.
 */
typedef struct {
    const rf_frame_t *frame;
    uint16_t index;
    int64_t  ts_us;
    uint32_t airtime_us;
    int16_t  rssi_dbm;
    uint8_t  confidence;
    bool     decoded_valid;
} rf_obs_t;

/* A set of observations that are the same waveform repeated. */
typedef struct {
    uint16_t rep;                        /* index into obs[] of the exemplar   */
    uint16_t count;                      /* how many times it was seen         */
    uint16_t member[RF_GROUP_MAX_OBS];   /* indices into obs[], arrival order  */
    int64_t  first_us;
    int64_t  last_us;
    int16_t  best_rssi_dbm;
    uint8_t  confidence;                 /* the exemplar's                     */
    bool     decoded_valid;              /* the exemplar's                     */
    uint16_t pulse_count;
    uint32_t score;                      /* rf_group_score(), precomputed      */
} rf_group_t;

/*
 * Score weights, stated here because the RELATION between them is the design
 * and a future edit must preserve it: REPEAT is larger than DECODED + the
 * largest possible CONF and LEN contributions put together, so an extra repeat
 * always outranks any amount of protocol knowledge. Undecoded candidates are
 * first-class; decoding only sorts equals.
 */
#define RF_SCORE_PER_REPEAT 1000u
#define RF_SCORE_REPEAT_CAP 20u     /* beyond this, more repeats prove nothing */
#define RF_SCORE_DECODED     400u
#define RF_SCORE_CONF_MUL      3u   /* x confidence (0..100)  => 0..300        */
#define RF_SCORE_LEN_CAP     200u   /* + min(pulse_count, 200)                 */

uint32_t rf_group_score(const rf_group_t *g);

/*
 * Group `n` observations by waveform similarity and rank them, best first.
 *
 * Returns how many groups were written (at most `max`, at most n). Groups are
 * sorted by score descending, ties broken by first arrival so the order is
 * stable across polls. Every observation lands in exactly one group: nothing is
 * filtered, ever — that is the entire point of this module.
 */
int rf_group_build(const rf_obs_t *obs, int n, rf_group_t *out, int max);

/* A run of consecutive frames that one transmission appears to have been cut
 * into. `gap_us[k]` is the measured silence BEFORE member k (gap_us[0] is 0). */
typedef struct {
    uint16_t member[RF_GROUP_MAX_OBS];
    uint32_t gap_us[RF_GROUP_MAX_OBS];
    uint16_t count;
    uint32_t total_pulses;   /* what a join would produce, gaps included */
    bool     joinable;       /* false when a join would not fit or not represent */
} rf_run_t;

/*
 * Find every run of >= 2 consecutive observations that look like one chopped
 * transmission, given the `idle_us` the session recorded with. Observations must
 * be in arrival order. Returns the number of runs written.
 */
int rf_frag_find_runs(const rf_obs_t *obs, int n, uint32_t idle_us,
                      rf_run_t *out, int max);

/*
 * Are two runs the same chopped transmission, seen twice?
 *
 * Press a button three times with the threshold set too short and you get three
 * runs, not one — and the merged candidate they stand for has been "seen 3x",
 * which is exactly the repeat evidence ranking needs. Answering this WITHOUT
 * materializing the joins is deliberate: a joined frame is a kilobyte, and a
 * session that allocated one per run would cost more RAM than the session
 * itself. Two runs match when they have the same number of pieces, each pair of
 * pieces is similar, and the silences between them agree within the same
 * tolerance (a drifting oscillator stretches the gaps as well as the pulses).
 */
bool rf_run_similar(const rf_obs_t *obs, const rf_run_t *a, const rf_run_t *b);

/*
 * out = a, then `gap_us` of silence, then b.
 *
 * Levels alternate strictly, so this is not a memcpy: if `a` already ends on a
 * LOW the gap EXTENDS that pulse rather than becoming a new one, and if `b`
 * begins on a LOW its first duration is absorbed into the gap for the same
 * reason. Getting this wrong yields a frame with two adjacent same-level pulses,
 * which replays as a different waveform entirely.
 *
 * False when either frame is empty, the result would exceed RF_FRAME_MAX_PULSES,
 * or a duration would exceed what one pulse can hold. `out` may not alias a or b.
 */
bool rf_frame_join(rf_frame_t *out, const rf_frame_t *a, uint32_t gap_us,
                   const rf_frame_t *b);

/* Stitch a whole run back into one frame using its measured gaps. False if any
 * step fails, in which case *out is left empty. */
bool rf_frame_join_run(rf_frame_t *out, const rf_obs_t *obs, const rf_run_t *run);

/*
 * How many pulses rf_frame_join_run() would produce, without producing them.
 *
 * Callers need this to describe a merged candidate before anyone asks for its
 * waveform, and adding the pieces up gives the wrong answer: a gap folds into a
 * trailing LOW instead of becoming a pulse, and a piece beginning on a LOW gives
 * its first duration to the gap. Being one or two out is not cosmetic — it is
 * the upper bound of the trim handles a user then drags. It lives here, next to
 * the join it predicts, so the two cannot drift; the host tests assert they
 * agree.
 */
uint16_t rf_run_joined_pulses(const rf_obs_t *obs, const rf_run_t *run);

#ifdef __cplusplus
}
#endif

#endif /* RF_GROUP_H */
