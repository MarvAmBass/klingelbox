/*
 * rf_capture.c - RMT RX driver for the CC1101's asynchronous OOK data line.
 *
 * The "why RMT" argument is in rf_capture.h and is not repeated here. This file
 * is about the three things that are easy to get wrong once that decision is
 * made, all of which have failure modes that look like "the radio works for a
 * few seconds and then goes deaf".
 *
 * (1) RE-ARMING. rmt_receive() starts exactly ONE reception. The moment the idle
 * threshold fires and the done-callback runs, the receiver is idle and stays
 * idle until somebody calls rmt_receive() again. Forget that call on any path —
 * including the error paths, including the "frame was too short, discard it"
 * path — and capture silently stops forever after the first frame. So re-arming
 * is the FIRST thing the consumer task does with a completed buffer, before any
 * filtering, conversion or queueing, and it re-arms the OTHER buffer so the one
 * being parsed cannot be overwritten underneath us. Two buffers is the minimum
 * that makes that possible; that is the entire reason there are two.
 *
 * (2) WHY THE RE-ARM IS IN A TASK AND NOT IN THE ISR. rmt_receive() may be
 * called from ISR context, and doing so would close the deaf window between the
 * callback and the task waking up. But it only stays safe with flash cache
 * disabled if CONFIG_RMT_RECV_FUNC_IN_IRAM is enabled, and this project's
 * sdkconfig sets CONFIG_RMT_ISR_IRAM_SAFE without it. Calling a flash-resident
 * rmt_receive() from an IRAM ISR while an NVS write has the cache disabled would
 * crash — a rare, load-dependent crash, the worst kind. The callback therefore
 * does nothing but xQueueSendFromISR(), and a high-priority task does the re-arm
 * microseconds later. The cost is that a second transmission arriving inside
 * that window is missed; since these remotes repeat their frame several times,
 * that is an acceptable trade for not crashing during a config save.
 *
 * (3) WHAT THE ISR MAY TOUCH. With CONFIG_RMT_ISR_IRAM_SAFE the callback runs
 * with the flash cache off. No ESP_LOG (format strings are flash rodata), no
 * const tables, no calls into flash-resident code. Everything it reads or writes
 * is in DRAM: the queue handle, a plain counter, and the event data the driver
 * hands it. The buffers themselves are allocated MALLOC_CAP_INTERNAL for the
 * same reason.
 *
 * Two smaller decisions worth stating:
 *
 * GLITCH FILTER CLAMPING. signal_range_min_ns is programmed into an 8-bit
 * hardware field counted at the RMT *group* clock (80 MHz APB on the S3), so the
 * largest glitch filter the silicon can express is ~3.2 us — far below the 20 us
 * the configuration asks for. The driver rejects an over-range value outright
 * with ESP_ERR_INVALID_ARG, so init() clamps and says so in the log rather than
 * failing to start the radio. The residual sub-20 us noise the hardware filter
 * cannot remove is handled upstream by min_pulses and by rf_normalize()'s
 * outlier rejection, which is where noise rejection belongs anyway.
 *
 * TRUNCATED FRAMES ARE DROPPED, NOT DELIVERED. A capture that hit
 * RF_FRAME_MAX_PULSES is missing its tail and would replay as a different
 * waveform, so it is counted in dropped_full and thrown away. Delivering it
 * would put a subtly wrong recording into the signal store, which is worse than
 * delivering nothing.
 */
#include <inttypes.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_attr.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "rf_capture.h"

static const char *TAG = "rf_capture";

/* Two pulses per RMT symbol, plus a little slack for the driver's end marker. */
#define RF_RX_BUF_SYMBOLS       ((RF_FRAME_MAX_PULSES / 2) + 8)
#define RF_RX_BUF_BYTES         (RF_RX_BUF_SYMBOLS * sizeof(rmt_symbol_word_t))
/* Exactly two: one armed in hardware, one being parsed. See (1) above. */
#define RF_RX_BUF_COUNT         2

/* 128 symbols => 3 of the S3's 48-symbol memory blocks. Deep enough that the
 * ping-pong copy-out happens a few times per frame instead of every few pulses,
 * which keeps the ISR rate down; DMA is deliberately off (S3 RMT RX DMA is
 * restricted and buys nothing at 1 us ticks and ~50 pulses per frame). */
#define RF_RX_MEM_BLOCK_SYMBOLS 128

/* Hardware limits behind rmt_receive()'s range validation. */
#define RF_RMT_FILTER_MAX_TICKS 255u     /* 8-bit field, counted at the group clock */
#define RF_RMT_IDLE_MAX_TICKS   32767u   /* 15-bit field, counted at channel resolution */

#define RF_RX_TASK_STACK        4096
#define RF_RX_TASK_PRIO         10

/* ISR -> consumer task. Carries only what the ISR already has in registers. */
typedef struct {
    rmt_symbol_word_t *syms;
    size_t             num;
} rf_rx_done_msg_t;

static struct {
    bool                 active;
    gpio_num_t           pin;
    uint32_t             resolution_hz;
    uint16_t             min_pulses;
    rmt_channel_handle_t chan;
    rmt_receive_config_t rx_cfg;
    rmt_symbol_word_t   *bufs[RF_RX_BUF_COUNT];
    QueueHandle_t        q_done;      /* rf_rx_done_msg_t, ISR -> task */
    QueueHandle_t        q_frames;    /* rf_frame_t by value, task -> consumer */
    TaskHandle_t         task;
    SemaphoreHandle_t    task_gone;
    volatile bool        task_run;
    rf_capture_stats_t   stats;       /* written by the task only */
} s_cap;

/* Written by the ISR only, read by rf_capture_get_stats(). 32-bit aligned, so
 * the read is atomic and no lock is needed for a diagnostic counter. */
static volatile uint32_t s_isr_overruns;

/* ---- defaults ------------------------------------------------------------ */

void rf_capture_cfg_default(rf_capture_cfg_t *out, gpio_num_t pin)
{
    if (out == NULL) {
        return;
    }
    out->pin           = pin;
    out->resolution_hz = 1000000u;   /* 1 us ticks; 15-bit symbol => 32767 us max */
    /* 3 us. The natural choice would be ~20 us (two orders below the
     * few-hundred-us pulses these remotes use), but the hardware cannot express
     * it: signal_range_min_ns is an 8-bit tick count at the RMT group clock
     * (80 MHz on the S3), capping the filter at ~3.2 us. So we ask for what the
     * silicon can actually do and let the remaining sub-20-us AGC hash be
     * rejected downstream by min_pulses and rf_normalize's clustering, which is
     * where it was always going to be caught anyway. Requesting 20000 here would
     * work — rf_capture_init() clamps it — but it would warn on every boot about
     * a limit we already know about, which trains people to ignore warnings. */
    out->glitch_ns     = 3000u;
    /* 8 ms of stillness ends a frame. Longer than any intra-frame gap in the
     * EV1527 family (a 3x long pulse at a 350 us base is ~1 ms) and shorter than
     * the ~10.8 ms sync gap, so the sync gap is what terminates the receive. */
    out->idle_us       = 8000u;
    /* 32. BENCH FINDING (2026-08-31): with the AGC wound up on a silent band the
     * CC1101's OOK output idles HIGH with ~25 us LOW dips, which the 3.2 us
     * glitch filter cannot touch. Those bursts terminate on the 8 ms idle
     * threshold at a very consistent ~17 pulses, so 16 let every one of them
     * through. An EV1527-class frame is ~50 pulses (24 bits x 2 + sync), so 32
     * sits comfortably between the two populations: it rejects the noise
     * outright while leaving a wide margin below any real remote's frame. */
    out->min_pulses    = 32;
    out->queue_depth   = 4;
}

/* ---- ISR ----------------------------------------------------------------- */

/*
 * IRAM_ATTR, and everything it touches lives in DRAM. Absolutely no logging, no
 * string literals, no flash-resident constants: with CONFIG_RMT_ISR_IRAM_SAFE
 * this runs with the flash cache disabled.
 */
static bool IRAM_ATTR rf_rx_done_cb(rmt_channel_handle_t chan,
                                    const rmt_rx_done_event_data_t *edata,
                                    void *user_ctx)
{
    BaseType_t hp_woken = pdFALSE;
    rf_rx_done_msg_t msg;

    (void)chan;
    (void)user_ctx;

    msg.syms = edata->received_symbols;
    msg.num  = edata->num_symbols;

    if (xQueueSendFromISR(s_cap.q_done, &msg, &hp_woken) != pdTRUE) {
        s_isr_overruns++;   /* consumer task starved; this buffer is lost */
    }
    return hp_woken == pdTRUE;
}

/* ---- conversion (task context) ------------------------------------------- */

/*
 * rmt_symbol_word_t packs two half-symbols (level0/duration0, level1/duration1).
 * A zero duration is the driver's end marker, and it can legitimately appear in
 * either half of the last symbol. Levels alternate by construction, so only the
 * first one is recorded and the rest are implied — see rf_frame.h.
 *
 * Returns false when the frame hit RF_FRAME_MAX_PULSES (i.e. was truncated).
 */
static bool rf_symbols_to_frame(const rmt_symbol_word_t *syms, size_t num,
                                uint32_t resolution_hz, rf_frame_t *f)
{
    const bool one_mhz = (resolution_hz == 1000000u);

    rf_frame_reset(f);
    if (num == 0) {
        return true;
    }
    f->first_level = (uint8_t)(syms[0].level0 & 1u);

    for (size_t i = 0; i < num; i++) {
        uint32_t d[2] = { syms[i].duration0, syms[i].duration1 };

        for (int h = 0; h < 2; h++) {
            uint32_t us;

            if (d[h] == 0) {
                return true;                 /* end marker: frame complete */
            }
            us = one_mhz ? d[h]
                         : (uint32_t)(((uint64_t)d[h] * 1000000ull) / resolution_hz);
            if (us > 65535u) {
                us = 65535u;                 /* cannot happen at 1 MHz, but be safe */
            }
            if (!rf_frame_push(f, (uint16_t)us)) {
                return false;                /* truncated */
            }
        }
    }
    return true;
}

/* ---- consumer task ------------------------------------------------------- */

static esp_err_t rf_rx_arm(rmt_symbol_word_t *buf)
{
    return rmt_receive(s_cap.chan, buf, RF_RX_BUF_BYTES, &s_cap.rx_cfg);
}

static void rf_capture_task(void *arg)
{
    rf_rx_done_msg_t msg;
    rf_frame_t frame;

    (void)arg;

    while (s_cap.task_run) {
        if (xQueueReceive(s_cap.q_done, &msg, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;   /* periodic wakeup so task_run is noticed promptly */
        }

        /* RE-ARM FIRST, on the buffer the hardware did NOT just fill. Nothing
         * below this point may return or continue before this has happened. */
        {
            rmt_symbol_word_t *next = (msg.syms == s_cap.bufs[0]) ? s_cap.bufs[1]
                                                                  : s_cap.bufs[0];
            esp_err_t err = rf_rx_arm(next);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "re-arm failed: %s — capture has stopped",
                         esp_err_to_name(err));
            }
        }

        if (!rf_symbols_to_frame(msg.syms, msg.num, s_cap.resolution_hz, &frame)) {
            s_cap.stats.dropped_full++;
            ESP_LOGW(TAG, "frame truncated at %d pulses, discarded",
                     RF_FRAME_MAX_PULSES);
            continue;
        }
        if (frame.count < s_cap.min_pulses) {
            s_cap.stats.dropped_short++;   /* RF energy, but no pulse stream */
            continue;
        }
        if (xQueueSend(s_cap.q_frames, &frame, 0) != pdTRUE) {
            s_cap.stats.overruns++;
            ESP_LOGW(TAG, "frame queue full, dropping a %u-pulse frame",
                     (unsigned)frame.count);
            continue;
        }
        s_cap.stats.frames++;
    }

    xSemaphoreGive(s_cap.task_gone);
    vTaskDelete(NULL);
}

/* ---- init / release ------------------------------------------------------ */

/* Frees everything; safe to call at any point during a partially-built init. */
static void rf_capture_teardown(void)
{
    /* Stop the consumer BEFORE the channel goes away: it re-arms unconditionally
     * on every completion, and re-arming a deleted channel would log an error
     * storm on every shutdown. */
    if (s_cap.task != NULL) {
        s_cap.task_run = false;
        if (xSemaphoreTake(s_cap.task_gone, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(TAG, "consumer task did not exit; leaking its resources");
        }
        s_cap.task = NULL;
    }
    if (s_cap.chan != NULL) {
        rmt_disable(s_cap.chan);          /* aborts any pending receive */
        rmt_del_channel(s_cap.chan);
        s_cap.chan = NULL;
    }
    if (s_cap.task_gone != NULL) {
        vSemaphoreDelete(s_cap.task_gone);
        s_cap.task_gone = NULL;
    }
    for (int i = 0; i < RF_RX_BUF_COUNT; i++) {
        if (s_cap.bufs[i] != NULL) {
            heap_caps_free(s_cap.bufs[i]);
            s_cap.bufs[i] = NULL;
        }
    }
    if (s_cap.q_done != NULL) {
        vQueueDelete(s_cap.q_done);
        s_cap.q_done = NULL;
    }
    if (s_cap.q_frames != NULL) {
        vQueueDelete(s_cap.q_frames);
        s_cap.q_frames = NULL;
    }
    if (s_cap.pin != GPIO_NUM_NC) {
        /* Hand the pin back so rf_transmit can bind it — RMT refuses to attach
         * two channels to one GPIO. See "PIN OWNERSHIP" in rf_capture.h. */
        gpio_reset_pin(s_cap.pin);
        s_cap.pin = GPIO_NUM_NC;
    }
    s_cap.active = false;
}

/* The RMT group (source) clock, which is what the RX glitch filter counts. */
static uint32_t rf_rmt_group_clock_hz(void)
{
    uint32_t hz = 0;

    if (esp_clk_tree_src_get_freq_hz((soc_module_clk_t)RMT_CLK_SRC_DEFAULT,
                                     ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED,
                                     &hz) != ESP_OK || hz == 0) {
        hz = 80000000u;   /* APB on the ESP32-S3 */
    }
    return hz;
}

esp_err_t rf_capture_init(const rf_capture_cfg_t *cfg)
{
    esp_err_t err;
    uint32_t group_hz, max_glitch_ns, max_idle_ns;
    uint32_t glitch_ns, idle_ns;

    if (cfg == NULL || cfg->pin == GPIO_NUM_NC || cfg->resolution_hz == 0 ||
        cfg->idle_us == 0 || cfg->queue_depth == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cap.active) {
        ESP_LOGW(TAG, "already initialized on GPIO %d", (int)s_cap.pin);
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_cap, 0, sizeof(s_cap));
    s_cap.pin           = GPIO_NUM_NC;   /* not ours until the channel is created */
    s_cap.resolution_hz = cfg->resolution_hz;
    s_cap.min_pulses    = cfg->min_pulses;
    s_isr_overruns      = 0;

    /* --- validate and clamp the two range thresholds --------------------- */
    group_hz      = rf_rmt_group_clock_hz();
    max_glitch_ns = (uint32_t)(((uint64_t)RF_RMT_FILTER_MAX_TICKS * 1000000000ull) / group_hz);
    max_idle_ns   = (uint32_t)(((uint64_t)RF_RMT_IDLE_MAX_TICKS * 1000000000ull) / cfg->resolution_hz);

    glitch_ns = cfg->glitch_ns;
    if (glitch_ns > max_glitch_ns) {
        ESP_LOGW(TAG, "glitch filter %" PRIu32 " ns exceeds the hardware maximum "
                      "(%" PRIu32 " ns at a %" PRIu32 " Hz group clock); clamping. "
                      "Sub-%" PRIu32 " us spikes are rejected by min_pulses and "
                      "rf_normalize instead.",
                 glitch_ns, max_glitch_ns, group_hz, cfg->glitch_ns / 1000u);
        glitch_ns = max_glitch_ns;
    }

    idle_ns = cfg->idle_us * 1000u;
    if (idle_ns > max_idle_ns) {
        ESP_LOGW(TAG, "idle threshold %" PRIu32 " us exceeds the hardware maximum "
                      "(%" PRIu32 " us at %" PRIu32 " Hz); clamping",
                 cfg->idle_us, max_idle_ns / 1000u, cfg->resolution_hz);
        idle_ns = max_idle_ns;
    }

    s_cap.rx_cfg.signal_range_min_ns = glitch_ns;
    s_cap.rx_cfg.signal_range_max_ns = idle_ns;
    s_cap.rx_cfg.flags.en_partial_rx = 0;

    /* --- buffers: internal RAM, because the ISR path touches them --------- */
    for (int i = 0; i < RF_RX_BUF_COUNT; i++) {
        s_cap.bufs[i] = heap_caps_malloc(RF_RX_BUF_BYTES,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (s_cap.bufs[i] == NULL) {
            ESP_LOGE(TAG, "no internal RAM for a %u-byte RX buffer",
                     (unsigned)RF_RX_BUF_BYTES);
            rf_capture_teardown();
            return ESP_ERR_NO_MEM;
        }
    }

    s_cap.q_done    = xQueueCreate(RF_RX_BUF_COUNT, sizeof(rf_rx_done_msg_t));
    s_cap.q_frames  = xQueueCreate(cfg->queue_depth, sizeof(rf_frame_t));
    s_cap.task_gone = xSemaphoreCreateBinary();
    if (s_cap.q_done == NULL || s_cap.q_frames == NULL || s_cap.task_gone == NULL) {
        ESP_LOGE(TAG, "queue allocation failed (frame queue is %u x %u bytes)",
                 cfg->queue_depth, (unsigned)sizeof(rf_frame_t));
        rf_capture_teardown();
        return ESP_ERR_NO_MEM;
    }

    /* --- the channel ------------------------------------------------------ */
    {
        rmt_rx_channel_config_t chan_cfg = {
            .gpio_num          = cfg->pin,
            .clk_src           = RMT_CLK_SRC_DEFAULT,
            .resolution_hz     = cfg->resolution_hz,
            .mem_block_symbols = RF_RX_MEM_BLOCK_SYMBOLS,
            .intr_priority     = 0,
            .flags = {
                .invert_in    = 0,
                .with_dma     = 0,   /* see RF_RX_MEM_BLOCK_SYMBOLS */
                .io_loop_back = 0,
            },
        };
        err = rmt_new_rx_channel(&chan_cfg, &s_cap.chan);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "rmt_new_rx_channel(GPIO %d) failed: %s — is the "
                          "transmitter still holding the pin?",
                     (int)cfg->pin, esp_err_to_name(err));
            rf_capture_teardown();
            return err;
        }
    }
    s_cap.pin = cfg->pin;

    {
        rmt_rx_event_callbacks_t cbs = { .on_recv_done = rf_rx_done_cb };
        err = rmt_rx_register_event_callbacks(s_cap.chan, &cbs, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "callback registration failed: %s", esp_err_to_name(err));
            rf_capture_teardown();
            return err;
        }
    }

    /* The task must exist before the channel is enabled, or the first frame's
     * done-message has nobody to re-arm on. */
    s_cap.task_run = true;
    if (xTaskCreate(rf_capture_task, "rf_capture", RF_RX_TASK_STACK, NULL,
                    RF_RX_TASK_PRIO, &s_cap.task) != pdPASS) {
        ESP_LOGE(TAG, "consumer task creation failed");
        s_cap.task_run = false;
        s_cap.task = NULL;
        rf_capture_teardown();
        return ESP_ERR_NO_MEM;
    }

    err = rmt_enable(s_cap.chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable failed: %s", esp_err_to_name(err));
        rf_capture_teardown();
        return err;
    }

    err = rf_rx_arm(s_cap.bufs[0]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "initial rmt_receive failed: %s", esp_err_to_name(err));
        rf_capture_teardown();
        return err;
    }

    s_cap.active = true;
    ESP_LOGI(TAG, "listening on GPIO %d: %" PRIu32 " Hz (%" PRIu32 " ns/tick), "
                  "glitch %" PRIu32 " ns, idle %" PRIu32 " us, min %u pulses, "
                  "queue %u frames",
             (int)cfg->pin, cfg->resolution_hz, 1000000000u / cfg->resolution_hz,
             glitch_ns, idle_ns / 1000u, cfg->min_pulses, cfg->queue_depth);
    return ESP_OK;
}

void rf_capture_release(void)
{
    if (!s_cap.active && s_cap.chan == NULL && s_cap.task == NULL) {
        return;   /* idempotent: releasing twice, or before init, is a no-op */
    }
    ESP_LOGI(TAG, "releasing GPIO %d (frames=%" PRIu32 " short=%" PRIu32
                  " full=%" PRIu32 " overruns=%" PRIu32 ")",
             (int)s_cap.pin, s_cap.stats.frames, s_cap.stats.dropped_short,
             s_cap.stats.dropped_full, s_cap.stats.overruns + s_isr_overruns);
    rf_capture_teardown();
}

bool rf_capture_active(void)
{
    return s_cap.active;
}

esp_err_t rf_capture_receive(rf_frame_t *out, TickType_t wait)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_cap.active || s_cap.q_frames == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xQueueReceive(s_cap.q_frames, out, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void rf_capture_get_stats(rf_capture_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_cap.stats;
    /* Buffers lost inside the ISR are consumer-too-slow events too. */
    out->overruns += s_isr_overruns;
}

void rf_capture_reset_stats(void)
{
    memset(&s_cap.stats, 0, sizeof(s_cap.stats));
    s_isr_overruns = 0;
}
