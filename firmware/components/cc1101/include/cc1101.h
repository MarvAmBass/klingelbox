/*
 * cc1101.h - Portable TI CC1101 sub-GHz transceiver driver (ESP-IDF).
 *
 * SCOPE / NON-SCOPE. This component drives the *chip*: SPI transport, register
 * and strobe access, identification, frequency/modulation/bandwidth/power
 * configuration, and mode switching. It knows nothing about pulses, frames,
 * doorbells or EV1527 — that all lives one layer up in `rfpulse`. Keeping the
 * split clean is what makes both components reusable outside this project.
 *
 * THE CENTRAL DESIGN DECISION: ASYNCHRONOUS SERIAL MODE, NOT PACKET MODE.
 * Cheap 433 MHz remotes (doorbells, gate fobs, weather stations) do not transmit
 * CC1101 packets. They emit a bare OOK waveform with no preamble, sync word,
 * length byte or CRC. Configuring the CC1101 for packet RX therefore discards
 * everything of interest. Instead this driver puts the chip in *asynchronous
 * serial* mode (PKTCTRL0.PKT_FORMAT = 3), where the demodulator's raw output is
 * exposed continuously on a GDO pin and the host times the edges itself.
 *
 * GDO0 IS BIDIRECTIONAL BY MODE — the subtlety worth stating loudly:
 *   - RX async: GDO0 is a data OUTPUT. IOCFG0 = 0x0D (serial data output,
 *     asynchronous). The host captures edges on it.
 *   - TX async: GDO0 is a data INPUT. The chip samples whatever the host drives
 *     onto the pin and keys the carrier accordingly. IOCFG0 must be set to 0x2E
 *     (3-state) so the CC1101 stops driving the line and the host can.
 * Getting this backwards is the classic cause of "transmit does nothing" — the
 * two chips fight over the pin. The mode setter below owns this transition.
 *
 * AGC IN OOK MODE. With no carrier present, the OOK demodulator's AGC winds gain
 * all the way up and the data line becomes continuous noise. That is normal and
 * expected, not a fault: the pulse layer above is responsible for rejecting it
 * (glitch filter + minimum frame length). Squelch behaviour is tuned by
 * AGCCTRL2's MAX_DVGA_GAIN and the magnitude target — see cc1101.c.
 */
#ifndef CC1101_H
#define CC1101_H

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- board wiring (supplied by the app from board_pins.h) ---------------- */
typedef struct {
    spi_host_device_t host;
    gpio_num_t sck;
    gpio_num_t mosi;
    gpio_num_t miso;
    gpio_num_t cs;
    gpio_num_t gdo0;      /* async data line (out in RX, in in TX) */
    gpio_num_t gdo2;      /* second GDO: wired, configurable, optional */
    int        clock_hz;  /* SPI clock; the CC1101 tolerates ~6.5 MHz burst */
} cc1101_pins_t;

/* ---- radio parameters (configuration, never constants in the code) ------- */
typedef enum {
    CC1101_MOD_ASK_OOK = 0,
    CC1101_MOD_2FSK,
    CC1101_MOD_GFSK,
    CC1101_MOD_4FSK,
    CC1101_MOD_MSK,
} cc1101_modulation_t;

typedef struct {
    uint32_t            freq_hz;          /* e.g. 433920000 */
    cc1101_modulation_t modulation;       /* OOK for these doorbells */
    /* Even in asynchronous mode the data rate still sets the demodulator's
     * filtering, so it materially affects how clean the captured edges are.
     * ~5 kBaud suits the ~250-500 us symbols typical of EV1527-class remotes. */
    uint32_t            datarate_bps;
    uint32_t            rx_bandwidth_hz;  /* channel filter, e.g. 203000 */
    int8_t              tx_power_dbm;     /* mapped to the nearest PATABLE entry */
} cc1101_radio_cfg_t;

/* Sensible starting point for a 433.92 MHz OOK doorbell. Supplied as a helper so
 * the values live in one place, not scattered as literals. */
void cc1101_radio_cfg_default(cc1101_radio_cfg_t *out);

/* ---- identification ------------------------------------------------------ */
/*
 * A genuine CC1101 reports PARTNUM 0x00 and a VERSION in a small known set
 * (0x04, 0x14 and 0x17 are all seen in the wild; E07-M1101D modules typically
 * read 0x14). The decisive evidence of a *dead bus* is a version of 0x00 or
 * 0xFF: those are what a floating MISO or a chip held in reset returns, so they
 * are treated as "not detected" rather than "unknown revision".
 */
typedef struct {
    uint8_t partnum;
    uint8_t version;
    bool    present;          /* passed the plausibility test above */
    bool    version_known;    /* version is one of the documented revisions */
} cc1101_ident_t;

/* ---- operating mode ------------------------------------------------------ */
typedef enum {
    CC1101_MODE_IDLE = 0,   /* SIDLE; radio quiet, registers accessible */
    CC1101_MODE_RX_ASYNC,   /* async OOK receive; GDO0 = demodulated data out */
    CC1101_MODE_TX_ASYNC,   /* async OOK transmit; GDO0 = data in (host-driven) */
} cc1101_mode_t;

typedef struct cc1101_dev_s *cc1101_handle_t;

/* Bring up the SPI bus + device and hard-reset the chip. Does NOT configure the
 * radio (call cc1101_configure) and does NOT verify the part is there — probe
 * separately so the caller can report the two failures distinctly. */
esp_err_t cc1101_init(const cc1101_pins_t *pins, cc1101_handle_t *out);

/* Read PARTNUM/VERSION and fill *ident. Returns ESP_OK when the SPI transaction
 * itself succeeded — inspect ident->present for whether a chip actually answered.
 * This split matters for diagnostics: a failed transaction is a bus/driver
 * problem, while present=false is a wiring/power problem. */
esp_err_t cc1101_probe(cc1101_handle_t dev, cc1101_ident_t *ident);

/* Apply a full radio configuration (frequency, modulation, rates, bandwidth,
 * power) plus the async-serial packet configuration. Safe to call repeatedly;
 * forces IDLE internally and restores the previous mode. */
esp_err_t cc1101_configure(cc1101_handle_t dev, const cc1101_radio_cfg_t *cfg);

/* Switch operating mode, including the GDO0 direction/IOCFG handover described
 * in the file header. */
esp_err_t cc1101_set_mode(cc1101_handle_t dev, cc1101_mode_t mode);
cc1101_mode_t cc1101_get_mode(cc1101_handle_t dev);

/* Current received signal strength, converted from the raw register to dBm per
 * the datasheet's offset formula. Only meaningful while in RX. */
esp_err_t cc1101_rssi_dbm(cc1101_handle_t dev, int *dbm_out);

/* Carrier-sense / sync status from PKTSTATUS. Used to distinguish "RF energy is
 * present but nothing decodes" from "the band is silent" — a required
 * diagnostic state for this project. */
esp_err_t cc1101_carrier_sense(cc1101_handle_t dev, bool *asserted);

/* Raw register access, exposed for diagnostics and for register dumps in the
 * web UI. Prefer the typed helpers above for normal use. */
esp_err_t cc1101_read_reg(cc1101_handle_t dev, uint8_t addr, uint8_t *val);
esp_err_t cc1101_write_reg(cc1101_handle_t dev, uint8_t addr, uint8_t val);
esp_err_t cc1101_strobe(cc1101_handle_t dev, uint8_t strobe, uint8_t *status_out);

/* Configure the second GDO pin's IOCFG2 function (e.g. 0x0E carrier sense).
 * Kept in the API because GDO2 stays wired even when unused. */
esp_err_t cc1101_set_gdo2_function(cc1101_handle_t dev, uint8_t iocfg2);

/* Release SPI + GPIO resources. */
void cc1101_deinit(cc1101_handle_t dev);

#ifdef __cplusplus
}
#endif

#endif /* CC1101_H */
