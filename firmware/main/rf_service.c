/*
 * rf_service.c - Radio ownership, burst coalescing and the RX/TX handover.
 *
 * See rf_service.h for the rationale. Three details in here are worth calling
 * out because they are where this kind of code usually goes wrong:
 *
 * 1. THE CAPTURE TASK POLLS IN SHORT SLICES. It holds the radio mutex only for a
 *    50 ms receive window at a time rather than blocking on the queue forever.
 *    If it slept holding the lock, a transmit request would wait for the next
 *    frame — which, on a quiet band, is never. Frames are not lost during the
 *    gap: the RMT peripheral keeps timestamping into its queue in hardware
 *    regardless of whether anyone is currently waiting on it.
 *
 * 2. BURSTS ARE FLUSHED ON A TIMER, NOT ON THE NEXT FRAME. A press yields ~4-6
 *    repeats and then silence. If we only flushed when a dissimilar frame
 *    arrived, the last press of the day would sit in the buffer forever. So the
 *    poll loop also checks for an expired burst window every slice.
 *
 * 3. RSSI IS SAMPLED AT THE START OF THE BURST. By the time the burst is flushed
 *    the transmitter has stopped and the AGC has wound back up to noise, so a
 *    reading taken then would be meaningless.
 */
#include "rf_service.h"

#include <string.h>

#include "board_pins.h"
#include "db_diag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rf_capture.h"
#include "rf_transmit.h"

static const char *TAG = "rf_service";

/* How long after the last similar frame a burst is considered finished. Real
 * remotes repeat every few tens of milliseconds; 250 ms comfortably spans the
 * gaps within one press without merging two deliberate presses together. */
#define BURST_WINDOW_US 250000

/* Similarity tolerances for "this is the same frame again". Cheap transmitters
 * drift by a few percent, and the absolute floor keeps short pulses from failing
 * a purely relative test. */
#define BURST_TOL_PCT 25
#define BURST_TOL_US  120

/* One receive slice. Bounds how long a transmit request waits for the lock. */
#define RX_SLICE_MS 50

/*
 * RSSI squelch — the discriminator that actually works.
 *
 * BENCH DATA (2026-08-31, real doorbell vs. a quiet band):
 *     real presses : 49-53 pulses, -24 to -42 dBm, confidence 67-92%
 *     AGC noise    : 33-92 pulses, -91 to -97 dBm, confidence 24-28%
 *
 * Note the pulse counts OVERLAP — a noise burst can be longer than a real frame,
 * so no min_pulses value can separate them. Signal strength, by contrast, splits
 * the two populations by ~50 dB with nothing in between. With no carrier the
 * CC1101's AGC winds to full gain and hands up amplified thermal noise near the
 * floor; an actual transmitter in the same room is fifty times louder.
 *
 * -75 dBm sits in the middle of that empty gap: roughly 30 dB of headroom below
 * the weakest real press observed, and 16 dB above the loudest noise. A remote
 * would have to be extraordinarily distant to fall below it, and by then its
 * timings would be too mangled to decode anyway.
 */
#define RSSI_SQUELCH_DBM (-75)

/*
 * TX echo suppression — do not react to our own transmissions.
 *
 * Mostly this cannot happen by construction: the capture channel is released for
 * the whole duration of a transmit, so the box is deaf while it keys the
 * carrier. That is why the bring-up auto-replay never ran away.
 *
 * It stops being guaranteed the moment anything else repeats the signal back:
 * a second Klingelbox in earshot, an RF repeater, or a chime that echoes on the
 * same code. Then transmit -> hear -> match -> transmit is a genuine feedback
 * loop that would hammer the band and the chime until power is pulled. The
 * defence is cheap, so it is worth having whether or not that hardware is
 * present today: remember the last frame we sent, and ignore an identical one
 * arriving immediately afterwards.
 *
 * One second is comfortably longer than any echo path and far shorter than a
 * human pressing the same button again on purpose, so a real second press is
 * never swallowed.
 */
#define TX_ECHO_WINDOW_US (1000 * 1000)

static cc1101_handle_t     s_radio;
static cc1101_ident_t      s_ident;
static cc1101_radio_cfg_t  s_radio_cfg;
static SemaphoreHandle_t   s_lock;

static rf_event_cb_t s_cb;
static void         *s_cb_ctx;

/* The burst currently being accumulated. Static, not on the task stack: an
 * rf_event_t is ~2 KB and the capture task does not need that headroom. */
static rf_event_t s_pending;
static bool       s_pending_valid;
static uint32_t   s_squelched;   /* frames dropped below the RSSI floor */

/* Last transmitted frame, for echo suppression (see TX_ECHO_WINDOW_US). */
static rf_frame_t s_tx_last;
static int64_t    s_tx_end_us;
static bool       s_tx_last_valid;
static uint32_t   s_echo_suppressed;

/* ------------------------------------------------------------------ helpers */

static void radio_lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void radio_unlock(void) { xSemaphoreGive(s_lock); }

/* Enter receive: radio in async RX (GDO0 = data out), RMT bound as an input. */
static esp_err_t enter_rx(void)
{
    rf_transmit_release();
    esp_err_t err = cc1101_set_mode(s_radio, CC1101_MODE_RX_ASYNC);
    if (err != ESP_OK)
        return err;
    if (rf_capture_active())
        return ESP_OK;
    rf_capture_cfg_t cfg;
    rf_capture_cfg_default(&cfg, DB_PIN_CC1101_GDO0);
    return rf_capture_init(&cfg);
}

/* Enter transmit: RMT input released FIRST, then the radio flips GDO0 to an
 * input, then we claim the pin as an output. Order matters — doing it the other
 * way round has both chips driving the line. */
static esp_err_t enter_tx(void)
{
    rf_capture_release();
    esp_err_t err = cc1101_set_mode(s_radio, CC1101_MODE_TX_ASYNC);
    if (err != ESP_OK)
        return err;
    rf_transmit_cfg_t cfg;
    rf_transmit_cfg_default(&cfg, DB_PIN_CC1101_GDO0);
    return rf_transmit_init(&cfg);
}

/* Publish the accumulated burst and clear it. */
static void flush_pending(void)
{
    if (!s_pending_valid)
        return;
    s_pending_valid = false;

    rf_normalize(&s_pending.frame, &s_pending.norm);
    bool decoded = rf_decode(&s_pending.frame, &s_pending.norm, &s_pending.decoded);
    s_pending.fingerprint = rf_fingerprint(&s_pending.frame, &s_pending.norm);

    if (s_pending.repeats >= 2)
        db_diag_report(DB_DIAG_REPEAT_FRAME_DETECTED, "%u copies, %u pulses",
                       s_pending.repeats, s_pending.frame.count);

    if (decoded)
        db_diag_report(DB_DIAG_PROTOCOL_DECODED, "%s: %s (base %u us, conf %u%%)",
                       s_pending.decoded.protocol, s_pending.decoded.text,
                       s_pending.norm.base_us, s_pending.norm.confidence);
    else
        db_diag_report(DB_DIAG_UNKNOWN_PROTOCOL_RAW,
                       "%u pulses, base %u us, conf %u%%, fp %08lx",
                       s_pending.frame.count, s_pending.norm.base_us,
                       s_pending.norm.confidence,
                       (unsigned long)s_pending.fingerprint);

    if (s_cb)
        s_cb(&s_pending, s_cb_ctx);
}

/* Fold a freshly captured frame into the pending burst, or start a new one. */
static void absorb(const rf_frame_t *frame)
{
    int64_t now = esp_timer_get_time();

    /* Squelch first: sample the signal strength before doing any other work, and
     * drop frames that came out of the noise floor (see RSSI_SQUELCH_DBM). This
     * is what stops the AGC hash from reaching the decoders — and, later, from
     * being offered as a learn candidate. */
    int rssi = 0;
    if (cc1101_rssi_dbm(s_radio, &rssi) == ESP_OK && rssi < RSSI_SQUELCH_DBM) {
        s_squelched++;
        return;
    }

    /* Our own transmission, bounced back by a repeater or a second box. Dropping
     * it here — before matching, before the graph — is what breaks the
     * transmit/hear/transmit feedback loop. */
    if (s_tx_last_valid && (now - s_tx_end_us) < TX_ECHO_WINDOW_US &&
        rf_frame_similar(&s_tx_last, frame, BURST_TOL_PCT, BURST_TOL_US)) {
        s_echo_suppressed++;
        ESP_LOGD(TAG, "ignored an echo of our own transmission");
        return;
    }

    if (s_pending_valid &&
        (now - s_pending.ts_us) < BURST_WINDOW_US &&
        rf_frame_similar(&s_pending.frame, frame, BURST_TOL_PCT, BURST_TOL_US)) {
        if (s_pending.repeats < 255)
            s_pending.repeats++;
        return;
    }

    /* A different frame arrived — the previous burst is over. */
    flush_pending();

    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.frame   = *frame;
    s_pending.repeats = 1;
    s_pending.ts_us   = now;
    s_pending.rssi_dbm = rssi;   /* sampled above, while the burst is still live */
    s_pending_valid = true;

    db_diag_report(DB_DIAG_PULSES_CAPTURED, "%u pulses, %lu us airtime, %d dBm",
                   frame->count, (unsigned long)rf_frame_duration_us(frame),
                   s_pending.rssi_dbm);
}

/*
 * Periodic health check while the band is quiet. If the radio reports carrier
 * energy but the noise filter is eating everything, that is a specific,
 * actionable state (wrong frequency or bandwidth) and deserves its own report
 * rather than silence.
 */
static void check_idle_health(void)
{
    static int64_t s_last_check;
    int64_t now = esp_timer_get_time();
    if (now - s_last_check < 10 * 1000 * 1000)
        return;
    s_last_check = now;

    rf_capture_stats_t st;
    rf_capture_get_stats(&st);

    bool cs = false;
    if (cc1101_carrier_sense(s_radio, &cs) == ESP_OK && cs && st.frames == 0)
        db_diag_report(DB_DIAG_RF_ENERGY_NO_PULSES,
                       "carrier sense asserted, %lu noise frames filtered, 0 valid",
                       (unsigned long)st.dropped_short);

    /* Squelched frames are normal on a quiet band, so this is a debug-level
     * note rather than a diagnostic state — but it must be visible, because a
     * squelch set too high would look exactly like a dead radio. */
    if (s_squelched) {
        ESP_LOGD(TAG, "squelch: %lu frame(s) below %d dBm since last check",
                 (unsigned long)s_squelched, RSSI_SQUELCH_DBM);
        s_squelched = 0;
    }
}

/* ------------------------------------------------------------- capture task */

static void capture_task(void *arg)
{
    (void)arg;
    static rf_frame_t frame;   /* 1 KB — keep it off the task stack */

    for (;;) {
        radio_lock();
        esp_err_t err = rf_capture_active()
                            ? rf_capture_receive(&frame, pdMS_TO_TICKS(RX_SLICE_MS))
                            : ESP_ERR_TIMEOUT;
        if (err == ESP_OK)
            absorb(&frame);

        /* Flush a burst that has gone quiet, and keep an eye on the band. */
        if (s_pending_valid && (esp_timer_get_time() - s_pending.ts_us) >= BURST_WINDOW_US)
            flush_pending();
        check_idle_health();
        radio_unlock();

        if (err != ESP_OK)
            vTaskDelay(1);   /* yield; the slice already provided the pacing */
    }
}

/* -------------------------------------------------------------- public API */

esp_err_t rf_service_start(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    const cc1101_pins_t pins = {
        .host     = DB_CC1101_SPI_HOST,
        .sck      = DB_PIN_CC1101_SCK,
        .mosi     = DB_PIN_CC1101_MOSI,
        .miso     = DB_PIN_CC1101_MISO,
        .cs       = DB_PIN_CC1101_CS,
        .gdo0     = DB_PIN_CC1101_GDO0,
        .gdo2     = DB_PIN_CC1101_GDO2,
        .clock_hz = DB_CC1101_SPI_HZ,
    };

    esp_err_t err = cc1101_init(&pins, &s_radio);
    if (err != ESP_OK) {
        db_diag_report(DB_DIAG_SPI_ERROR, "cc1101_init: %s", esp_err_to_name(err));
        return err;
    }

    err = cc1101_probe(s_radio, &s_ident);
    if (err != ESP_OK) {
        db_diag_report(DB_DIAG_SPI_ERROR, "probe transaction: %s", esp_err_to_name(err));
        return err;
    }

    if (!s_ident.present) {
        /* Deliberately NOT fatal: keep booting so the UI can explain the fault. */
        db_diag_report(DB_DIAG_CC1101_NOT_DETECTED,
                       "PARTNUM=0x%02X VERSION=0x%02X (a floating MISO reads 0x00/0xFF)",
                       s_ident.partnum, s_ident.version);
        return ESP_OK;
    }

    db_diag_report(DB_DIAG_CC1101_OK, "PARTNUM=0x%02X VERSION=0x%02X%s",
                   s_ident.partnum, s_ident.version,
                   s_ident.version_known ? "" : " (undocumented revision)");

    cc1101_radio_cfg_default(&s_radio_cfg);
    err = cc1101_configure(s_radio, &s_radio_cfg);
    if (err != ESP_OK) {
        db_diag_report(DB_DIAG_RADIO_CONFIG_SUSPECT, "configure: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "radio: %lu Hz, OOK, %lu bps, %lu Hz bw, %d dBm",
             (unsigned long)s_radio_cfg.freq_hz, (unsigned long)s_radio_cfg.datarate_bps,
             (unsigned long)s_radio_cfg.rx_bandwidth_hz, s_radio_cfg.tx_power_dbm);

    rf_decoders_register_builtin();

    radio_lock();
    err = enter_rx();
    radio_unlock();
    if (err != ESP_OK) {
        db_diag_report(DB_DIAG_RADIO_CONFIG_SUSPECT, "enter RX: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(capture_task, "rf_capture", 4096, NULL, 12, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "capture running on GDO0 (GPIO %d)", (int)DB_PIN_CC1101_GDO0);
    return ESP_OK;
}

void rf_service_set_listener(rf_event_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_cb_ctx = ctx;
}

bool rf_service_radio_present(void) { return s_ident.present; }

void rf_service_get_ident(cc1101_ident_t *out) { if (out) *out = s_ident; }

void rf_service_get_radio_cfg(cc1101_radio_cfg_t *out) { if (out) *out = s_radio_cfg; }

esp_err_t rf_service_set_radio_cfg(const cc1101_radio_cfg_t *cfg)
{
    if (!cfg || !s_ident.present)
        return ESP_ERR_INVALID_STATE;
    radio_lock();
    rf_capture_release();
    esp_err_t err = cc1101_configure(s_radio, cfg);
    if (err == ESP_OK) {
        s_radio_cfg = *cfg;
        err = enter_rx();
    }
    radio_unlock();
    return err;
}

esp_err_t rf_service_transmit(const rf_frame_t *frame, uint8_t repeats, uint32_t gap_us)
{
    if (!frame || frame->count == 0)
        return ESP_ERR_INVALID_ARG;
    if (!s_ident.present) {
        db_diag_report(DB_DIAG_TX_FAILED, "no radio detected");
        return ESP_ERR_INVALID_STATE;
    }

    radio_lock();
    esp_err_t err = enter_tx();
    if (err == ESP_OK)
        err = rf_transmit_frame(frame, repeats, gap_us);

    /* Always return to receive, even if the transmit failed — otherwise a single
     * bad send would leave the box deaf until reboot. */
    esp_err_t back = enter_rx();
    radio_unlock();

    if (err != ESP_OK) {
        db_diag_report(DB_DIAG_TX_FAILED, "%s", esp_err_to_name(err));
    } else {
        /* Arm echo suppression. Recorded AFTER the send so the window starts
         * when the carrier stops, not when it started. */
        s_tx_last       = *frame;
        s_tx_end_us     = esp_timer_get_time();
        s_tx_last_valid = true;
        db_diag_report(DB_DIAG_TX_OK, "%u pulses x%u, gap %lu us (software-level)",
                       frame->count, repeats, (unsigned long)gap_us);
    }

    if (back != ESP_OK)
        ESP_LOGE(TAG, "failed to return to RX: %s", esp_err_to_name(back));
    return err;
}

esp_err_t rf_service_rssi(int *dbm_out)
{
    if (!dbm_out || !s_ident.present)
        return ESP_ERR_INVALID_STATE;
    radio_lock();
    esp_err_t err = cc1101_rssi_dbm(s_radio, dbm_out);
    radio_unlock();
    return err;
}
