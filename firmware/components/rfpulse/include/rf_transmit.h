/*
 * rf_transmit.h - Replay a pulse frame through RMT TX onto the CC1101 data line.
 *
 * The transmitter is the exact mirror of the capture path: it takes the same
 * protocol-agnostic rf_frame_t and reproduces its durations on GDO0 while the
 * CC1101 sits in asynchronous TX mode keying the carrier. Because the recording
 * is raw timings, this replays protocols the firmware cannot decode — the stated
 * goal that unknown captures must still be usable.
 *
 * Repeats matter for real receivers. These remotes transmit their frame several
 * times back-to-back, and many receivers deliberately require two or more
 * consistent copies before acting (a cheap noise-rejection trick). Replaying a
 * single frame therefore often does nothing even when the timings are perfect —
 * so repeats and the inter-frame gap are first-class parameters here.
 *
 * See rf_capture.h "PIN OWNERSHIP": release the capture channel first.
 */
#ifndef RF_TRANSMIT_H
#define RF_TRANSMIT_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "rf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin;            /* CC1101 GDO0, driven by us in TX mode */
    uint32_t   resolution_hz;  /* must match capture (1 MHz) for faithful replay */
    uint8_t    idle_level;     /* line level between frames — 0 for OOK */
} rf_transmit_cfg_t;

void rf_transmit_cfg_default(rf_transmit_cfg_t *out, gpio_num_t pin);

esp_err_t rf_transmit_init(const rf_transmit_cfg_t *cfg);
void rf_transmit_release(void);
bool rf_transmit_active(void);

/*
 * Send `frame` `repeats` times with AT LEAST `gap_us` of idle between copies,
 * blocking until the last pulse has physically left the peripheral (so the
 * caller may immediately switch the radio back to RX without truncating the
 * tail).
 *
 * `gap_us` is a minimum, not an addition. If the frame's own last pulse is at
 * the idle level it already supplies part of that idle and only the shortfall
 * is appended; and if that trailing idle dwarfs every other pulse in the frame
 * it IS the protocol's framing gap, so the frame's period is emitted exactly as
 * authored and nothing is appended at all. That keeps a frame which carries its
 * own framing gap (a synthesized EV1527 word, or a capture that happened to
 * include one) from going out with a period no real transmitter produces — see
 * the "FRAMING GAP" note in rf_transmit.c.
 */
esp_err_t rf_transmit_frame(const rf_frame_t *frame, uint8_t repeats, uint32_t gap_us);

#ifdef __cplusplus
}
#endif

#endif /* RF_TRANSMIT_H */
