/*
 * rf_ev1527.c - The first decoder plugin, and the matching encoder.
 *
 * EV1527 (and its PT2262 / HS1527 / RT1527 relatives) is what is inside almost
 * every cheap wireless doorbell, gate fob and PIR sensor sold today. It sends a
 * sync gap followed by 24 bits: 20 bits of factory-burned transmitter address
 * and 4 bits of button/data. Each bit is a PAIR of pulses — short-then-long for
 * a 0, long-then-short for a 1 — with the long nominally three times the short.
 *
 * WHY THIS FILE IS A PLUGIN AND NOT THE ARCHITECTURE. Everything below it works
 * on raw timings and neither knows nor cares that EV1527 exists; a frame this
 * decoder declines stays fully recordable, matchable and replayable. Adding a
 * second protocol means adding a file like this one and one registry call — it
 * must never mean touching rf_frame.c or rf_capture.c.
 *
 * NOT A SINGLE MICROSECOND CONSTANT APPEARS IN THE DECODER. All the "known"
 * numbers (350 us short, 1050 us long, 10.8 ms sync gap) are properties of one
 * particular remote at one particular temperature and battery voltage. What is
 * actually invariant is the *ratio*, so the decoder works exclusively in the
 * multiples rf_normalize() learned from this specific capture: a bit is a pair
 * whose short half is 1x the learned base and whose long half is 2x..5x it. The
 * band is wide because real fobs are: nominal 3x, but 2.5x and 4x are both
 * commonplace, and the same fob shifts within its own battery's life. The ratio
 * is additionally cross-checked against the raw durations so a mis-estimated
 * base cannot manufacture a plausible-looking decode out of nothing.
 *
 * WHERE THE SYNC GAP ENDS UP, WHICH IS THE FIDDLY PART. A transmitting fob emits
 * ...gap, 24 bits, gap, 24 bits, gap... continuously while the button is held.
 * What the receiver hands us depends purely on when RMT armed and where its idle
 * threshold fell — and since the sync gap (~31x base, over 10 ms) is normally
 * LONGER than the capture idle threshold, the gap is usually the thing that ENDS
 * the frame rather than something inside it. So all four of these are ordinary,
 * expected shapes and all four must decode:
 *
 *     [gap][24 bit-pairs]              armed during the gap
 *     [24 bit-pairs][short][gap]       gap terminated the receive
 *     [24 bit-pairs]                   gap fell outside the capture entirely
 *     [partial bit][24 bit-pairs]      armed mid-bit
 *
 * Rather than enumerate those cases the decoder simply scans every plausible bit
 * boundary and accepts the first offset at which 24 consecutive well-formed
 * bit-pairs appear. A wrong offset is rejected by construction: it pairs the
 * long half of one bit with the short half of the next, which either violates
 * the 1x/2..5x shape or hits the sync gap.
 *
 * PORTABILITY. No ESP-IDF, no FreeRTOS. Host-compiled by host-test/.
 */
#include <stdio.h>
#include <string.h>

#include "rf_decode.h"

#define EV1527_BITS            24u
#define EV1527_PULSES          (EV1527_BITS * 2u)   /* two pulses per bit */

/* Long/short ratio accepted, in tenths. Nominal 3.0; the band covers both the
 * part-to-part spread of the encoder's RC oscillator and the asymmetry a lazy
 * OOK demodulator adds to the leading/trailing edges. */
#define EV1527_RATIO_MIN_T10   20u   /* 2.0x */
#define EV1527_RATIO_MAX_T10   50u   /* 5.0x */

/* Same band expressed as learned multiples (rf_normalize rounds to integers). */
#define EV1527_LONG_MULT_MIN   2u
#define EV1527_LONG_MULT_MAX   5u

/* The long half must be the SAME multiple throughout one frame — a transmitter
 * does not change its own ratio mid-word. +/-1 absorbs a rounding flip when the
 * true ratio sits near a half-integer (e.g. 3.5x). */
#define EV1527_LONG_MULT_SLOP  1u

/* Below this, rf_normalize is telling us the frame has no coherent timing
 * structure at all; decoding it could only produce a coincidence. */
#define EV1527_MIN_CONFIDENCE  30u

/*
 * Sync gap, in multiples of the short pulse, for the ENCODER only.
 *
 * The EV1527 datasheet's own preamble is 1 short high followed by 31 shorts of
 * low, and that 31 is what every commodity receiver's software (rc-switch and
 * the many firmwares derived from it) expects. Receivers key their frame
 * detection off "a low at least ~10x the short pulse", so 31 is comfortably
 * inside every implementation's window while still matching the reference part.
 * The decoder does NOT check for this number — see the module comment — it is
 * only what we emit so that other people's receivers recognise us.
 */
#define EV1527_SYNC_MULT       31u

#define EV1527_BASE_DEFAULT_US 350u
#define EV1527_BASE_MIN_US     50u
/* Capped so 31x the base still fits rf_frame_t's uint16 microsecond field. */
#define EV1527_BASE_MAX_US     2000u

/* ---- decoder ------------------------------------------------------------- */

/* Classify one pulse pair. Returns 0/1 for a valid bit, -1 when the pair is not
 * a bit at all. `*long_mult` receives the multiple of the long half so the
 * caller can enforce ratio consistency across the whole word. */
static int ev1527_classify_pair(const rf_frame_t *frame, const rf_norm_t *norm,
                                uint16_t i, uint32_t *long_mult)
{
    uint32_t ma = norm->mult[i];
    uint32_t mb = norm->mult[i + 1];
    uint32_t da = frame->durations_us[i];
    uint32_t db = frame->durations_us[i + 1];
    uint32_t lo, hi, lm;
    int bit;

    if (ma == 0 || mb == 0) {
        return -1;                      /* a timing outlier is never a bit */
    }
    if (ma == 1u && mb >= EV1527_LONG_MULT_MIN && mb <= EV1527_LONG_MULT_MAX) {
        bit = 0;                        /* short, long */
        lm = mb;
        lo = da; hi = db;
    } else if (mb == 1u && ma >= EV1527_LONG_MULT_MIN && ma <= EV1527_LONG_MULT_MAX) {
        bit = 1;                        /* long, short */
        lm = ma;
        lo = db; hi = da;
    } else {
        return -1;
    }

    /* Cross-check against the raw durations. The multiples came from an
     * estimated base; this does not, so a bad base estimate cannot fabricate a
     * bit that the actual waveform does not contain. */
    if (lo == 0) {
        return -1;
    }
    {
        uint32_t ratio_t10 = (hi * 10u) / lo;
        if (ratio_t10 < EV1527_RATIO_MIN_T10 || ratio_t10 > EV1527_RATIO_MAX_T10) {
            return -1;
        }
    }

    *long_mult = lm;
    return bit;
}

/* Try to read 24 bit-pairs starting at pulse index `start`. */
static bool ev1527_try_offset(const rf_frame_t *frame, const rf_norm_t *norm,
                              uint16_t start, uint32_t *out_code)
{
    uint32_t code = 0;
    uint32_t ref_long = 0;

    for (uint32_t b = 0; b < EV1527_BITS; b++) {
        uint32_t lm = 0;
        int bit = ev1527_classify_pair(frame, norm, (uint16_t)(start + b * 2u), &lm);

        if (bit < 0) {
            return false;
        }
        if (b == 0) {
            ref_long = lm;
        } else {
            uint32_t d = (lm > ref_long) ? (lm - ref_long) : (ref_long - lm);
            if (d > EV1527_LONG_MULT_SLOP) {
                return false;           /* the ratio changed mid-word: not one frame */
            }
        }
        code = (code << 1) | (uint32_t)bit;
    }
    *out_code = code & 0x00FFFFFFu;
    return true;
}

static bool ev1527_decode(const rf_frame_t *frame, const rf_norm_t *norm, rf_decoded_t *out)
{
    uint32_t code = 0;
    bool found = false;

    if (frame == NULL || norm == NULL || out == NULL) {
        return false;
    }
    if (norm->count != frame->count || frame->count < EV1527_PULSES) {
        return false;
    }
    if (norm->base_us == 0 || norm->confidence < EV1527_MIN_CONFIDENCE) {
        return false;
    }

    /* Scan every candidate bit boundary. A bit always begins on a HIGH (carrier
     * on) pulse, which halves the search and — more importantly — removes the
     * ambiguity between a frame and the same frame shifted by one pulse, where
     * every 0 would read as a 1. */
    uint16_t last_start = (uint16_t)(frame->count - EV1527_PULSES);
    for (uint16_t s = 0; s <= last_start; s++) {
        if (rf_frame_level_at(frame, s) != 1u) {
            continue;
        }
        if (ev1527_try_offset(frame, norm, s, &code)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    out->valid = true;
    (void)snprintf(out->protocol, sizeof(out->protocol), "%s", "ev1527");
    out->bit_count = (uint8_t)EV1527_BITS;
    out->code = (uint64_t)code;
    out->id = (code >> 4) & 0x000FFFFFu;      /* top 20 bits: transmitter address */
    out->button = (uint8_t)(code & 0x0Fu);    /* low 4 bits: button / data nibble */
    out->base_us = norm->base_us;
    (void)snprintf(out->text, sizeof(out->text), "EV1527 id=0x%05X btn=0x%X",
                   (unsigned)out->id, (unsigned)out->button);
    return true;
}

const rf_decoder_t rf_decoder_ev1527 = {
    .name = "ev1527",
    .decode = ev1527_decode,
};

/* ---- encoder ------------------------------------------------------------- */

/*
 * Emits sync-first: [1x high][31x low] then the 24 bit-pairs.
 *
 * Sync-first rather than sync-last matters for a SINGLE transmission, which is
 * what a "virtual signal" pairing burst may amount to if the user's receiver
 * latches on the first word it sees: a receiver that needs a preamble gets one
 * immediately instead of only after a full word has gone past it. When the
 * transmitter repeats (which rf_transmit does, and which real receivers usually
 * insist on), the gap that opens each copy also serves as the terminator of the
 * previous one, so the wire looks exactly like a genuine fob either way.
 */
bool rf_ev1527_build(uint32_t id20, uint8_t button4, uint16_t base_us, rf_frame_t *out)
{
    uint32_t base = (base_us == 0) ? EV1527_BASE_DEFAULT_US : base_us;
    uint32_t code;

    if (out == NULL) {
        return false;
    }
    if (base < EV1527_BASE_MIN_US) base = EV1527_BASE_MIN_US;
    if (base > EV1527_BASE_MAX_US) base = EV1527_BASE_MAX_US;

    code = ((id20 & 0x000FFFFFu) << 4) | (uint32_t)(button4 & 0x0Fu);

    rf_frame_reset(out);
    out->first_level = 1;                                   /* starts with carrier on */

    if (!rf_frame_push(out, (uint16_t)base)) return false;                    /* sync high */
    if (!rf_frame_push(out, (uint16_t)(base * EV1527_SYNC_MULT))) return false; /* sync gap */

    for (int b = (int)EV1527_BITS - 1; b >= 0; b--) {
        uint32_t bit = (code >> b) & 1u;
        uint16_t high = (uint16_t)(bit ? base * 3u : base);
        uint16_t low  = (uint16_t)(bit ? base : base * 3u);

        if (!rf_frame_push(out, high)) return false;
        if (!rf_frame_push(out, low)) return false;
    }
    return true;
}
