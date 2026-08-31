/*
 * db_diag.h - The firmware's shared diagnostic vocabulary.
 *
 * A hard requirement of this project is that failures are DISTINGUISHABLE. "It
 * doesn't work" has at least five very different causes on an RF bring-up — a
 * dead SPI bus, a mis-tuned radio, a noisy band, an unrecognized protocol, or a
 * transmit that never keyed the carrier — and each one demands a different fix.
 * So instead of scattering ad-hoc log lines, every layer reports into this one
 * enum, and the same values surface identically in the serial log, in
 * GET /api/diagnostics, and in the web UI.
 *
 * Each state records its last occurrence time, an occurrence count, and a short
 * free-text detail, which is what makes the API view genuinely useful rather
 * than a boolean health light.
 *
 * A note on honesty: DB_DIAG_TX_OK asserts only that the software-level
 * transmit path completed — the pulses left the peripheral with the radio in TX
 * mode. It deliberately does NOT claim any receiver reacted, because the box
 * has no way to know that.
 */
#ifndef DB_DIAG_H
#define DB_DIAG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* ---- radio presence ---- */
    DB_DIAG_CC1101_NOT_DETECTED = 0, /* SPI answered but no plausible chip ID   */
    DB_DIAG_CC1101_OK,               /* PARTNUM/VERSION read back sane          */
    DB_DIAG_SPI_ERROR,               /* the transaction itself failed           */

    /* ---- reception ---- */
    DB_DIAG_RADIO_CONFIG_SUSPECT,    /* configured, but the band looks wrong    */
    DB_DIAG_RF_ENERGY_NO_PULSES,     /* carrier/RSSI up, nothing survives filter*/
    DB_DIAG_PULSES_CAPTURED,         /* a frame passed the sanity check         */
    DB_DIAG_REPEAT_FRAME_DETECTED,   /* >=2 near-identical frames in the window */

    /* ---- interpretation ---- */
    DB_DIAG_PROTOCOL_DECODED,        /* a decoder plugin claimed the frame      */
    DB_DIAG_UNKNOWN_PROTOCOL_RAW,    /* captured, undecoded — still replayable  */

    /* ---- transmission ---- */
    DB_DIAG_TX_OK,                   /* software-level success only (see above) */
    DB_DIAG_TX_FAILED,

    DB_DIAG__COUNT
} db_diag_t;

/* Stable machine-readable name, e.g. "PULSES_CAPTURED". Used verbatim in the
 * REST API so the UI and logs cannot drift apart. */
const char *db_diag_name(db_diag_t state);

/* One-line human explanation of what the state means — shown in the UI so a
 * user who is not holding the datasheet can still act on it. */
const char *db_diag_help(db_diag_t state);

/* Record an occurrence (and log it at an appropriate level). `fmt` may be NULL. */
void db_diag_report(db_diag_t state, const char *fmt, ...);

/*
 * A HUMAN clause for an esp_err_t, for anything that ends up in front of a user.
 *
 * It lives here, next to db_diag_help(), because it answers the same question:
 * what does this failure mean to somebody who is not holding the datasheet.
 * "could not create the virtual signal: ESP_ERR_INVALID_STATE" is a real thing
 * this firmware once said to a real user, and it is worse than saying nothing —
 * it reads as a crash, it is unsearchable outside the ESP-IDF sources, and it
 * gives no hint about what to do next. An esp_err_t is a contract between two C
 * files; it is never UI. The raw name still belongs in the LOG, where it is
 * exactly the right thing.
 *
 * The result is a lower-case clause with no trailing stop, so it can be pasted
 * after a caller's own phrasing: "could not delete the signal — it no longer
 * exists". A caller that knows something more specific than the code must say
 * THAT instead; this is the floor, not the target.
 */
const char *db_err_text(esp_err_t err);

/* ---- readback, for GET /api/diagnostics ---- */
typedef struct {
    uint32_t count;         /* times reported since boot */
    int64_t  last_us;       /* esp_timer_get_time() of the last report, 0 = never */
    char     detail[96];    /* last formatted detail, "" if none */
} db_diag_entry_t;

void db_diag_get(db_diag_t state, db_diag_entry_t *out);
void db_diag_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_DIAG_H */
