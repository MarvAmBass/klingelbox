/*
 * rf_capture.h - Hardware-timestamped OOK edge capture via the RMT peripheral.
 *
 * WHY RMT AND NOT A GPIO ISR. A naive `micros()`-in-an-ISR approach has to win a
 * race against Wi-Fi, lwIP, the HTTP server and flash operations for every single
 * edge; at the few-hundred-microsecond pulse widths these remotes use, the jitter
 * corrupts captures in exactly the situations that matter (while the UI is open).
 * The RMT receiver timestamps edges in HARDWARE into a symbol buffer and only
 * interrupts once per frame, so background activity cannot smear the timings.
 *
 * The configuration does three jobs at once:
 *   - resolution_hz = 1 MHz gives 1 us ticks. An RMT symbol carries a 15-bit
 *     duration, so a single pulse tops out at 32767 us; anything longer is an
 *     inter-frame gap, which is precisely what we want to treat as a boundary.
 *   - glitch_ns (RMT's signal_range_min_ns) drops the shortest spikes. With no
 *     carrier the CC1101's AGC winds up and the data line is continuous noise, so
 *     some rejection is load-bearing. Note the hardware ceiling: this field is an
 *     8-bit tick count at the RMT group clock (80 MHz on the S3), so the filter
 *     tops out near 3.2 us — far short of the ~20 us one might want. The rest of
 *     the noise is therefore rejected downstream by min_pulses and by
 *     rf_normalize's clustering. rf_capture_init() clamps an over-range request
 *     and logs it rather than failing.
 *   - idle_us (RMT's signal_range_max_ns) IS the frame-boundary detector: when the
 *     line stays still that long, the receiver completes and hands up the frame.
 *
 * ISR DISCIPLINE. sdkconfig sets CONFIG_RMT_ISR_IRAM_SAFE=y so capture keeps
 * working while the flash cache is disabled (e.g. during an NVS save). The
 * receive-done callback must therefore be IRAM_ATTR, must touch no
 * flash-resident constants, and must do nothing but hand the buffer to a queue.
 * All parsing happens in task context.
 *
 * PIN OWNERSHIP. Capture and transmit share GDO0, and RMT will not bind two
 * channels to one GPIO. Exactly one of the two may be initialized at a time; the
 * caller (rf_service) serializes this under a mutex. Always _release() one before
 * _init()ing the other.
 */
#ifndef RF_CAPTURE_H
#define RF_CAPTURE_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "rf_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin;             /* the CC1101 GDO0 data line */
    uint32_t   resolution_hz;   /* 1000000 => 1 us ticks */
    uint32_t   glitch_ns;       /* shortest pulse considered real */
    uint32_t   idle_us;         /* silence that ends a frame (e.g. 8000) */
    uint16_t   min_pulses;      /* frames shorter than this are noise, dropped */
    uint8_t    queue_depth;     /* completed frames buffered for the consumer */
} rf_capture_cfg_t;

/* Reasonable defaults for 433 MHz remote-control traffic. */
void rf_capture_cfg_default(rf_capture_cfg_t *out, gpio_num_t pin);

/* Create the RMT RX channel and start receiving. */
esp_err_t rf_capture_init(const rf_capture_cfg_t *cfg);

/* Stop receiving, delete the channel and release the GPIO so the transmitter can
 * claim it. Safe to call when not initialized. */
void rf_capture_release(void);

bool rf_capture_active(void);

/*
 * Block until the next frame that passed the min_pulses filter, or timeout.
 * Returns ESP_OK with *out filled, or ESP_ERR_TIMEOUT.
 */
esp_err_t rf_capture_receive(rf_frame_t *out, TickType_t wait);

/* Counters for diagnostics: how much the radio is hearing vs. how much survives
 * the noise filter. A large `dropped_short` with zero `frames` is the signature
 * of "RF energy present but no valid pulse stream". */
typedef struct {
    uint32_t frames;          /* delivered to the consumer */
    uint32_t dropped_short;   /* below min_pulses (noise) */
    uint32_t dropped_full;    /* hit RF_FRAME_MAX_PULSES, truncated */
    uint32_t overruns;        /* consumer too slow; frame discarded */
} rf_capture_stats_t;

void rf_capture_get_stats(rf_capture_stats_t *out);
void rf_capture_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* RF_CAPTURE_H */
