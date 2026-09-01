/*
 * test_rf_decode.c - Host-compiled tests for the protocol-agnostic pulse logic.
 *
 * WHY OFF-TARGET TESTS EXIST FOR THIS AND NOTHING ELSE. The parts of rfpulse
 * that are hard to get right are the parts that have no hardware in them: the
 * base-width estimator, the confidence score, the fingerprint's stability under
 * drift, and the EV1527 bit framing. Debugging those on a device means flashing,
 * pressing a physical button, and hoping the RF environment cooperates — a loop
 * measured in minutes with no way to inject a controlled 8 % oscillator drift.
 * Here the same code runs in milliseconds against synthetic frames whose exact
 * timings we choose, including the pathological ones a real remote produces only
 * on a cold morning with a flat battery.
 *
 * This binary deliberately links ONLY rf_frame.c, rf_decode.c and rf_ev1527.c.
 * If it ever stops compiling because one of them grew an ESP-IDF include, that
 * is the layering violation the test is also there to catch.
 *
 * Build and run:  make test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf_decode.h"
#include "rf_frame.h"
#include "rf_group.h"

/* ---- micro test harness -------------------------------------------------- */

static int g_pass;
static int g_fail;
static const char *g_case = "";

#define CASE(name) do { g_case = (name); } while (0)

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            printf("  FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* ---- deterministic helpers ---------------------------------------------- */

/* A tiny LCG. Deterministic on every platform, so a failure is reproducible
 * rather than "it went red on CI once". */
static uint32_t g_rng = 12345u;

static void rng_seed(uint32_t s) { g_rng = s ? s : 1u; }

static uint32_t rng_next(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 8) & 0x00FFFFFFu;
}

/* Uniform in [lo, hi]. */
static uint32_t rng_range(uint32_t lo, uint32_t hi)
{
    return lo + rng_next() % (hi - lo + 1u);
}

/* Scale every duration by a random factor within +/- pct. This models the thing
 * that actually breaks naive decoders: an RC oscillator that is off by a few
 * percent, pulse to pulse and press to press. */
static void jitter_frame(rf_frame_t *f, uint32_t pct)
{
    for (uint16_t i = 0; i < f->count; i++) {
        uint32_t d = f->durations_us[i];
        int32_t delta = (int32_t)rng_range(0, 2u * pct) - (int32_t)pct;
        int32_t nd = (int32_t)d + (int32_t)(d * (uint32_t)(delta < 0 ? -delta : delta) / 100u)
                     * (delta < 0 ? -1 : 1);
        if (nd < 1) nd = 1;
        if (nd > 65535) nd = 65535;
        f->durations_us[i] = (uint16_t)nd;
    }
}

/* Copy a sub-range of pulses into a new frame, fixing up first_level so the
 * strict alternation invariant still holds. Used to synthesize the four frame
 * shapes rf_capture can legitimately hand up (see rf_ev1527.c). */
static void slice_frame(const rf_frame_t *src, uint16_t from, uint16_t n, rf_frame_t *dst)
{
    rf_frame_reset(dst);
    dst->first_level = rf_frame_level_at(src, from);
    for (uint16_t i = 0; i < n; i++) {
        rf_frame_push(dst, src->durations_us[from + i]);
    }
}

/* ---- tests --------------------------------------------------------------- */

static void test_build_decode_roundtrip(void)
{
    static const uint32_t ids[]   = { 0x12345u, 0x00001u, 0xFFFFFu, 0xA5A5Au, 0x7C3E1u };
    static const uint8_t buttons[] = { 0x1u, 0x4u, 0x8u, 0xFu, 0x2u };
    static const uint16_t bases[]  = { 0u /* default */, 200u, 350u, 450u, 800u };

    CASE("ev1527 build -> normalize -> decode round-trip");

    for (unsigned bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++) {
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            rf_frame_t f;
            rf_norm_t n;
            rf_decoded_t d;
            uint16_t expect_base = bases[bi] ? bases[bi] : 350u;

            CHECK(rf_ev1527_build(ids[i], buttons[i], bases[bi], &f),
                  "build failed for id=0x%05X base=%u", ids[i], bases[bi]);
            CHECK(f.count == 50, "expected 50 pulses, got %u", f.count);
            CHECK(f.first_level == 1, "expected first_level=1, got %u", f.first_level);

            rf_normalize(&f, &n);
            CHECK(n.base_us == expect_base, "base %u, expected %u", n.base_us, expect_base);
            CHECK(n.confidence >= 90, "clean frame confidence %u < 90", n.confidence);

            CHECK(rf_decode(&f, &n, &d), "decode declined id=0x%05X base=%u",
                  ids[i], bases[bi]);
            CHECK(d.valid && strcmp(d.protocol, "ev1527") == 0,
                  "protocol '%s'", d.protocol);
            CHECK(d.id == ids[i], "id 0x%05X != 0x%05X", (unsigned)d.id, ids[i]);
            CHECK(d.button == buttons[i], "button 0x%X != 0x%X", d.button, buttons[i]);
            CHECK(d.bit_count == 24, "bit_count %u", d.bit_count);
            CHECK(d.code == (((uint64_t)ids[i] << 4) | buttons[i]),
                  "code 0x%06lX", (unsigned long)d.code);
            CHECK(d.base_us == expect_base, "decoded base %u", d.base_us);
            CHECK(strstr(d.text, "EV1527") != NULL, "text '%s'", d.text);
        }
    }
}

static void test_frame_shapes(void)
{
    rf_frame_t full, sliced, rotated;
    rf_norm_t n;
    rf_decoded_t d;
    const uint32_t id = 0x3BEEFu & 0xFFFFFu;
    const uint8_t btn = 0x6u;

    CASE("all four capture shapes decode");

    rf_ev1527_build(id, btn, 350, &full);

    /* (a) exactly as built: 24 bit-pairs then the sync pair. This is also the
     * shape a capture takes when the sync gap terminated the receive. */
    rf_normalize(&full, &n);
    CHECK(rf_decode(&full, &n, &d) && d.id == id && d.button == btn,
          "trailing-sync frame failed");

    /* (b) sync fell outside the capture: bits only. */
    slice_frame(&full, 0, 48, &sliced);
    rf_normalize(&sliced, &n);
    CHECK(rf_decode(&sliced, &n, &d) && d.id == id && d.button == btn,
          "no-sync frame failed (id=0x%05X btn=0x%X)", (unsigned)d.id, d.button);

    /* (c) armed mid-bit: a leading partial (low) pulse before the bits. The
     * partial here is the tail of the PREVIOUS word's sync gap, which is what
     * arming mid-gap actually produces. */
    rf_frame_reset(&sliced);
    sliced.first_level = 0;
    rf_frame_push(&sliced, 2000);                 /* clipped remainder of the gap */
    for (uint16_t i = 0; i < 48; i++) rf_frame_push(&sliced, full.durations_us[i]);
    rf_normalize(&sliced, &n);
    CHECK(rf_decode(&sliced, &n, &d) && d.id == id && d.button == btn,
          "leading-partial frame failed");

    /* (d) armed during the gap: sync pair first, then the bits. */
    rf_frame_reset(&rotated);
    rotated.first_level = 1;
    rf_frame_push(&rotated, full.durations_us[48]);
    rf_frame_push(&rotated, full.durations_us[49]);
    for (uint16_t i = 0; i < 48; i++) rf_frame_push(&rotated, full.durations_us[i]);
    rf_normalize(&rotated, &n);
    CHECK(rf_decode(&rotated, &n, &d) && d.id == id && d.button == btn,
          "sync-led frame failed");
}

/*
 * The user's real doorbell, captured on the bench and read back verbatim from
 * GET /api/signals/1. It is here so the encoder is checked against a genuine
 * transmitter rather than only against our own idea of one: replaying THIS
 * waveform rings the chime, so any synthesized frame that claims to be the same
 * code has to carry the same 24 bits at the same base.
 *
 * Note the shape: 49 pulses, ending on a HIGH. A capture can never contain the
 * ~9 ms sync gap, because that gap is longer than the 8 ms capture idle
 * threshold and is therefore exactly what ENDS the recording. The 49th pulse is
 * the fob's one-base sync HIGH; the sync LOW that follows it fell outside.
 */
static const uint16_t k_real_doorbell[] = {
    890, 262, 321, 851, 891, 263, 307, 864, 296, 868, 882, 270, 893, 291, 278,
    868, 893, 270, 302, 846, 317, 858, 291, 864, 311, 859, 886, 266, 314, 841,
    902, 270, 893, 290, 279, 866, 892, 270, 303, 871, 870, 296, 296, 862, 294,
    871, 290, 849, 316
};

static void load_real_doorbell(rf_frame_t *f)
{
    rf_frame_reset(f);
    f->first_level = 1;
    for (unsigned i = 0; i < sizeof(k_real_doorbell) / sizeof(k_real_doorbell[0]); i++)
        rf_frame_push(f, k_real_doorbell[i]);
}

static void test_real_capture_matches_encoder(void)
{
    rf_frame_t cap, built;
    rf_norm_t nc, nb;
    rf_decoded_t dc, db;

    CASE("real captured doorbell vs synthesized same code");

    load_real_doorbell(&cap);
    rf_normalize(&cap, &nc);
    CHECK(rf_decode(&cap, &nc, &dc), "the real capture no longer decodes");
    CHECK(dc.id == 0xA685Au, "captured id 0x%05X != 0xA685A", (unsigned)dc.id);
    CHECK(dc.button == 0x8u, "captured button 0x%X != 0x8", dc.button);
    CHECK(nc.base_us == 291u, "captured base %u != 291", nc.base_us);

    CHECK(rf_ev1527_build(dc.id, dc.button, nc.base_us, &built), "build failed");
    rf_normalize(&built, &nb);
    CHECK(rf_decode(&built, &nb, &db), "synthesized frame does not decode");

    /* The identity has to survive the round trip through the air-format. The
     * pulse COUNTS legitimately differ (the capture is missing its sync low),
     * which is why this compares decodes and not frames. */
    CHECK(db.id == dc.id && db.button == dc.button && db.code == dc.code,
          "synthesized 0x%06lX != captured 0x%06lX",
          (unsigned long)db.code, (unsigned long)dc.code);
    CHECK(db.base_us == dc.base_us, "base %u != %u", db.base_us, dc.base_us);

    /* And the bit halves have to line up with the ones the fob actually sent:
     * every captured bit pulse must be within 20 % of the synthesized one. */
    for (uint16_t i = 0; i < 48; i++) {
        uint32_t a = cap.durations_us[i], b = built.durations_us[i];
        uint32_t ref = (a > b) ? a : b;
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        CHECK(diff * 100u <= ref * 20u,
              "pulse %u: captured %lu us vs synthesized %lu us",
              i, (unsigned long)a, (unsigned long)b);
    }
}

/*
 * THE REGRESSION TEST FOR THE PAIRING BUG (bench, 2026-08-31).
 *
 * A word on air is only decodable if its framing gap is unambiguous, and every
 * rc-switch-derived receiver — which is most of them — finds the word boundary
 * by looking for ONE long low per period and then derives the base pulse width
 * from its length. The encoder used to emit the sync PAIR up front, and
 * rf_transmit used to ADD its inter-frame gap on top of whatever the frame
 * ended with; between them the repeated waveform contained TWO long lows per
 * period, separated by a stray one-base high. No EV1527 transmitter emits that,
 * and receivers that alternate their decode attempts across successive gaps can
 * lock onto the wrong one and never decode the word at all.
 *
 * This models the wire the way rf_transmit_frame() drives it — including the
 * merge of two consecutive same-level pulses and the minimum-gap rule — and
 * asserts the two invariants that make the waveform a fob replica: exactly one
 * long low per period, and that low being the protocol's 31x base.
 */
static void air_period_lows(const rf_frame_t *f, uint32_t gap_us, uint32_t threshold_us,
                            unsigned *n_long, uint32_t *longest)
{
    uint16_t last = (uint16_t)(f->count - 1u);
    uint32_t trailing = (rf_frame_level_at(f, last) == 0u) ? f->durations_us[last] : 0u;
    uint32_t longest_other = 0;
    bool frames_itself;
    uint32_t pad;

    for (uint16_t i = 0; i < last; i++)
        if (f->durations_us[i] > longest_other) longest_other = f->durations_us[i];
    frames_itself = (trailing > 0u) && (trailing >= 2u * longest_other);
    pad = frames_itself ? 0u : (gap_us > trailing) ? (gap_us - trailing) : 0u;

    *n_long = 0;
    *longest = 0;
    for (uint16_t i = 0; i < f->count; i++) {
        uint32_t d = f->durations_us[i];
        if (rf_frame_level_at(f, i) != 0u)
            continue;
        /* The frame's own trailing low merges with the pad, and (because the
         * next copy follows immediately) that is one single low on the wire. */
        if (i == last)
            d += pad;
        if (d > threshold_us) {
            (*n_long)++;
            if (d > *longest) *longest = d;
        }
    }
    /* A frame ending on a HIGH puts the whole pad on the wire as its own low. */
    if (trailing == 0u && pad > threshold_us) {
        (*n_long)++;
        if (pad > *longest) *longest = pad;
    }
}

static void test_air_period_has_one_sync(void)
{
    static const uint16_t bases[] = { 200u, 291u, 350u, 800u, 1200u };

    CASE("repeated waveform has exactly one sync low per word");

    for (unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        rf_frame_t f;
        unsigned n_long = 0;
        uint32_t longest = 0;
        uint32_t base = bases[i];
        /* Anything longer than 4x base cannot be a bit half, so it can only be
         * framing. That is the same "is this a gap" question a receiver asks. */
        uint32_t threshold = base * 4u;

        CHECK(rf_ev1527_build(0xA685Au, 0x8u, (uint16_t)base, &f),
              "build failed at base %lu", (unsigned long)base);

        air_period_lows(&f, 8000u, threshold, &n_long, &longest);
        CHECK(n_long == 1, "base %lu: %u long lows per period, expected 1",
              (unsigned long)base, n_long);
        /* The sync must stay base-proportional: a receiver that reads the base
         * back out of it as sync/31 has to arrive at the base we encoded. */
        CHECK(longest / 31u >= base - (base / 10u) && longest / 31u <= base + (base / 10u),
              "base %lu: sync %lu us implies base %lu us",
              (unsigned long)base, (unsigned long)longest, (unsigned long)(longest / 31u));
    }

    /* The captured frame — which ends on a HIGH and so relies entirely on the
     * transmitter's gap — must keep working exactly as it does today. */
    {
        rf_frame_t cap;
        unsigned n_long = 0;
        uint32_t longest = 0;

        load_real_doorbell(&cap);
        air_period_lows(&cap, 8000u, 291u * 4u, &n_long, &longest);
        CHECK(n_long == 1, "replayed capture: %u long lows per period, expected 1", n_long);
        CHECK(longest == 8000u, "replayed capture gap %lu us, expected the full 8000",
              (unsigned long)longest);
    }
}

static void test_base_estimation_under_jitter(void)
{
    static const uint16_t bases[] = { 200u, 350u, 500u, 750u };

    CASE("base-width estimation under +/-8% jitter");

    rng_seed(0xBEEF);
    for (unsigned bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++) {
        for (unsigned trial = 0; trial < 25; trial++) {
            rf_frame_t f;
            rf_norm_t n;
            rf_decoded_t d;
            uint32_t nominal = bases[bi];
            uint32_t err, allow;

            rf_ev1527_build(0x2ACE7u, 0x9u, (uint16_t)nominal, &f);
            jitter_frame(&f, 8);
            rf_normalize(&f, &n);

            err = (n.base_us > nominal) ? (n.base_us - nominal) : (nominal - n.base_us);
            allow = nominal * 5u / 100u;   /* averaging 25+ samples must beat +/-8% */
            CHECK(err <= allow, "base %u vs nominal %u (err %u > %u) trial %u",
                  n.base_us, nominal, err, allow, trial);
            CHECK(n.confidence >= 70, "jittered confidence %u < 70 (base %u)",
                  n.confidence, nominal);
            CHECK(rf_decode(&f, &n, &d) && d.id == 0x2ACE7u && d.button == 0x9u,
                  "jittered decode failed at base %u trial %u", nominal, trial);
        }
    }
}

static void test_base_estimation_with_glitch(void)
{
    rf_frame_t f;
    rf_norm_t n;
    rf_decoded_t d;

    CASE("a glitch burst must not poison the base estimate");

    /* A plain minimum would take the base from these spikes and every real pulse
     * would then quantize to nonsense. The cluster-population rule skips them. */
    rf_ev1527_build(0x51234u, 0x3u, 400, &f);
    f.durations_us[10] = 45;
    f.durations_us[11] = 52;
    f.durations_us[30] = 38;

    rf_normalize(&f, &n);
    CHECK(n.base_us >= 380 && n.base_us <= 420, "base %u poisoned by glitches", n.base_us);
    CHECK(n.confidence >= 80, "three glitches must not collapse confidence (%u)",
          n.confidence);
    /* The corrupted pulses destroy two of the 24 bits, and a 24-bit word with
     * two holes is not a decode — so declining here is the correct behaviour,
     * not a shortcoming. The point of the test is that the ESTIMATOR survived. */
    CHECK(!rf_decode(&f, &n, &d), "a frame with destroyed bits must not decode");
}

static void test_fingerprint(void)
{
    rf_frame_t base_frame, f;
    rf_norm_t n;
    rf_fingerprint_t ref, fp;
    unsigned stable = 0;

    CASE("fingerprint stable under jitter");

    rng_seed(0x1234);
    rf_ev1527_build(0x4D2A1u, 0x7u, 350, &base_frame);
    rf_normalize(&base_frame, &n);
    ref = rf_fingerprint(&base_frame, &n);
    CHECK(ref != 0, "fingerprint of a good frame is 0");

    for (unsigned i = 0; i < 40; i++) {
        f = base_frame;
        jitter_frame(&f, 8);
        rf_normalize(&f, &n);
        fp = rf_fingerprint(&f, &n);
        if (fp == ref) stable++;
    }
    CHECK(stable == 40, "only %u/40 jittered copies kept their fingerprint", stable);

    CASE("fingerprint differs between distinct codes");
    {
        static const uint32_t ids[] = { 0x4D2A1u, 0x4D2A2u, 0x00000u, 0xFFFFFu, 0x13579u };
        rf_fingerprint_t fps[sizeof(ids) / sizeof(ids[0])];
        unsigned collisions = 0;

        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            rf_ev1527_build(ids[i], (uint8_t)(i & 0xFu), 350, &f);
            rf_normalize(&f, &n);
            fps[i] = rf_fingerprint(&f, &n);
        }
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
            for (unsigned j = i + 1; j < sizeof(ids) / sizeof(ids[0]); j++) {
                if (fps[i] == fps[j]) collisions++;
            }
        }
        CHECK(collisions == 0, "%u fingerprint collisions between distinct codes",
              collisions);
    }

    CASE("fingerprint tolerates a missing normalization");
    {
        rf_ev1527_build(0x4D2A1u, 0x7u, 350, &f);
        CHECK(rf_fingerprint(&f, NULL) != 0, "NULL-norm fallback returned 0");
        CHECK(rf_fingerprint(NULL, NULL) == 0, "NULL frame must hash to 0");
    }
}

static void test_frame_similar(void)
{
    rf_frame_t a, b;

    CASE("rf_frame_similar");

    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &a);

    /* Positive: the same press, 4 % slower oscillator. */
    b = a;
    for (uint16_t i = 0; i < b.count; i++) {
        b.durations_us[i] = (uint16_t)(b.durations_us[i] * 104u / 100u);
    }
    CHECK(rf_frame_similar(&a, &b, 20, 60), "4%% drift must still match");

    /* Positive: short pulses off by more than 20 % but under the absolute floor
     * — this is exactly what the floor exists for. */
    b = a;
    b.durations_us[0] = (uint16_t)(a.durations_us[0] + 55);
    CHECK(rf_frame_similar(&a, &b, 10, 60), "absolute floor must rescue short pulses");
    CHECK(!rf_frame_similar(&a, &b, 10, 20), "with a small floor this must not match");

    /* Negative: one pulse grossly wrong. */
    b = a;
    b.durations_us[7] = (uint16_t)(a.durations_us[7] * 3u);
    CHECK(!rf_frame_similar(&a, &b, 20, 60), "a 3x pulse must not match");

    /* Negative: different pulse counts are different waveforms, full stop. */
    b = a;
    b.count--;
    CHECK(!rf_frame_similar(&a, &b, 50, 500), "different counts must not match");

    /* Symmetry. */
    b = a;
    b.durations_us[3] = (uint16_t)(a.durations_us[3] * 118u / 100u);
    CHECK(rf_frame_similar(&a, &b, 20, 0) == rf_frame_similar(&b, &a, 20, 0),
          "similarity must be symmetric");

    /* Degenerate inputs. */
    CHECK(!rf_frame_similar(&a, NULL, 20, 60), "NULL must not match");
    {
        rf_frame_t e1, e2;
        rf_frame_reset(&e1);
        rf_frame_reset(&e2);
        CHECK(rf_frame_similar(&e1, &e2, 0, 0), "two empty frames are the same");
    }
}

static void test_noise_rejected(void)
{
    unsigned decoded = 0;
    unsigned high_conf = 0;
    uint8_t worst = 0;

    CASE("pure noise is rejected");

    rng_seed(0xC0FFEE);
    for (unsigned trial = 0; trial < 200; trial++) {
        rf_frame_t f;
        rf_norm_t n;
        rf_decoded_t d;

        rf_frame_reset(&f);
        f.first_level = 1;
        /* Broadband AGC noise: no coherent symbol lengths at all. */
        for (unsigned i = 0; i < 60; i++) {
            rf_frame_push(&f, (uint16_t)rng_range(80, 2500));
        }
        rf_normalize(&f, &n);
        if (n.confidence > worst) worst = n.confidence;
        if (n.confidence >= 60) high_conf++;
        if (rf_decode(&f, &n, &d)) decoded++;
    }
    CHECK(decoded == 0, "%u/200 noise frames produced a decode", decoded);
    CHECK(high_conf == 0, "%u/200 noise frames scored confidence >= 60 (worst %u)",
          high_conf, worst);
    printf("  (noise: worst-case confidence %u)\n", worst);
}

static void test_degenerate_frames(void)
{
    rf_frame_t f;
    rf_norm_t n;
    rf_decoded_t d;

    CASE("degenerate frames");

    rf_frame_reset(&f);
    rf_normalize(&f, &n);
    CHECK(n.count == 0 && n.base_us == 0 && n.confidence == 0, "empty frame");
    CHECK(!rf_decode(&f, &n, &d) && !d.valid, "empty frame must not decode");

    /* Too few pulses to be 24 bits. */
    rf_frame_reset(&f);
    f.first_level = 1;
    for (unsigned i = 0; i < 12; i++) rf_frame_push(&f, (uint16_t)(i & 1 ? 1050 : 350));
    rf_normalize(&f, &n);
    CHECK(!rf_decode(&f, &n, &d), "12 pulses must not yield 24 bits");

    /* NULL safety. */
    rf_normalize(NULL, &n);
    CHECK(n.count == 0, "rf_normalize(NULL) must clear the output");
    CHECK(!rf_decode(NULL, &n, &d), "rf_decode(NULL frame)");
    CHECK(!rf_decoder_register(NULL), "registering NULL must fail");
}

static void test_frame_primitives(void)
{
    rf_frame_t f;

    CASE("rf_frame primitives");

    rf_frame_reset(&f);
    CHECK(f.count == 0 && f.first_level == 0, "reset");
    CHECK(rf_frame_duration_us(&f) == 0, "empty duration");

    f.first_level = 1;
    for (unsigned i = 0; i < RF_FRAME_MAX_PULSES; i++) {
        CHECK(rf_frame_push(&f, 100), "push %u must succeed", i);
    }
    CHECK(!rf_frame_push(&f, 100), "push past the end must fail, not overflow");
    CHECK(f.count == RF_FRAME_MAX_PULSES, "count %u", f.count);
    CHECK(rf_frame_duration_us(&f) == 100u * RF_FRAME_MAX_PULSES, "total airtime");

    CHECK(rf_frame_level_at(&f, 0) == 1, "level[0]");
    CHECK(rf_frame_level_at(&f, 1) == 0, "level[1]");
    CHECK(rf_frame_level_at(&f, 2) == 1, "level[2]");
    CHECK(!rf_frame_push(NULL, 10), "push into NULL");
    CHECK(rf_frame_duration_us(NULL) == 0, "duration of NULL");
}

/* ======================================================================
   GROUPING, RANKING AND FRAGMENT REJOINING (rf_group.c)

   These replace an admission gate that used to decide, before a user saw
   anything at all, whether a captured frame was allowed to become a candidate.
   The gate demanded two repeats and 65 % normalization confidence, both tuned on
   EV1527 — so a remote of any other shape was silently invisible. The tests
   below therefore assert the INVERSION as hard as they assert the mechanics:
   nothing is ever filtered, and repetition (which needs no protocol knowledge)
   outranks decoding (which does).
   ====================================================================== */

/* Fill an observation. `frame` is borrowed, exactly as rf_raw borrows its slots. */
static void obs_set(rf_obs_t *o, const rf_frame_t *f, uint16_t index,
                    int64_t ts_us, int16_t rssi, uint8_t conf, bool decoded)
{
    o->frame         = f;
    o->index         = index;
    o->ts_us         = ts_us;
    o->airtime_us    = rf_frame_duration_us(f);
    o->rssi_dbm      = rssi;
    o->confidence    = conf;
    o->decoded_valid = decoded;
}

/* Scale every duration by pct/100 — a same-press oscillator drift, small enough
 * to stay inside the grouping tolerance. */
static void scale_frame(rf_frame_t *f, uint32_t pct)
{
    for (uint16_t i = 0; i < f->count; i++) {
        uint32_t d = (uint32_t)f->durations_us[i] * pct / 100u;
        f->durations_us[i] = (uint16_t)(d < 1 ? 1 : (d > 65535 ? 65535 : d));
    }
}

/* A frame no decoder claims: short, and with no coherent symbol structure. */
static void make_odd_frame(rf_frame_t *f, uint16_t base)
{
    static const uint16_t shape[] = { 3, 7, 2, 11, 4, 5, 9, 2 };
    rf_frame_reset(f);
    f->first_level = 1;
    for (unsigned i = 0; i < sizeof(shape) / sizeof(shape[0]); i++) {
        rf_frame_push(f, (uint16_t)(shape[i] * base));
    }
}

static void test_group_basics(void)
{
    rf_frame_t a, a2, a3, b;
    rf_obs_t obs[4];
    rf_group_t g[8];

    CASE("grouping: repeats collapse into one candidate");

    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &a);
    a2 = a; scale_frame(&a2, 103);      /* the same press, 3 % slower */
    a3 = a; scale_frame(&a3,  97);
    rf_ev1527_build(0x13579u, 0x2u, 350, &b);

    obs_set(&obs[0], &a,  1, 1000000, -40, 90, true);
    obs_set(&obs[1], &b,  2, 2000000, -70, 88, true);
    obs_set(&obs[2], &a2, 3, 3000000, -38, 92, true);
    obs_set(&obs[3], &a3, 4, 4000000, -41, 85, true);

    int n = rf_group_build(obs, 4, g, 8);
    CHECK(n == 2, "4 frames, 2 distinct waveforms -> 2 groups, got %d", n);
    CHECK(g[0].count == 3, "the thrice-seen waveform must lead, count %u", g[0].count);
    CHECK(g[1].count == 1, "the once-seen waveform is second, count %u", g[1].count);
    CHECK(g[0].score > g[1].score, "3 repeats must outscore 1");

    /* Membership is in arrival order and complete. */
    CHECK(g[0].member[0] == 0 && g[0].member[1] == 2 && g[0].member[2] == 3,
          "members in arrival order");
    CHECK(g[0].first_us == 1000000 && g[0].last_us == 4000000, "first/last timestamps");
    CHECK(g[0].best_rssi_dbm == -38, "best RSSI across the group, got %d",
          g[0].best_rssi_dbm);

    /* The exemplar is the cleanest member, not simply the first. */
    CHECK(g[0].rep == 2, "exemplar must be the 92%% member, got %u", g[0].rep);
    CHECK(g[0].confidence == 92, "exemplar confidence, got %u", g[0].confidence);

    /* Every observation is accounted for exactly once. */
    unsigned total = 0;
    for (int i = 0; i < n; i++) total += g[i].count;
    CHECK(total == 4, "no observation may be dropped, total %u", total);
}

static void test_group_repeats_dominate_decoding(void)
{
    rf_frame_t odd, odd2, odd3, ev;
    rf_obs_t obs[5];
    rf_group_t g[8];

    CASE("ranking: repetition outranks a decode");

    /* THE REGRESSION TEST FOR THE WHOLE FEATURE. The undecoded frame is short,
     * scores badly on confidence and would have failed the old gate on every
     * count. It was heard three times; the pristine decoded EV1527 twice. The
     * undecoded one must win, because repetition is the stronger evidence and it
     * is evidence we can collect without knowing the protocol. */
    make_odd_frame(&odd, 120);
    odd2 = odd; scale_frame(&odd2, 104);
    odd3 = odd; scale_frame(&odd3,  96);
    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &ev);

    obs_set(&obs[0], &ev,   1, 1000000, -30, 100, true);
    obs_set(&obs[1], &odd,  2, 2000000, -60,  20, false);
    obs_set(&obs[2], &ev,   3, 3000000, -30, 100, true);
    obs_set(&obs[3], &odd2, 4, 4000000, -62,  18, false);
    obs_set(&obs[4], &odd3, 5, 5000000, -61,  22, false);

    int n = rf_group_build(obs, 5, g, 8);
    CHECK(n == 2, "two waveforms, got %d groups", n);
    CHECK(g[0].count == 3 && !g[0].decoded_valid,
          "the 3x undecoded candidate must rank first");
    CHECK(g[1].count == 2 && g[1].decoded_valid,
          "the 2x decoded candidate must rank second");

    /* Stated as the invariant rather than as two numbers, so an edit to the
     * weights that breaks the intent fails here. */
    CHECK(RF_SCORE_PER_REPEAT >
          RF_SCORE_DECODED + 100u * RF_SCORE_CONF_MUL + RF_SCORE_LEN_CAP,
          "one repeat must outweigh every non-repeat bonus combined");
}

static void test_group_undecoded_is_first_class(void)
{
    rf_frame_t f;
    rf_obs_t obs[1];
    rf_group_t g[4];

    CASE("ranking: nothing is ever filtered out");

    /* Two pulses, zero confidence, no decode, seen once: the weakest thing that
     * can arrive. The old gate rejected it outright. It must still be offered —
     * the user is the one who decides whether it is their bell. */
    rf_frame_reset(&f);
    f.first_level = 1;
    rf_frame_push(&f, 400);
    rf_frame_push(&f, 900);
    obs_set(&obs[0], &f, 1, 1000000, -95, 0, false);

    int n = rf_group_build(obs, 1, g, 4);
    CHECK(n == 1, "a single weak frame is still a candidate, got %d", n);
    CHECK(g[0].count == 1 && g[0].pulse_count == 2, "reported verbatim");
    CHECK(g[0].score > 0, "score %u", g[0].score);
}

static void test_group_tiebreaks(void)
{
    rf_frame_t x, y, z;
    rf_obs_t obs[3];
    rf_group_t g[8];

    CASE("ranking: decode and confidence break ties, nothing more");

    rf_ev1527_build(0x11111u, 0x1u, 350, &x);
    make_odd_frame(&y, 100);
    make_odd_frame(&z, 260);

    /* All seen once. Decoded must lead; between the two undecoded ones the
     * higher confidence wins. */
    obs_set(&obs[0], &y, 1, 1000000, -50, 40, false);
    obs_set(&obs[1], &x, 2, 2000000, -50, 40, true);
    obs_set(&obs[2], &z, 3, 3000000, -50, 70, false);

    int n = rf_group_build(obs, 3, g, 8);
    CHECK(n == 3, "three distinct waveforms, got %d", n);
    CHECK(g[0].decoded_valid, "decoded leads at equal repeats");
    CHECK(!g[1].decoded_valid && g[1].confidence == 70, "then the cleaner one");
    CHECK(!g[2].decoded_valid && g[2].confidence == 40, "then the noisier one");

    /* Equal in every respect: arrival order decides, and stays decided. */
    obs_set(&obs[0], &y, 1, 1000000, -50, 40, false);
    obs_set(&obs[1], &z, 2, 2000000, -50, 40, false);
    n = rf_group_build(obs, 2, g, 8);
    CHECK(n == 2 && g[0].member[0] == 0, "ties keep arrival order");
}

static void test_group_degenerate(void)
{
    rf_obs_t obs[2];
    rf_frame_t empty, good;
    rf_group_t g[4];

    CASE("grouping: degenerate inputs");

    rf_frame_reset(&empty);
    rf_ev1527_build(0x22222u, 0x3u, 350, &good);
    obs_set(&obs[0], &empty, 1, 1000, -50, 0, false);
    obs_set(&obs[1], &good,  2, 2000, -50, 90, true);

    CHECK(rf_group_build(NULL, 3, g, 4) == 0, "NULL observations");
    CHECK(rf_group_build(obs, 2, NULL, 4) == 0, "NULL output");
    CHECK(rf_group_build(obs, 2, g, 0) == 0, "zero capacity");
    CHECK(rf_group_build(obs, 0, g, 4) == 0, "no observations");
    CHECK(rf_group_build(obs, 2, g, 4) == 1, "an empty frame is not an observation");
    CHECK(rf_group_score(NULL) == 0, "score of NULL");
}

/* ---- fragmentation ------------------------------------------------------- */

/* Split `src` at the LOW pulse `at`, which becomes the silence the capture layer
 * would have thrown away. Models exactly what a too-short idle threshold does. */
static void split_at_low(const rf_frame_t *src, uint16_t at,
                         rf_frame_t *head, rf_frame_t *tail, uint32_t *gap_us)
{
    slice_frame(src, 0, at, head);
    slice_frame(src, (uint16_t)(at + 1), (uint16_t)(src->count - at - 1), tail);
    *gap_us = src->durations_us[at];
}

static void test_frag_detects_a_chopped_transmission(void)
{
    rf_frame_t whole, head, tail, rejoined;
    rf_obs_t obs[2];
    rf_run_t runs[4];
    uint32_t gap = 0;

    CASE("fragmentation: a cut transmission is found and rejoined");

    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &whole);
    /* Index 25 is a LOW (first_level is 1, so odd indices are LOW). */
    CHECK(rf_frame_level_at(&whole, 25) == 0, "pick a LOW to split on");
    split_at_low(&whole, 25, &head, &tail, &gap);

    /* The threshold that cut it was just under the gap. */
    uint32_t idle = gap - 100u;
    int64_t t0 = 1000000;
    obs_set(&obs[0], &head, 1, t0, -40, 80, false);
    obs_set(&obs[1], &tail, 2,
            t0 + (int64_t)gap + (int64_t)rf_frame_duration_us(&tail), -40, 75, false);

    int n = rf_frag_find_runs(obs, 2, idle, runs, 4);
    CHECK(n == 1, "the pair must be reported as one run, got %d", n);
    CHECK(runs[0].count == 2, "run length %u", runs[0].count);
    CHECK(runs[0].gap_us[1] == gap, "the gap must be MEASURED (%u), got %u",
          gap, runs[0].gap_us[1]);
    CHECK(runs[0].joinable, "this run must be joinable");

    /* And the join is exact: rebuilding from the pieces plus the measured
     * silence reproduces the original waveform pulse for pulse. An invented gap
     * would pass every other check here and still never ring a bell. */
    CHECK(rf_frame_join_run(&rejoined, obs, &runs[0]), "join must succeed");
    CHECK(rejoined.count == whole.count, "rejoined %u pulses, original %u",
          rejoined.count, whole.count);
    /* The prediction and the join must never disagree: callers describe a
     * merged candidate (and size its trim handles) from the prediction long
     * before anyone asks for the waveform. */
    CHECK(rf_run_joined_pulses(obs, &runs[0]) == rejoined.count,
          "predicted %u pulses, joined %u",
          rf_run_joined_pulses(obs, &runs[0]), rejoined.count);
    CHECK(rejoined.first_level == whole.first_level, "first_level preserved");
    int same = 1;
    for (uint16_t i = 0; i < whole.count && i < rejoined.count; i++) {
        if (rejoined.durations_us[i] != whole.durations_us[i]) same = 0;
    }
    CHECK(same, "every duration must survive the round trip");
}

static void test_frag_ignores_honest_repeats(void)
{
    rf_frame_t a, b;
    rf_obs_t obs[3];
    rf_run_t runs[4];

    CASE("fragmentation: identical neighbours are repeats, not fragments");

    /* A transmitter whose inter-word gap is shorter than the threshold gets
     * split too — but into IDENTICAL pieces. That is a repeat group and must
     * never be reported as fragmentation, or the UI would tell the user to
     * raise a threshold that is doing its job. */
    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &a);
    b = a; scale_frame(&b, 102);

    int64_t t = 1000000;
    uint32_t air = rf_frame_duration_us(&a);
    obs_set(&obs[0], &a, 1, t, -40, 90, true);
    obs_set(&obs[1], &b, 2, t + 9000 + (int64_t)air, -40, 90, true);
    obs_set(&obs[2], &a, 3, t + 18000 + 2 * (int64_t)air, -40, 90, true);

    CHECK(rf_frag_find_runs(obs, 3, 8000, runs, 4) == 0,
          "similar frames must never be called fragments");
}

static void test_frag_ignores_separate_presses(void)
{
    rf_frame_t a, b;
    rf_obs_t obs[2];
    rf_run_t runs[4];

    CASE("fragmentation: a real pause is not a cut");

    rf_ev1527_build(0x0ABCDu, 0x5u, 350, &a);
    make_odd_frame(&b, 400);

    /* Different waveforms, but a whole second apart: the transmitter stopped. */
    obs_set(&obs[0], &a, 1, 1000000, -40, 90, true);
    obs_set(&obs[1], &b, 2, 2000000, -40, 30, false);
    CHECK(rf_frag_find_runs(obs, 2, 8000, runs, 4) == 0,
          "a 1 s pause is not the idle threshold cutting anything");

    /* And the boundary itself: just inside the window is a cut, just outside
     * is not. */
    uint32_t air = rf_frame_duration_us(&b);
    obs_set(&obs[1], &b, 2, 1000000 + 15000 + (int64_t)air, -40, 30, false);
    CHECK(rf_frag_find_runs(obs, 2, 8000, runs, 4) == 1, "15 ms after an 8 ms idle is a cut");
    obs_set(&obs[1], &b, 2, 1000000 + 17000 + (int64_t)air, -40, 30, false);
    CHECK(rf_frag_find_runs(obs, 2, 8000, runs, 4) == 0, "17 ms is past 2x the idle");

    CHECK(rf_frag_find_runs(obs, 2, 0, runs, 4) == 0, "idle_us 0 means no verdict");
    CHECK(rf_frag_find_runs(NULL, 2, 8000, runs, 4) == 0, "NULL observations");
    CHECK(rf_frag_find_runs(obs, 2, 8000, NULL, 4) == 0, "NULL output");
}

static void test_frag_three_way_run(void)
{
    rf_frame_t whole, p1, p2, p3, mid, rejoined;
    rf_obs_t obs[3];
    rf_run_t runs[4];
    uint32_t g1 = 0, g2 = 0;

    CASE("fragmentation: three pieces rejoin as one");

    rf_ev1527_build(0x7C3E1u, 0xAu, 300, &whole);
    split_at_low(&whole, 15, &p1, &mid, &g1);
    /* `mid` starts at index 16 of the original; splitting it again at ITS index
     * 15 lands on original index 31, another LOW. */
    CHECK(rf_frame_level_at(&mid, 15) == 0, "second split must also be a LOW");
    split_at_low(&mid, 15, &p2, &p3, &g2);

    int64_t t = 1000000;
    t += rf_frame_duration_us(&p1);
    obs_set(&obs[0], &p1, 1, t, -35, 70, false);
    t += (int64_t)g1 + rf_frame_duration_us(&p2);
    obs_set(&obs[1], &p2, 2, t, -35, 65, false);
    t += (int64_t)g2 + rf_frame_duration_us(&p3);
    obs_set(&obs[2], &p3, 3, t, -35, 60, false);

    uint32_t idle = (g1 < g2 ? g1 : g2) - 50u;
    int n = rf_frag_find_runs(obs, 3, idle, runs, 4);
    CHECK(n == 1 && runs[0].count == 3, "one run of three, got %d runs", n);
    CHECK(runs[0].gap_us[1] == g1 && runs[0].gap_us[2] == g2, "both gaps measured");

    CHECK(rf_frame_join_run(&rejoined, obs, &runs[0]), "three-way join");
    CHECK(rejoined.count == whole.count, "rejoined %u vs %u pulses",
          rejoined.count, whole.count);
    CHECK(rf_run_joined_pulses(obs, &runs[0]) == rejoined.count,
          "three-way prediction %u vs joined %u",
          rf_run_joined_pulses(obs, &runs[0]), rejoined.count);
    int same = (rejoined.first_level == whole.first_level);
    for (uint16_t i = 0; i < whole.count && i < rejoined.count; i++) {
        if (rejoined.durations_us[i] != whole.durations_us[i]) same = 0;
    }
    CHECK(same, "a three-piece round trip must be exact");

    /* The rejoined frame is a real EV1527 again — the strongest possible proof
     * that the reconstruction is faithful rather than merely plausible. */
    rf_norm_t nrm;
    rf_decoded_t dec;
    rf_normalize(&rejoined, &nrm);
    CHECK(rf_decode(&rejoined, &nrm, &dec) && dec.valid, "rejoined frame must decode");
    CHECK(dec.id == 0x7C3E1u && dec.button == 0xAu, "and to the original identity");
}

static void test_frame_join_levels(void)
{
    rf_frame_t a, b, out;

    CASE("join: strict level alternation is preserved");

    /* a ends HIGH, b starts HIGH: the gap becomes its own LOW pulse. */
    rf_frame_reset(&a); a.first_level = 1;
    rf_frame_push(&a, 100); rf_frame_push(&a, 200); rf_frame_push(&a, 300);
    rf_frame_reset(&b); b.first_level = 1;
    rf_frame_push(&b, 400); rf_frame_push(&b, 500);
    CHECK(rf_frame_join(&out, &a, 7000, &b), "join a(HIGH-end) + b(HIGH-start)");
    CHECK(out.count == 6, "3 + gap + 2 = 6, got %u", out.count);
    CHECK(out.durations_us[3] == 7000, "the gap is its own pulse");
    CHECK(rf_frame_level_at(&out, 3) == 0, "and it is LOW");
    CHECK(out.durations_us[4] == 400 && out.durations_us[5] == 500, "b follows intact");

    /* a ends LOW: the gap EXTENDS that pulse instead of doubling it. */
    rf_frame_reset(&a); a.first_level = 1;
    rf_frame_push(&a, 100); rf_frame_push(&a, 200);
    CHECK(rf_frame_join(&out, &a, 7000, &b), "join a(LOW-end) + b(HIGH-start)");
    CHECK(out.count == 4, "2 + 2 with the gap folded in, got %u", out.count);
    CHECK(out.durations_us[1] == 7200, "gap folded into the trailing LOW, got %u",
          out.durations_us[1]);

    /* b starts LOW: its first duration is silence and joins the gap. */
    rf_frame_reset(&b); b.first_level = 0;
    rf_frame_push(&b, 600); rf_frame_push(&b, 400); rf_frame_push(&b, 500);
    rf_frame_reset(&a); a.first_level = 1;
    rf_frame_push(&a, 100); rf_frame_push(&a, 200); rf_frame_push(&a, 300);
    CHECK(rf_frame_join(&out, &a, 7000, &b), "join a(HIGH-end) + b(LOW-start)");
    /* 3 + one gap pulse + 2 of b's 3: b's leading silence became part of the gap
     * rather than a fourth pulse, which is what "absorbed" buys. */
    CHECK(out.count == 6, "b's leading silence is absorbed, got %u", out.count);
    CHECK(out.durations_us[3] == 7600, "gap + b[0], got %u", out.durations_us[3]);
    CHECK(out.durations_us[4] == 400, "b resumes at its first HIGH");

    /* Levels must alternate everywhere, by construction. */
    for (uint16_t i = 1; i < out.count; i++) {
        CHECK(rf_frame_level_at(&out, i) != rf_frame_level_at(&out, (uint16_t)(i - 1)),
              "levels alternate at %u", i);
    }
}

static void test_frame_join_refusals(void)
{
    rf_frame_t a, b, out;

    CASE("join: refuse rather than corrupt");

    rf_frame_reset(&a); a.first_level = 1;
    rf_frame_push(&a, 100); rf_frame_push(&a, 200); rf_frame_push(&a, 300);
    rf_frame_reset(&b); b.first_level = 1;
    rf_frame_push(&b, 400);

    CHECK(!rf_frame_join(NULL, &a, 0, &b), "NULL output");
    CHECK(!rf_frame_join(&out, NULL, 0, &b), "NULL a");
    CHECK(!rf_frame_join(&out, &a, 0, NULL), "NULL b");

    rf_frame_t empty;
    rf_frame_reset(&empty);
    CHECK(!rf_frame_join(&out, &empty, 0, &b), "empty a");
    CHECK(!rf_frame_join(&out, &a, 0, &empty), "empty b");

    /* A gap that will not fit one duration must be refused, not truncated. */
    CHECK(!rf_frame_join(&out, &a, 70000, &b), "a gap over 65535 us cannot be a pulse");
    CHECK(out.count == 0, "a refused join must leave the output empty");

    /* b consisting only of silence has nothing to contribute. */
    rf_frame_t silent;
    rf_frame_reset(&silent); silent.first_level = 0;
    rf_frame_push(&silent, 500);
    CHECK(!rf_frame_join(&out, &a, 100, &silent), "b that is only silence");

    /* Overflowing the pulse ceiling. */
    rf_frame_t big1, big2;
    rf_frame_reset(&big1); big1.first_level = 1;
    rf_frame_reset(&big2); big2.first_level = 1;
    for (uint16_t i = 0; i < RF_FRAME_MAX_PULSES; i++) {
        rf_frame_push(&big1, 100);
        rf_frame_push(&big2, 100);
    }
    CHECK(!rf_frame_join(&out, &big1, 1000, &big2), "1024 pulses cannot fit in 512");
    CHECK(out.count == 0, "and the output is left empty");

    CHECK(!rf_frame_join_run(&out, NULL, NULL), "join_run NULL");
}

int main(void)
{
    printf("rfpulse host tests\n");
    printf("------------------\n");

    rf_decoders_register_builtin();
    rf_decoders_register_builtin();   /* must be idempotent */

    test_frame_primitives();
    test_build_decode_roundtrip();
    test_frame_shapes();
    test_real_capture_matches_encoder();
    test_air_period_has_one_sync();
    test_base_estimation_under_jitter();
    test_base_estimation_with_glitch();
    test_fingerprint();
    test_frame_similar();
    test_noise_rejected();
    test_degenerate_frames();

    test_group_basics();
    test_group_repeats_dominate_decoding();
    test_group_undecoded_is_first_class();
    test_group_tiebreaks();
    test_group_degenerate();
    test_frag_detects_a_chopped_transmission();
    test_frag_ignores_honest_repeats();
    test_frag_ignores_separate_presses();
    test_frag_three_way_run();
    test_frame_join_levels();
    test_frame_join_refusals();

    printf("------------------\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
