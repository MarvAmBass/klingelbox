/*
 * db_diag.c - Implementation of the shared diagnostic vocabulary (see db_diag.h).
 *
 * Intentionally trivial: a fixed-size table, a mutex, and a printf. The value is
 * not in the mechanism but in every layer funnelling through the SAME one, so a
 * user reading the serial console, the REST API and the UI sees identical names.
 */
#include "db_diag.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "db_diag";

static db_diag_entry_t s_entries[DB_DIAG__COUNT];
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

/* Names double as the REST API's wire format — do not rename casually. */
static const char *const s_names[DB_DIAG__COUNT] = {
    [DB_DIAG_CC1101_NOT_DETECTED]  = "CC1101_NOT_DETECTED",
    [DB_DIAG_CC1101_OK]            = "CC1101_OK",
    [DB_DIAG_SPI_ERROR]            = "SPI_ERROR",
    [DB_DIAG_RADIO_CONFIG_SUSPECT] = "RADIO_CONFIG_SUSPECT",
    [DB_DIAG_RF_ENERGY_NO_PULSES]  = "RF_ENERGY_NO_PULSES",
    [DB_DIAG_PULSES_CAPTURED]      = "PULSES_CAPTURED",
    [DB_DIAG_REPEAT_FRAME_DETECTED]= "REPEAT_FRAME_DETECTED",
    [DB_DIAG_PROTOCOL_DECODED]     = "PROTOCOL_DECODED",
    [DB_DIAG_UNKNOWN_PROTOCOL_RAW] = "UNKNOWN_PROTOCOL_RAW",
    [DB_DIAG_TX_OK]                = "TX_OK",
    [DB_DIAG_TX_FAILED]            = "TX_FAILED",
};

static const char *const s_help[DB_DIAG__COUNT] = {
    [DB_DIAG_CC1101_NOT_DETECTED]  = "No CC1101 answered on SPI. Check 3V3, GND, and the CSN/SCK/MOSI/MISO wiring.",
    [DB_DIAG_CC1101_OK]            = "The CC1101 responded with a valid part number and version.",
    [DB_DIAG_SPI_ERROR]            = "An SPI transaction failed at the driver level — a bus or configuration fault, not wiring.",
    [DB_DIAG_RADIO_CONFIG_SUSPECT] = "The radio is configured but the band looks implausible. Check frequency, modulation and antenna.",
    [DB_DIAG_RF_ENERGY_NO_PULSES]  = "Signal energy is present but no valid pulse stream emerged. Likely wrong frequency/bandwidth, or just background noise.",
    [DB_DIAG_PULSES_CAPTURED]      = "A raw pulse frame was captured and passed the noise filter.",
    [DB_DIAG_REPEAT_FRAME_DETECTED]= "The same frame arrived more than once — the signature of a genuine remote-control press.",
    [DB_DIAG_PROTOCOL_DECODED]     = "A decoder recognized the frame and extracted its payload.",
    [DB_DIAG_UNKNOWN_PROTOCOL_RAW] = "Pulses were captured but no decoder recognized them. The frame can still be replayed as-is.",
    [DB_DIAG_TX_OK]                = "The transmit completed in software. This does not confirm any receiver reacted.",
    [DB_DIAG_TX_FAILED]            = "The transmit path failed before or during keying.",
};

/* Which states deserve which log level — a not-detected radio should shout, a
 * captured frame should not. */
static esp_log_level_t level_for(db_diag_t s)
{
    switch (s) {
    case DB_DIAG_CC1101_NOT_DETECTED:
    case DB_DIAG_SPI_ERROR:
    case DB_DIAG_TX_FAILED:
        return ESP_LOG_ERROR;
    case DB_DIAG_RADIO_CONFIG_SUSPECT:
    case DB_DIAG_RF_ENERGY_NO_PULSES:
        return ESP_LOG_WARN;
    default:
        return ESP_LOG_INFO;
    }
}

static void ensure_lock(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
}

const char *db_diag_name(db_diag_t state)
{
    if (state < 0 || state >= DB_DIAG__COUNT || !s_names[state])
        return "UNKNOWN";
    return s_names[state];
}

const char *db_diag_help(db_diag_t state)
{
    if (state < 0 || state >= DB_DIAG__COUNT || !s_help[state])
        return "";
    return s_help[state];
}

const char *db_err_text(esp_err_t err)
{
    /* NVS has its own error space (ESP_ERR_NVS_BASE, 0x1100). Every code in it
     * means one thing to a user: the write did not stick. Spelling them out
     * individually would be a list of internal distinctions nobody can act on. */
    if (err >= 0x1100 && err <= 0x11FF)
        return "the device could not save it — its storage may be full or worn out";

    switch (err) {
    case ESP_OK:                   return "it worked";
    case ESP_ERR_NOT_FOUND:        return "it no longer exists";
    case ESP_ERR_INVALID_ARG:      return "the request was not valid";
    case ESP_ERR_INVALID_SIZE:     return "the data was the wrong size";
    case ESP_ERR_NO_MEM:           return "there is no room left for another one";
    case ESP_ERR_INVALID_STATE:    return "the device is busy or not ready for that right now";
    case ESP_ERR_TIMEOUT:          return "it took too long and was given up on";
    case ESP_ERR_NOT_SUPPORTED:    return "this device does not support that";
    case ESP_ERR_INVALID_RESPONSE: return "the other end replied with something unusable";
    case ESP_ERR_NOT_FINISHED:     return "it did not run to completion";
    default:                       return "the device could not complete it";
    }
}

void db_diag_report(db_diag_t state, const char *fmt, ...)
{
    if (state < 0 || state >= DB_DIAG__COUNT)
        return;

    char detail[sizeof(((db_diag_entry_t *)0)->detail)];
    detail[0] = '\0';
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(detail, sizeof(detail), fmt, ap);
        va_end(ap);
    }

    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_entries[state].count++;
    s_entries[state].last_us = esp_timer_get_time();
    memcpy(s_entries[state].detail, detail, sizeof(detail));
    xSemaphoreGive(s_lock);

    ESP_LOG_LEVEL(level_for(state), TAG, "[%s]%s%s",
                  db_diag_name(state), detail[0] ? " " : "", detail);
}

void db_diag_get(db_diag_t state, db_diag_entry_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (state < 0 || state >= DB_DIAG__COUNT)
        return;
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_entries[state];
    xSemaphoreGive(s_lock);
}

void db_diag_reset(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_entries, 0, sizeof(s_entries));
    xSemaphoreGive(s_lock);
}
