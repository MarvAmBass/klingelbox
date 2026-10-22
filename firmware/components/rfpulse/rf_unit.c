/*
 * rf_unit.c - Repeating-unit extraction and phase-free matching (see rf_unit.h).
 *
 * Implementation notes, each guarding against a failure mode that would look
 * like something else:
 *
 * 1. THE PERIOD SCAN GOES SMALLEST-FIRST AND STOPS AT THE FIRST WINNER. Any
 *    true period's multiples also pass the self-similarity test, so scanning
 *    upward and accepting the first qualifying period is what returns the
 *    FUNDAMENTAL rather than two or three words glued together. The converse
 *    trap — a sub-period of the word passing first — is blocked by the
 *    coverage bar: a too-small period's match run dies at the first
 *    inter-word gap (the gap pairs with a data pulse at that lag) and cannot
 *    explain the window, while the true period strides across every gap.
 *
 * 2. PERIODS ARE EVEN, BY CONSTRUCTION, NOT BY ASSUMPTION. Levels alternate
 *    strictly (rf_frame.h), so a pulse can only be "the same pulse one repeat
 *    later" if the lag preserves its level — i.e. the lag is even. Odd lags
 *    are not merely unlikely, they are impossible, so they are never scanned;
 *    that halves the work and removes a whole class of nonsense matches.
 *
 * 3. THE CANONICAL UNIT IS A MEDIAN, NOT A PICK. Every repeat in the window
 *    is an independent noisy sample of the same word, and the per-position
 *    median averages the capture jitter away while staying indifferent to one
 *    mangled repeat — the same reasoning as rf_normalize()'s cluster median.
 *    This is why a unit learned from any window of the stream is the SAME
 *    unit: the medians converge on the transmitter's timings, not on the
 *    accidents of one recording.
 *
 * 4. THE ROTATION PUTS THE GAP LAST, FOR EVERYTHING DOWNSTREAM. The longest
 *    LOW of the period is the inter-repeat gap; rotating the unit to end on
 *    it yields the word-then-framing-gap shape every other stored signal has,
 *    so replay (rf_transmit's framing-gap rule), storage and display need no
 *    special case. Matching never depends on the rotation — rf_unit_match()
 *    is cyclic — so even a pathological word whose longest LOW is ambiguous
 *    only risks a cosmetic rotation difference, never a missed match.
 */
#include <string.h>

#include "rf_unit.h"

/* The same two-tolerance per-pulse window as rf_frame.c's durations_similar,
 * restated here because that one is static and takes whole frames. Symmetric
 * (relative window against the larger value) for the same reason. */
static bool pulse_similar(uint32_t a, uint32_t b, uint8_t tol_pct, uint16_t tol_us)
{
    uint32_t diff  = (a > b) ? (a - b) : (b - a);
    uint32_t ref   = (a > b) ? a : b;
    uint32_t allow = (ref * (uint32_t)tol_pct) / 100u;

    if (allow < (uint32_t)tol_us) {
        allow = (uint32_t)tol_us;
    }
    return diff <= allow;
}

/* Insertion sort: per-position sample sets are tiny (one entry per repeat). */
static void sort_u16(uint16_t *a, uint16_t n)
{
    for (uint16_t i = 1; i < n; i++) {
        uint16_t v = a[i];
        uint16_t j = i;
        while (j > 0 && a[j - 1] > v) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = v;
    }
}

/* Median of a sorted array; the mean of the middle two when even, so two
 * repeats average their jitter instead of arbitrarily preferring one. */
static uint16_t median_u16(const uint16_t *sorted, uint16_t n)
{
    if (n == 0) {
        return 0;
    }
    if (n & 1u) {
        return sorted[n / 2];
    }
    return (uint16_t)(((uint32_t)sorted[n / 2 - 1] + sorted[n / 2]) / 2u);
}

bool rf_unit_extract(const rf_frame_t *frame, uint8_t tol_pct, uint16_t tol_us,
                     rf_unit_t *out)
{
    uint16_t n;

    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    rf_frame_reset(&out->unit);
    if (frame == NULL || frame->count > RF_FRAME_MAX_PULSES) {
        return false;
    }
    n = frame->count;
    if (n < 2u * RF_UNIT_MIN_PERIOD) {
        return false;   /* cannot hold two repeats of any acceptable period */
    }

    const uint16_t *d = frame->durations_us;
    uint16_t period = 0, span_lo = 0, span_hi = 0;   /* [span_lo, span_hi) */

    /* The scan starts BELOW the acceptance floor on purpose: it must find the
     * FUNDAMENTAL period, and then rf_unit.h's RF_UNIT_MIN_PERIOD judges it.
     * Starting the scan at the floor instead would dress a trivial rhythm up
     * as a floor-sized "word" (a bare 300/900 alternation scanned from 8
     * "repeats every 8 pulses" too) — and a window whose fundamental is that
     * short is rhythmic interference, not a transmitter's word. */
    for (uint16_t p = 2u; p <= n / 2u; p += 2u) {
        /* Longest contiguous run of i where pulse i and pulse i+p agree —
         * i.e. the stretch of the window that is periodic at lag p. */
        uint16_t run_s = 0, best_s = 0, best_len = 0, len = 0;

        for (uint16_t i = 0; i + p < n; i++) {
            bool ok = pulse_similar(d[i], d[i + p], tol_pct, tol_us);

            /* Clipped endpoints: the first pulse starts wherever the receiver
             * re-armed and the last ends wherever the recording was cut, so
             * SHORT against the periodic partner is expected there. Longer is
             * not — that is a different waveform. */
            if (!ok && i == 0 && d[0] < d[p]) {
                ok = true;
            }
            if (!ok && i + p == n - 1u && d[i + p] < d[i]) {
                ok = true;
            }

            if (ok) {
                if (len == 0) {
                    run_s = i;
                }
                len++;
                if (len > best_len) {
                    best_len = len;
                    best_s   = run_s;
                }
            } else {
                len = 0;
            }
        }

        /* best_len >= p means pulses [best_s, best_s+best_len+p) are one
         * stretch of >= 2 full periods. Coverage is judged over that whole
         * stretch against the whole window. */
        if (best_len >= p &&
            ((uint32_t)best_len + p) * 100u >= (uint32_t)n * RF_UNIT_COVERAGE_PCT) {
            period  = p;
            span_lo = best_s;
            span_hi = (uint16_t)(best_s + best_len + p);
            break;   /* smallest period first == the fundamental (note 1) */
        }
    }
    if (period == 0 || period < RF_UNIT_MIN_PERIOD) {
        return false;   /* no period, or a fundamental too short to be a word */
    }

    /* Trim the periodic stretch to whole repeats; the remainder at the end is
     * a partial repeat and contributes no sample of its missing positions. */
    uint16_t reps = (uint16_t)((span_hi - span_lo) / period);
    if (reps < 2) {
        return false;   /* unreachable given best_len >= p; refuse anyway */
    }

    /* Canonical durations: per-position median across the repeats (note 3). */
    uint16_t canon[RF_FRAME_MAX_PULSES / 2];   /* period <= n/2 <= 256 */
    uint16_t samples[RF_FRAME_MAX_PULSES / RF_UNIT_MIN_PERIOD + 1];

    for (uint16_t j = 0; j < period; j++) {
        uint16_t ns = 0;
        /* The window's clipped endpoints (see above) are real pulses cut
         * short; letting them vote would drag the median low. They are only
         * re-admitted in the degenerate case where a position has no honest
         * sample at all (both of its repeats sit on the window's edges). */
        for (int pass = 0; pass < 2 && ns == 0; pass++) {
            for (uint16_t k = 0; k < reps; k++) {
                uint16_t idx = (uint16_t)(span_lo + j + k * period);
                if (pass == 0 && (idx == 0 || idx == n - 1u)) {
                    continue;
                }
                samples[ns++] = d[idx];
            }
        }
        sort_u16(samples, ns);
        canon[j] = median_u16(samples, ns);
    }

    /* Rotate so the longest LOW — the inter-repeat gap — comes last (note 4). */
    uint16_t gap_j = 0;
    uint32_t gap_d = 0;
    for (uint16_t j = 0; j < period; j++) {
        if (rf_frame_level_at(frame, (uint16_t)(span_lo + j)) == 0 &&
            canon[j] >= gap_d) {
            gap_d = canon[j];
            gap_j = j;
        }
    }

    out->unit.first_level =
        rf_frame_level_at(frame, (uint16_t)(span_lo + (gap_j + 1u) % period));
    for (uint16_t j = 0; j < period; j++) {
        uint16_t src = (uint16_t)((gap_j + 1u + j) % period);
        if (!rf_frame_push(&out->unit, canon[src])) {
            rf_frame_reset(&out->unit);
            return false;   /* cannot happen: period <= n/2 */
        }
    }

    out->repeats      = reps;
    out->gap_us       = gap_d;
    out->coverage_pct = (uint8_t)(((uint32_t)(span_hi - span_lo) * 100u) / n);
    return true;
}

uint16_t rf_unit_match(const rf_frame_t *stream,
                       const uint16_t *unit_durations_us, uint16_t unit_count,
                       uint8_t unit_first_level,
                       uint8_t tol_pct, uint16_t tol_us)
{
    if (stream == NULL || unit_durations_us == NULL) {
        return 0;
    }
    uint16_t n = stream->count;
    uint16_t p = unit_count;
    if (p == 0 || n == 0 || n < p || n > RF_FRAME_MAX_PULSES ||
        p > RF_FRAME_MAX_PULSES || (p & 1u)) {
        return 0;   /* an odd "period" cannot preserve levels — see note 2 */
    }

    const uint16_t *d = stream->durations_us;
    uint16_t best = 0;

    for (uint16_t phi = 0; phi < p; phi++) {
        /* Level alignment: both sequences alternate, so agreeing at pulse 0
         * means agreeing everywhere — or nowhere. */
        if (rf_frame_level_at(stream, 0) !=
            (uint8_t)(unit_first_level ^ (phi & 1u))) {
            continue;
        }

        bool ok = true;
        uint16_t u = phi;
        for (uint16_t i = 0; i < n; i++) {
            uint32_t du = unit_durations_us[u];
            if (!pulse_similar(d[i], du, tol_pct, tol_us) &&
                /* the stream's clipped endpoints, as in extraction */
                !((i == 0 || i == n - 1u) && d[i] < du)) {
                ok = false;
                break;
            }
            u++;
            if (u == p) {
                u = 0;
            }
        }
        if (!ok) {
            continue;
        }

        /* FULL aligned repeats only: the partial copy before the first unit
         * boundary and after the last both matched above, but they are not
         * whole copies and must not inflate the caller's evidence count. */
        uint16_t lead = (uint16_t)((p - phi) % p);
        uint16_t reps = (uint16_t)((n - lead) / p);
        if (reps > best) {
            best = reps;
        }
    }
    return best;
}
