/*
 * rf_frame.h - The protocol-agnostic representation everything else is built on.
 *
 * This is deliberately the LOWEST common denominator of an OOK/ASK transmission:
 * a carrier frequency, a modulation, and a sequence of HIGH/LOW pulse durations.
 * No bits, no protocol, no assumptions. A frame that no decoder understands is
 * still a complete, replayable recording — which is the whole point: unknown
 * protocols must round-trip even when they cannot be interpreted.
 *
 * Levels alternate strictly. `first_level` gives the level of durations_us[0];
 * every subsequent entry is the opposite of its predecessor. Storing only
 * durations (not level/duration pairs) halves the memory and makes the
 * alternation impossible to violate by construction.
 */
#ifndef RF_FRAME_H
#define RF_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 512 pulses covers an EV1527 frame (~50) with room for long unknown protocols
 * and multi-frame bursts. At 2 bytes each this is a 1 KB payload — small enough
 * to pass by value through queues and to keep several in RAM without PSRAM. */
#define RF_FRAME_MAX_PULSES 512

/* A single captured or synthesized burst. */
typedef struct {
    uint16_t count;                              /* number of valid durations */
    uint8_t  first_level;                        /* 0 or 1: level of durations_us[0] */
    uint16_t durations_us[RF_FRAME_MAX_PULSES];  /* alternating, microseconds */
} rf_frame_t;

/* Radio settings a frame was captured with / must be replayed with. Travels with
 * the frame so a stored signal replays under the same conditions it was heard. */
typedef struct {
    uint32_t freq_hz;
    uint8_t  modulation;    /* cc1101_modulation_t value, kept as a plain int
                               so this header does not depend on the driver */
    uint32_t datarate_bps;
    uint32_t rx_bandwidth_hz;
} rf_radio_params_t;

static inline void rf_frame_reset(rf_frame_t *f) { f->count = 0; f->first_level = 0; }

/* Append one pulse; returns false if the frame is full (caller should treat a
 * full frame as a truncated capture, not a fatal error). */
bool rf_frame_push(rf_frame_t *f, uint16_t duration_us);

/* Total airtime of the frame in microseconds. */
uint32_t rf_frame_duration_us(const rf_frame_t *f);

/* Level (0/1) of pulse index i, derived from first_level. */
static inline uint8_t rf_frame_level_at(const rf_frame_t *f, uint16_t i)
{
    return (uint8_t)(f->first_level ^ (i & 1u));
}

/*
 * Fuzzy equality used for repeat detection and button matching. Two frames match
 * when they have the same pulse count and every duration agrees within
 * `tolerance_pct` (relative) or `tolerance_us` (absolute floor, which is what
 * saves short pulses from failing a purely relative test). Cheap transmitters
 * drift by several percent between presses and as their battery ages, so an
 * exact comparison would never match anything.
 */
bool rf_frame_similar(const rf_frame_t *a, const rf_frame_t *b,
                      uint8_t tolerance_pct, uint16_t tolerance_us);

/*
 * Echo-tolerant variant for TX echo suppression, where `sent` is a frame WE
 * transmitted and `heard` is a frame just captured off the air. The verbatim
 * comparison above is structurally wrong for that job: when `sent` ends in a
 * low pulse at least as long as the receiver's idle threshold — a synthesized
 * EV1527 frame carries its own 31x-base framing gap, ~10.85 ms at the default
 * 350 us base against an 8 ms threshold — an echo of it can never be captured
 * whole. That trailing low is exactly what TERMINATES the reception, and the
 * capture path drops the terminating silence, so the echo arrives one pulse
 * short of what was sent and an equal-count comparison never matches.
 *
 * This function therefore accepts either the verbatim match (rf_frame_similar)
 * or `heard` matching `sent` with such a trailing gap stripped. The "is the gap
 * long enough to have been truncated" test is widened by the same per-pulse
 * tolerance window, because the echoing device re-times the gap with its own
 * oscillator; the widening cannot cause false suppression, since every
 * remaining pulse must still agree. `capture_idle_us` is the RX idle threshold
 * currently in force; passing 0 disables the stripped form entirely.
 */
bool rf_frame_similar_tx_echo(const rf_frame_t *sent, const rf_frame_t *heard,
                              uint8_t tolerance_pct, uint16_t tolerance_us,
                              uint32_t capture_idle_us);

#ifdef __cplusplus
}
#endif

#endif /* RF_FRAME_H */
