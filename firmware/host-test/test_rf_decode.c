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

    printf("------------------\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
