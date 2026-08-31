/*
 * rf_transmit.c - RMT TX replay of a raw pulse frame onto the CC1101 data line.
 *
 * The mirror image of rf_capture.c, and deliberately just as ignorant: it is
 * handed an rf_frame_t of alternating durations and reproduces them. Because a
 * recording is nothing but timings, this replays protocols the firmware cannot
 * decode — which is the whole reason the pulse layer exists.
 *
 * THE 15-BIT PROBLEM, WHICH IS NOT OPTIONAL TO SOLVE. An rmt_symbol_word_t
 * carries a 15-bit duration: 32767 ticks, so 32767 us at our 1 MHz resolution.
 * rf_frame_t stores 16-bit microseconds, so a legal frame can contain a pulse
 * the hardware cannot express in one symbol — and an EV1527 sync gap at a slow
 * base is exactly such a pulse (31 x 1200 us = 37.2 ms). Writing it into the
 * bitfield truncates modulo 32768 and produces a 4.4 ms gap: silently wrong
 * timing that no receiver reacts to and that looks like a wiring fault. Long
 * pulses are therefore SPLIT across consecutive symbols at the same level,
 * which the peripheral emits back-to-back with no seam.
 *
 * WHY THE GAP IS SYMBOLS AND NOT A vTaskDelay. Real receivers require two or
 * more consistent copies of a frame before acting (a cheap and effective noise
 * filter), so the inter-frame gap is part of the waveform, not a scheduling
 * detail. A delay between two rmt_transmit() calls would be at the mercy of the
 * FreeRTOS tick and of anything else running, and a gap that varies by
 * milliseconds can push the receiver's repeat detector outside its window. The
 * gap is emitted by the same peripheral that emits the pulses, so it is as
 * accurate as they are.
 *
 * WHY _frame() BLOCKS. The caller's next move after transmitting is to flip the
 * CC1101 back to RX and give the pin back to rf_capture. Doing that while the
 * peripheral still has symbols queued truncates the tail of the transmission —
 * and the caller has no way to know it happened. rmt_tx_wait_all_done() makes
 * the function's completion mean what it appears to mean.
 *
 * See rf_capture.h "PIN OWNERSHIP": rf_capture_release() must be called first.
 * That is the caller's job (rf_service serializes it under a mutex); all this
 * file can do is report the resulting bind failure clearly.
 */
#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "rf_transmit.h"

static const char *TAG = "rf_transmit";

/* Largest duration a single RMT symbol half can express (15-bit field). */
#define RF_TX_MAX_TICKS      32767u

/* Worst case, every one of the 512 durations is 65535 us and needs three
 * 32767-tick pieces; plus up to three more for the trailing gap, plus a pad. */
#define RF_TX_MAX_ENTRIES    (3u * RF_FRAME_MAX_PULSES + 4u)
#define RF_TX_MAX_SYMBOLS    ((RF_TX_MAX_ENTRIES + 1u) / 2u)

#define RF_TX_MEM_BLOCK_SYMBOLS  64
#define RF_TX_QUEUE_DEPTH        4

static struct {
    bool                 active;
    gpio_num_t           pin;
    uint32_t             resolution_hz;
    uint8_t              idle_level;
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t encoder;
    rmt_symbol_word_t   *syms;     /* internal RAM: the driver's ISR reads it */
} s_tx;

void rf_transmit_cfg_default(rf_transmit_cfg_t *out, gpio_num_t pin)
{
    if (out == NULL) {
        return;
    }
    out->pin           = pin;
    /* Must match rf_capture's resolution or a replay is not a replay. */
    out->resolution_hz = 1000000u;
    /* OOK: idle low means no carrier. Leaving the line high between frames would
     * hold the CC1101 keyed and jam the band. */
    out->idle_level    = 0;
}

/* ---- symbol building ----------------------------------------------------- */

/*
 * Append `us` microseconds at `level`, splitting across as many symbol halves as
 * the 15-bit duration field requires. Returns false if the buffer is exhausted.
 */
typedef struct {
    rmt_symbol_word_t *syms;
    size_t             cap_entries;   /* in half-symbols */
    size_t             n_entries;
    uint32_t           resolution_hz;
} rf_tx_builder_t;

static bool rf_tx_put(rf_tx_builder_t *b, uint8_t level, uint32_t us)
{
    uint64_t ticks;

    if (us == 0) {
        return true;   /* a zero-length pulse is not a pulse; and a zero duration
                        * in the middle of the stream would end the transmission */
    }
    ticks = ((uint64_t)us * b->resolution_hz) / 1000000ull;
    if (ticks == 0) {
        ticks = 1;
    }

    while (ticks > 0) {
        uint32_t chunk = (ticks > RF_TX_MAX_TICKS) ? RF_TX_MAX_TICKS : (uint32_t)ticks;
        size_t idx = b->n_entries / 2u;

        if (b->n_entries >= b->cap_entries) {
            return false;
        }
        /* Splitting a long pulse: consecutive halves at the SAME level are
         * emitted seamlessly, so the wire sees one continuous pulse. */
        if ((b->n_entries & 1u) == 0u) {
            b->syms[idx].level0    = level & 1u;
            b->syms[idx].duration0 = chunk;
            b->syms[idx].level1    = level & 1u;   /* pre-set; overwritten below */
            b->syms[idx].duration1 = 0;
        } else {
            b->syms[idx].level1    = level & 1u;
            b->syms[idx].duration1 = chunk;
        }
        b->n_entries++;
        ticks -= chunk;
    }
    return true;
}

/*
 * Pad to a whole number of symbols.
 *
 * The obvious pad is a zero-duration half, since that is the peripheral's
 * end-of-transmission marker — but the driver appends its OWN EOF symbol after
 * the payload, and stopping the hardware one symbol early on our marker instead
 * of the driver's is exactly the kind of thing that turns rmt_tx_wait_all_done()
 * into a hang. So the pad is one tick at the idle level instead: sub-microsecond
 * at 1 MHz, invisible on an OOK line that is about to go idle anyway, and it
 * leaves the driver's completion bookkeeping untouched. This is also why
 * rf_tx_put() silently drops zero-length pulses rather than emitting them.
 */
static size_t rf_tx_finish(rf_tx_builder_t *b, uint8_t idle_level)
{
    if ((b->n_entries & 1u) != 0u) {
        size_t idx = b->n_entries / 2u;
        b->syms[idx].level1    = idle_level & 1u;
        b->syms[idx].duration1 = 1;
        b->n_entries++;
    }
    return b->n_entries / 2u;
}

/* ---- init / release ------------------------------------------------------ */

static void rf_transmit_teardown(void)
{
    if (s_tx.chan != NULL) {
        rmt_disable(s_tx.chan);
        rmt_del_channel(s_tx.chan);
        s_tx.chan = NULL;
    }
    if (s_tx.encoder != NULL) {
        rmt_del_encoder(s_tx.encoder);
        s_tx.encoder = NULL;
    }
    if (s_tx.syms != NULL) {
        heap_caps_free(s_tx.syms);
        s_tx.syms = NULL;
    }
    if (s_tx.pin != GPIO_NUM_NC) {
        gpio_reset_pin(s_tx.pin);   /* give the pin back to rf_capture */
        s_tx.pin = GPIO_NUM_NC;
    }
    s_tx.active = false;
}

esp_err_t rf_transmit_init(const rf_transmit_cfg_t *cfg)
{
    esp_err_t err;

    if (cfg == NULL || cfg->pin == GPIO_NUM_NC || cfg->resolution_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx.active) {
        ESP_LOGW(TAG, "already initialized on GPIO %d", (int)s_tx.pin);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_tx, 0, sizeof(s_tx));
    s_tx.pin           = GPIO_NUM_NC;
    s_tx.resolution_hz = cfg->resolution_hz;
    s_tx.idle_level    = cfg->idle_level ? 1u : 0u;

    s_tx.syms = heap_caps_malloc(RF_TX_MAX_SYMBOLS * sizeof(rmt_symbol_word_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_tx.syms == NULL) {
        ESP_LOGE(TAG, "no internal RAM for %u TX symbols", (unsigned)RF_TX_MAX_SYMBOLS);
        rf_transmit_teardown();
        return ESP_ERR_NO_MEM;
    }

    {
        rmt_tx_channel_config_t chan_cfg = {
            .gpio_num          = cfg->pin,
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = cfg->resolution_hz,
            .mem_block_symbols = RF_TX_MEM_BLOCK_SYMBOLS,
            .trans_queue_depth = RF_TX_QUEUE_DEPTH,
            .intr_priority     = 0,
            .flags = {
                .invert_out   = 0,
                .with_dma     = 0,
                .io_loop_back = 0,
                .io_od_mode   = 0,
            },
        };
        err = rmt_new_tx_channel(&chan_cfg, &s_tx.chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_new_tx_channel(GPIO %d) failed: %s — call "
                          "rf_capture_release() before claiming the pin",
                     (int)cfg->pin, esp_err_to_name(err));
            rf_transmit_teardown();
            return err;
        }
    }
    s_tx.pin = cfg->pin;

    {
        /* Empty struct in IDF 5.3 — it has no fields at all, so `{ 0 }`
         * is an excess initializer and warns. `{}` is what the ESP-IDF
         * examples use for exactly this type. */
        rmt_copy_encoder_config_t enc_cfg = {};
        err = rmt_new_copy_encoder(&enc_cfg, &s_tx.encoder);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
            rf_transmit_teardown();
            return err;
        }
    }

    err = rmt_enable(s_tx.chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rf_transmit_teardown();
        return err;
    }

    s_tx.active = true;
    ESP_LOGI(TAG, "ready on GPIO %d: %" PRIu32 " Hz, idle level %u",
             (int)cfg->pin, cfg->resolution_hz, s_tx.idle_level);
    return ESP_OK;
}

void rf_transmit_release(void)
{
    if (!s_tx.active && s_tx.chan == NULL && s_tx.syms == NULL) {
        return;   /* idempotent */
    }
    ESP_LOGI(TAG, "releasing GPIO %d", (int)s_tx.pin);
    rf_transmit_teardown();
}

bool rf_transmit_active(void)
{
    return s_tx.active;
}

/* ---- transmit ------------------------------------------------------------ */

esp_err_t rf_transmit_frame(const rf_frame_t *frame, uint8_t repeats, uint32_t gap_us)
{
    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = {
            /* Leave the line where the radio expects to find it: for OOK that is
             * low, i.e. carrier off. */
            .eot_level         = 0,
            .queue_nonblocking = 0,
        },
    };
    rf_tx_builder_t b;
    size_t n_symbols;
    esp_err_t err;
    uint8_t n_reps = (repeats == 0) ? 1u : repeats;

    if (frame == NULL || frame->count == 0 || frame->count > RF_FRAME_MAX_PULSES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_tx.active) {
        ESP_LOGE(TAG, "not initialized (does rf_capture still own the pin?)");
        return ESP_ERR_INVALID_STATE;
    }
    tx_cfg.flags.eot_level = s_tx.idle_level;

    /* Build the waveform once: one copy of the frame plus the trailing idle gap.
     * The gap is included in every copy — including the last — so the radio is
     * guaranteed to be quiet for gap_us before the caller switches it back to
     * RX, and so a receiver counting repeats sees a uniform cadence. */
    b.syms          = s_tx.syms;
    b.cap_entries   = RF_TX_MAX_ENTRIES;
    b.n_entries     = 0;
    b.resolution_hz = s_tx.resolution_hz;

    for (uint16_t i = 0; i < frame->count; i++) {
        if (!rf_tx_put(&b, rf_frame_level_at(frame, i), frame->durations_us[i])) {
            ESP_LOGE(TAG, "frame does not fit in %u symbols", (unsigned)RF_TX_MAX_SYMBOLS);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    if (gap_us > 0 && !rf_tx_put(&b, s_tx.idle_level, gap_us)) {
        ESP_LOGE(TAG, "inter-frame gap does not fit in %u symbols",
                 (unsigned)RF_TX_MAX_SYMBOLS);
        return ESP_ERR_INVALID_SIZE;
    }
    n_symbols = rf_tx_finish(&b, s_tx.idle_level);

    for (uint8_t r = 0; r < n_reps; r++) {
        err = rmt_transmit(s_tx.chan, s_tx.encoder, s_tx.syms,
                           n_symbols * sizeof(rmt_symbol_word_t), &tx_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_transmit copy %u/%u failed: %s",
                     r + 1u, n_reps, esp_err_to_name(err));
            (void)rmt_tx_wait_all_done(s_tx.chan, -1);
            return err;
        }
    }

    /* Block until the peripheral is genuinely finished — see the module comment. */
    err = rmt_tx_wait_all_done(s_tx.chan, -1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "sent %u pulses x%u (%" PRIu32 " us airtime each, %" PRIu32
                  " us gap, %u symbols)",
             (unsigned)frame->count, n_reps, rf_frame_duration_us(frame),
             gap_us, (unsigned)n_symbols);
    return ESP_OK;
}
