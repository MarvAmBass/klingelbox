/*
 * rf_decode.h - Timing normalization, fingerprinting, and the decoder registry.
 *
 * LAYERING, WHICH IS THE POINT. Decoding is strictly optional analysis performed
 * ON TOP of a raw frame. Nothing below this header knows a protocol exists, and
 * a frame that no decoder claims remains fully usable (matchable and replayable).
 * EV1527 is registered here as the first plugin, not baked into the architecture.
 *
 * NORMALIZATION LEARNS TIMINGS, IT DOES NOT ASSUME THEM. Cheap remotes use RC
 * oscillators that drift with temperature and battery voltage, so the "correct"
 * short-pulse width is a property of the individual capture, not a constant. The
 * normalizer estimates the base (shortest recurring) pulse width from the frame
 * itself by histogramming durations and taking the dominant short cluster, then
 * expresses every pulse as a multiple of that base. A ~1:3 short/long ratio is
 * common for EV1527-class remotes but is an OUTPUT of this analysis, never an
 * input assumption.
 */
#ifndef RF_DECODE_H
#define RF_DECODE_H

#include <stdbool.h>
#include <stdint.h>
#include "rf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RF_PROTOCOL_NAME_MAX 16
#define RF_DECODE_TEXT_MAX   48

/* ---- normalization ------------------------------------------------------- */

typedef struct {
    uint16_t base_us;       /* estimated short-pulse width, learned from the frame */
    uint8_t  confidence;    /* 0..100: how cleanly durations cluster on multiples */
    uint16_t count;         /* == frame->count */
    /* Each pulse as a rounded multiple of base_us, clamped to 255. 0 marks a
     * pulse that did not fit any multiple within tolerance (a timing outlier). */
    uint8_t  mult[RF_FRAME_MAX_PULSES];
} rf_norm_t;

/* Analyze a raw frame. Always succeeds; a noisy frame simply yields a low
 * confidence, which callers may use to reject it. */
void rf_normalize(const rf_frame_t *frame, rf_norm_t *out);

/*
 * A stable identity for matching a press against a learned button. Derived from
 * the decoded payload when a decoder claimed the frame (the strongest evidence),
 * otherwise from the quantized timing structure, which stays stable across the
 * oscillator drift that makes raw durations useless as a key.
 */
typedef uint32_t rf_fingerprint_t;

rf_fingerprint_t rf_fingerprint(const rf_frame_t *frame, const rf_norm_t *norm);

/* ---- decoded result ------------------------------------------------------ */

typedef struct {
    bool     valid;
    char     protocol[RF_PROTOCOL_NAME_MAX];  /* e.g. "ev1527" */
    uint8_t  bit_count;
    uint64_t code;                            /* raw decoded bits, MSB-first */
    uint32_t id;                              /* protocol-specific address part */
    uint8_t  button;                          /* protocol-specific data part */
    uint16_t base_us;                         /* timing the decode was based on */
    char     text[RF_DECODE_TEXT_MAX];        /* short human-readable summary */
} rf_decoded_t;

/* ---- decoder plugin registry --------------------------------------------- */

/*
 * A decoder inspects the raw frame plus its normalization and either claims it
 * (fills *out, returns true) or declines. Decoders must be pure and fast: they
 * run in the capture consumer task, and several are tried per frame.
 */
typedef struct {
    const char *name;
    bool (*decode)(const rf_frame_t *frame, const rf_norm_t *norm, rf_decoded_t *out);
} rf_decoder_t;

/* Register a decoder. The pointer must remain valid for the process lifetime
 * (use a static). Decoders are tried in registration order. */
bool rf_decoder_register(const rf_decoder_t *decoder);

/* Register every decoder built into this component (currently: EV1527). Called
 * once at startup; individual decoders may also be registered by the app. */
void rf_decoders_register_builtin(void);

/* Try every registered decoder. Returns true and fills *out on the first claim;
 * returns false (out->valid = false) when the frame is an unknown protocol —
 * which is an ordinary, fully supported outcome, not an error. */
bool rf_decode(const rf_frame_t *frame, const rf_norm_t *norm, rf_decoded_t *out);

/* ---- EV1527: the first decoder plugin, plus its encoder -------------------
 *
 * EV1527 (and the compatible PT2262/HS1527 family) sends a sync gap followed by
 * 24 bits: 20 bits of transmitter address and 4 bits of button/data. Each bit is
 * a pair of pulses — short/long for a 0, long/short for a 1 — with the long
 * nominally three times the short. All of those numbers are treated as
 * expectations to be VERIFIED against the learned base width, never as hardcoded
 * constants; the decoder tolerates the drift real remotes exhibit.
 */
extern const rf_decoder_t rf_decoder_ev1527;

/*
 * Synthesize an EV1527 frame — this is how "virtual signals" are created: pick a
 * fresh address, transmit it while the user's own doorbell receiver is in
 * learning mode, and the receiver is then paired to a signal this box owns.
 *
 * id20    - 20-bit transmitter address (only the low 20 bits are used)
 * button4 - 4-bit data/button nibble (only the low 4 bits are used)
 * base_us - short-pulse width; ~350 us is typical. Pass 0 for a sane default.
 */
bool rf_ev1527_build(uint32_t id20, uint8_t button4, uint16_t base_us, rf_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RF_DECODE_H */
