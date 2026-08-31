/*
 * rf_decode.c - Timing normalization, fingerprinting, and the decoder registry.
 *
 * THE ONE IDEA IN THIS FILE: the base pulse width is a property of the CAPTURE,
 * not a constant of the protocol. Cheap 433 MHz remotes clock their encoder from
 * an RC oscillator trimmed by a single external resistor. Two "identical" fobs
 * off the same reel differ by 10 % or more; one fob drifts with temperature and
 * again as its battery sags. Any code that says `if (d > 300 && d < 400)` works
 * on the bench and fails in the porch. So we measure the base width from the
 * frame in front of us and express everything as a multiple of it. The famous
 * 1:3 short/long ratio of the EV1527 family then falls out as an *observation*
 * (mult 1 vs mult 3) rather than going in as an assumption.
 *
 * WHY A CLUSTER MEDIAN AND NOT THE MINIMUM. The obvious estimator — "base = the
 * shortest pulse" — is destroyed by a single spike. The OOK demodulator is
 * squelchless: with no carrier the CC1101's AGC winds up and the data line is
 * continuous noise, so one 40 us glitch riding on an otherwise perfect frame
 * would drag the base down by a factor of eight and every real pulse would then
 * quantize to a meaningless multiple. Instead we sort the durations, walk them
 * into anchored clusters (a cluster spans at most +CLUSTER_SPAN_PCT above its
 * own lowest member, so nothing chains across a genuine gap), and take the first
 * cluster with a real population. Its MEDIAN — not its mean — becomes the base,
 * because the median is indifferent to whatever the outliers do. A second,
 * least-squares-flavoured pass then refines the estimate using every pulse that
 * already fits a small multiple, which averages the jitter down across the
 * whole frame instead of trusting one cluster's worth of samples.
 *
 * WHY CONFIDENCE IS NOT JUST "FRACTION THAT FIT". If the estimated base is small
 * enough, *every* duration lands near some multiple of it — noise included. A
 * fit fraction alone therefore rates pure noise highly, which is exactly the
 * failure mode we must not have. The discriminator that actually separates
 * signal from noise is the SHAPE of the multiple histogram: a real OOK protocol
 * spends nearly all its pulses on two or three distinct symbol lengths, while
 * noise spreads across many. See rf_normalize() for the exact formula.
 *
 * PORTABILITY. No ESP-IDF, no FreeRTOS. Host-compiled by host-test/.
 */
#include <string.h>

#include "rf_decode.h"

/* ---- tuning constants (all reasoned about in the comments below) ---------- */

/* A cluster of "the same" pulse length spans from its lowest member up to
 * +35 %. Wide enough for the several-percent drift plus capture jitter of a real
 * remote; narrow enough that a 1x cluster can never swallow a 3x one (the
 * nearest competing symbol sits at +200 %). */
#define RF_CLUSTER_SPAN_PCT     35u
/* ...with an absolute floor so very short bases do not produce a zero-width
 * cluster window. */
#define RF_CLUSTER_SPAN_MIN_US  30u

/* Smallest population a cluster needs before we believe it is a symbol and not a
 * burst of glitches. Scaled to the frame (an EV1527 frame spends half its pulses
 * on the short symbol) and capped so long frames do not raise the bar absurdly. */
#define RF_CLUSTER_MIN_POP_DIV  10u
#define RF_CLUSTER_MIN_POP_LO   3u
#define RF_CLUSTER_MIN_POP_HI   8u

/* Residual a pulse may show against its nearest multiple. Expressed against the
 * BASE, not against the pulse: that keeps the window strictly narrower than
 * half a base, so "fits a multiple" stays a meaningful statement instead of
 * degenerating into "is a number". */
#define RF_FIT_TOL_PCT          35u
#define RF_FIT_TOL_FLOOR_US     40u
#define RF_FIT_TOL_CAP_PCT      45u   /* never reach base/2 - multiples must not overlap */

/* Multiples above this are treated as gaps and excluded from base refinement:
 * a 31x sync gap carries 31x the jitter and would dominate a least-squares fit. */
#define RF_REFINE_MAX_MULT      8u

/* Cluster tightness scale: a mean absolute deviation of this many percent of the
 * base scores zero tightness, 0 % scores full marks. */
#define RF_TIGHT_FULLSCALE_PCT  12u

#define RF_BASE_MIN_US          20u
#define RF_BASE_MAX_US          20000u

/* ---- small helpers ------------------------------------------------------- */

static uint32_t rf_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Shell sort with Ciura's gap sequence: no recursion, no allocation, no
 * worst-case blowup on the already-nearly-sorted inputs a clean capture
 * produces, and comfortably fast for the 512-element maximum. */
static void rf_sort_u16(uint16_t *a, uint16_t n)
{
    static const uint16_t gaps[] = { 301, 132, 57, 23, 10, 4, 1 };

    for (unsigned g = 0; g < sizeof(gaps) / sizeof(gaps[0]); g++) {
        uint16_t gap = gaps[g];
        if (gap >= n) {
            continue;
        }
        for (uint16_t i = gap; i < n; i++) {
            uint16_t tmp = a[i];
            uint16_t j = i;
            while (j >= gap && a[j - gap] > tmp) {
                a[j] = a[j - gap];
                j = (uint16_t)(j - gap);
            }
            a[j] = tmp;
        }
    }
}

/* Tolerance a pulse may deviate from `base * mult`. Deliberately independent of
 * `mult`: see RF_FIT_TOL_* above. */
static uint32_t rf_fit_tolerance(uint32_t base)
{
    uint32_t rel = (base * RF_FIT_TOL_PCT) / 100u;
    uint32_t cap = (base * RF_FIT_TOL_CAP_PCT) / 100u;
    uint32_t floor_us = RF_FIT_TOL_FLOOR_US;

    if (floor_us > cap) {
        floor_us = cap;          /* keep the window under half a base */
    }
    return (rel > floor_us) ? rel : floor_us;
}

/* ---- normalization ------------------------------------------------------- */

void rf_normalize(const rf_frame_t *frame, rf_norm_t *out)
{
    uint16_t sorted[RF_FRAME_MAX_PULSES];
    uint16_t hist[256];
    uint16_t n;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (frame == NULL || frame->count == 0) {
        return;
    }

    n = frame->count;
    if (n > RF_FRAME_MAX_PULSES) {
        n = RF_FRAME_MAX_PULSES;
    }
    out->count = n;

    /* --- 1. sort a working copy ------------------------------------------ */
    memcpy(sorted, frame->durations_us, (size_t)n * sizeof(uint16_t));
    rf_sort_u16(sorted, n);

    /* --- 2. find the dominant SHORT cluster ------------------------------ */
    uint32_t min_pop = n / RF_CLUSTER_MIN_POP_DIV;
    min_pop = rf_clamp_u32(min_pop, RF_CLUSTER_MIN_POP_LO, RF_CLUSTER_MIN_POP_HI);
    if (min_pop > n) {
        min_pop = n;
    }

    uint16_t c_lo = 0, c_hi = 0;   /* [c_lo, c_hi) into `sorted` */
    for (uint16_t i = 0; i < n; ) {
        uint32_t anchor = sorted[i];
        uint32_t span = (anchor * RF_CLUSTER_SPAN_PCT) / 100u;
        if (span < RF_CLUSTER_SPAN_MIN_US) {
            span = RF_CLUSTER_SPAN_MIN_US;
        }
        uint32_t limit = anchor + span;

        uint16_t j = i;
        while (j < n && (uint32_t)sorted[j] <= limit) {
            j++;
        }
        if ((uint32_t)(j - i) >= min_pop) {
            c_lo = i;
            c_hi = j;
            break;                 /* first populated cluster == the short symbol */
        }
        i = j;                     /* too sparse: a glitch burst, skip it */
    }
    if (c_hi == c_lo) {
        /* Nothing clustered at all — every duration is its own island. That is
         * the signature of noise. Fall back to the median of the whole frame so
         * callers still get a usable (if meaningless) base, and let the
         * confidence terms below report how bad it is. */
        c_lo = 0;
        c_hi = n;
    }

    uint32_t base = sorted[c_lo + (uint16_t)((c_hi - c_lo) / 2u)];   /* median */
    base = rf_clamp_u32(base, RF_BASE_MIN_US, RF_BASE_MAX_US);

    /* --- 3. refine: average the jitter out over the whole frame ---------- */
    {
        uint32_t tol = rf_fit_tolerance(base);
        uint32_t sum_d = 0, sum_m = 0;

        for (uint16_t i = 0; i < n; i++) {
            uint32_t d = frame->durations_us[i];
            uint32_t m = (d + base / 2u) / base;
            if (m == 0 || m > RF_REFINE_MAX_MULT) {
                continue;
            }
            uint32_t expect = m * base;
            uint32_t diff = (d > expect) ? (d - expect) : (expect - d);
            if (diff <= tol) {
                sum_d += d;
                sum_m += m;
            }
        }
        if (sum_m > 0) {
            uint32_t refined = (sum_d + sum_m / 2u) / sum_m;
            base = rf_clamp_u32(refined, RF_BASE_MIN_US, RF_BASE_MAX_US);
        }
    }
    out->base_us = (uint16_t)base;

    /* --- 4. quantize every pulse ----------------------------------------- */
    {
        uint32_t tol = rf_fit_tolerance(base);
        uint32_t fitted = 0;

        memset(hist, 0, sizeof(hist));
        for (uint16_t i = 0; i < n; i++) {
            uint32_t d = frame->durations_us[i];
            uint32_t m = (d + base / 2u) / base;
            uint32_t expect, diff;

            if (m == 0) {
                out->mult[i] = 0;          /* shorter than half a base: outlier */
                continue;
            }
            expect = m * base;
            diff = (d > expect) ? (d - expect) : (expect - d);
            if (diff > tol) {
                out->mult[i] = 0;          /* between multiples: outlier */
                continue;
            }
            if (m > 255u) {
                m = 255u;                  /* clamped, but still a valid gap */
            }
            out->mult[i] = (uint8_t)m;
            hist[m]++;
            fitted++;
        }

        /*
         * CONFIDENCE (0..100). Three independent things have to be true before a
         * normalization deserves to be trusted, so the score multiplies a
         * coverage term by a quality term rather than averaging everything:
         *
         *   fit_pct   how many pulses landed on SOME multiple within tolerance.
         *             Necessary but, on its own, easily faked by noise.
         *   top2_pct  how many pulses landed on the TWO most popular multiples.
         *             This is the real signal/noise discriminator: an OOK remote
         *             spends ~all of its pulses on two symbol lengths (short and
         *             long) plus one sync gap, so this is ~98 % for a good
         *             frame; broadband noise smears across many multiples and
         *             scores low.
         *   tight_pct how tightly the base cluster itself sits around the final
         *             base estimate (mean absolute deviation, full scale at
         *             RF_TIGHT_FULLSCALE_PCT). A wide cluster means the "base"
         *             is an average of things that are not the same symbol.
         *
         *   quality    = 0.60*top2 + 0.30*tight + 0.10     (small floor so a
         *                clean-but-unusual frame is never scored at zero)
         *   confidence = fit * quality
         *
         * Measured on synthetic frames (host-test/): a clean EV1527 frame scores
         * 98; the same frame with +/-8 % oscillator drift scores 84..93; 5000
         * uniform-noise frames never exceeded 53. Callers that want a hard
         * accept/reject line should therefore put it around 65 — comfortably
         * inside the gap from both sides.
         */
        uint32_t fit_pct = (fitted * 100u) / n;

        uint32_t top1 = 0, top2 = 0;
        for (unsigned m = 1; m < 256u; m++) {
            uint32_t c = hist[m];
            if (c > top1) { top2 = top1; top1 = c; }
            else if (c > top2) { top2 = c; }
        }
        uint32_t top2_pct = ((top1 + top2) * 100u) / n;

        uint32_t tight_pct = 0;
        if (c_hi > c_lo) {
            uint32_t dev = 0;
            for (uint16_t i = c_lo; i < c_hi; i++) {
                uint32_t d = sorted[i];
                dev += (d > base) ? (d - base) : (base - d);
            }
            dev /= (uint32_t)(c_hi - c_lo);                  /* mean abs deviation */
            uint32_t dev_pct = (dev * 100u) / base;
            if (dev_pct < RF_TIGHT_FULLSCALE_PCT) {
                tight_pct = ((RF_TIGHT_FULLSCALE_PCT - dev_pct) * 100u) / RF_TIGHT_FULLSCALE_PCT;
            }
        }

        uint32_t quality = (60u * top2_pct + 30u * tight_pct + 10u * 100u) / 100u;
        uint32_t conf = (fit_pct * quality) / 100u;

        /* Too few pulses to say anything statistical about. */
        if (n < 8) {
            conf = (conf * n) / 8u;
        }
        out->confidence = (uint8_t)rf_clamp_u32(conf, 0, 100);
    }
}

/* ---- fingerprint --------------------------------------------------------- */

#define RF_FNV1A_OFFSET  2166136261u
#define RF_FNV1A_PRIME   16777619u

static uint32_t rf_fnv1a(uint32_t h, uint8_t b)
{
    h ^= (uint32_t)b;
    h *= RF_FNV1A_PRIME;
    return h;
}

/* Long pulses are saturated to this class before hashing — see below. */
#define RF_FP_MULT_CAP 5u

/*
 * FNV-1a over the QUANTIZED structure, never over raw microseconds.
 *
 * The whole point of a fingerprint is that the same button pressed twice hashes
 * to the same value. Raw durations cannot do that — they drift by several
 * percent between presses — but the multiples they quantize to are integers and
 * are stable under exactly that drift. So we hash the pulse count followed by
 * mult[]: two recordings of one button agree bit-for-bit here even when not a
 * single duration matches.
 *
 * WITH ONE CORRECTION, LEARNED THE HARD WAY. Drift is a *relative* error, so it
 * scales with the multiple: at 8 % it moves a 1x pulse by a tenth of a base
 * (harmless) but a 31x sync gap by two and a half bases, which makes the gap's
 * exact multiple flap between 29 and 34 — and one flapping byte changes the
 * whole hash. Long pulses are therefore saturated to a single "this is a gap"
 * class before hashing: their precise length is simultaneously the least
 * reliably measured and the least identity-bearing quantity in the frame, while
 * the short/long symbols that actually carry the payload are kept exact. For the
 * same reason a pulse that failed the fit test is re-examined against its raw
 * magnitude, so a jittered gap lands in the gap class instead of being confused
 * with a sub-base glitch.
 *
 * FNV-1a is chosen for being four lines long, endian-independent as used here,
 * and well-behaved on short byte strings. There is no adversary; collision
 * resistance beyond "distinct remotes get distinct values" is not required, and
 * the caller prefers a decoded identity whenever a decoder claimed the frame.
 *
 * first_level is excluded for the same reason rf_frame_similar ignores it: the
 * receiver arms on whichever edge arrives first, so it is not a property of the
 * transmission.
 */
rf_fingerprint_t rf_fingerprint(const rf_frame_t *frame, const rf_norm_t *norm)
{
    uint32_t h = RF_FNV1A_OFFSET;
    uint16_t n;

    if (frame == NULL) {
        return 0;
    }
    n = frame->count;
    if (n > RF_FRAME_MAX_PULSES) {
        n = RF_FRAME_MAX_PULSES;
    }

    h = rf_fnv1a(h, (uint8_t)(n & 0xFFu));
    h = rf_fnv1a(h, (uint8_t)(n >> 8));

    if (norm != NULL && norm->count == n && norm->base_us > 0) {
        uint32_t base = norm->base_us;

        for (uint16_t i = 0; i < n; i++) {
            uint32_t m = norm->mult[i];

            if (m == 0) {
                /* Did not fit a multiple. Recover a coarse magnitude so an
                 * outlier that is plainly a long gap is not hashed as if it were
                 * a glitch (and vice versa). */
                m = ((uint32_t)frame->durations_us[i] + base / 2u) / base;
            }
            if (m > RF_FP_MULT_CAP) {
                m = RF_FP_MULT_CAP;
            }
            h = rf_fnv1a(h, (uint8_t)m);
        }
    } else {
        /* No normalization available. Coarse-quantize to 64 us buckets so the
         * result is at least drift-tolerant-ish; this path exists so callers
         * never have to special-case a missing norm, not because it is good. */
        for (uint16_t i = 0; i < n; i++) {
            h = rf_fnv1a(h, (uint8_t)((frame->durations_us[i] >> 6) & 0xFFu));
        }
    }
    return h;
}

/* ---- decoder registry ---------------------------------------------------- */

/*
 * A fixed-size table, populated once at startup and read-only thereafter. No
 * locking: registration happens before the capture task exists, and decoders are
 * required to be pure, so concurrent rf_decode() calls are safe by construction.
 * No dynamic allocation, deliberately — this table is walked per captured frame.
 */
#define RF_DECODER_MAX 8

static const rf_decoder_t *s_decoders[RF_DECODER_MAX];
static uint8_t s_decoder_count;

bool rf_decoder_register(const rf_decoder_t *decoder)
{
    if (decoder == NULL || decoder->decode == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < s_decoder_count; i++) {
        if (s_decoders[i] == decoder) {
            return true;       /* idempotent: re-registering is not an error */
        }
    }
    if (s_decoder_count >= RF_DECODER_MAX) {
        return false;
    }
    s_decoders[s_decoder_count++] = decoder;
    return true;
}

void rf_decoders_register_builtin(void)
{
    rf_decoder_register(&rf_decoder_ev1527);
}

bool rf_decode(const rf_frame_t *frame, const rf_norm_t *norm, rf_decoded_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (frame == NULL || norm == NULL) {
        return false;
    }

    for (uint8_t i = 0; i < s_decoder_count; i++) {
        /* Clear between attempts so a decoder that declines halfway through
         * cannot leave debris for the next one (or for the caller). */
        memset(out, 0, sizeof(*out));
        if (s_decoders[i]->decode(frame, norm, out)) {
            out->valid = true;
            return true;
        }
    }
    memset(out, 0, sizeof(*out));
    /* Not an error: "unknown protocol, still replayable" is a supported and
     * expected outcome — see UNKNOWN_PROTOCOL_RAW in the diagnostics table. */
    return false;
}
