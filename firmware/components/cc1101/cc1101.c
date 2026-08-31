/*
 * cc1101.c - TI CC1101 sub-GHz transceiver driver for ESP-IDF (esp32s3).
 *
 * See cc1101.h for the scope statement and the two decisions that shape this
 * file: asynchronous serial mode instead of packet mode, and the GDO0 direction
 * handover between RX and TX. What follows are the *implementation* decisions
 * that are not obvious from the header.
 *
 * WHY CS IS DRIVEN BY HAND (spics_io_num = -1).
 * The CC1101 is not a plain SPI slave. Its SPI section (datasheet §10.1) says
 * that after CSn goes low the host must wait for SO/MISO to go LOW before
 * clocking the header byte: that transition is the chip signalling "crystal is
 * running, digital core is awake". The ESP32 SPI master asserts and de-asserts
 * its hardware CS strictly around the clocked bytes, which leaves no window in
 * which to observe that handshake. So the driver owns the CS GPIO itself,
 * asserts it, polls MISO low with a bounded timeout, and only then hands the
 * bytes to the SPI master. Skipping the handshake mostly "works" once the chip
 * is warm and fails exactly at power-on and after SRES/SPWD — the sort of
 * intermittent bring-up failure that is very expensive to chase later.
 *
 * Reading MISO with gpio_get_level() while the SPI peripheral owns the pad is
 * legal: the pad input is routed to the GPIO input register regardless of which
 * IO-MUX function drives the pin, and the SPI driver leaves the input enabled.
 *
 * WHY DMA IS DISABLED ON THE BUS.
 * The largest transaction here is a 9-byte PATABLE burst. DMA would buy nothing
 * and would impose DMA-capable/word-aligned buffer rules on every caller for no
 * benefit, so the bus is opened with SPI_DMA_DISABLED and transfers go through
 * spi_device_polling_transmit() — lowest latency, no interrupt per register
 * access, which matters because register pokes happen inside mode switches.
 *
 * WHY STATUS REGISTERS ARE ALWAYS READ AS BURSTS.
 * The header byte is {R/W, BURST, addr[5:0]}. Addresses 0x30-0x3D are shared
 * between the *command strobes* (write, and read-single) and the *status
 * registers* (read-burst). A "read single" of 0x30 does not return PARTNUM, it
 * fires SRES and resets the chip. Every status read below therefore sets the
 * burst bit; cc1101_read_reg() applies the same rule automatically so the
 * diagnostics/register-dump path cannot accidentally reset the radio.
 *
 * WHY THE REGISTER VALUES ARE COMPUTED, NOT TABULATED.
 * PLAN.md §3.5 requires frequency, modulation, data rate and bandwidth to be
 * configuration rather than constants. cc1101_configure() therefore derives
 * FREQ2/1/0, MDMCFG4/3/2 and the PATABLE entry from cc1101_radio_cfg_t using
 * the datasheet formulas. Only the registers that genuinely have no free
 * parameter (calibration, test and front-end values from SmartRF Studio) are
 * literals, and each one carries a comment saying what it is for.
 *
 * PIN OWNERSHIP. This driver deliberately never configures GDO0 on the ESP32
 * side. In RX the CC1101 drives that line and the host must stay off it; in TX
 * the host drives it. Ownership of the ESP32 end belongs to components/rfpulse
 * (RMT RX / RMT TX), which claims and releases it around mode changes. All this
 * driver does is tell the *CC1101* whether to drive the pin or three-state it.
 *
 * THREADING. There is no internal lock. The intended owner (main/rf_service)
 * already serializes radio access under its own mutex — the same mutex that
 * serializes RMT RX/TX pin ownership — and adding a second lock here would only
 * create a second place for the ordering to be wrong.
 */

#include <stdlib.h>
#include <string.h>

#include "cc1101.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cc1101";

/* ---- SPI header byte bits ------------------------------------------------ */
#define CC1101_READ         0x80u   /* bit7: 1 = read, 0 = write               */
#define CC1101_BURST        0x40u   /* bit6: 1 = burst (auto-increment)        */

/* ---- configuration registers (0x00-0x2E) --------------------------------- */
#define CC1101_IOCFG2       0x00u   /* GDO2 output pin configuration           */
#define CC1101_IOCFG1       0x01u   /* GDO1 = the MISO pad; leave at reset     */
#define CC1101_IOCFG0       0x02u   /* GDO0 output pin configuration           */
#define CC1101_FIFOTHR      0x03u   /* FIFO thresholds + ADC retention bit     */
#define CC1101_SYNC1        0x04u   /* sync word hi (unused: SYNC_MODE = 0)    */
#define CC1101_SYNC0        0x05u   /* sync word lo (unused)                   */
#define CC1101_PKTLEN       0x06u   /* packet length (unused: infinite mode)   */
#define CC1101_PKTCTRL1     0x07u   /* address check / status append           */
#define CC1101_PKTCTRL0     0x08u   /* packet format, CRC, length config       */
#define CC1101_ADDR         0x09u   /* device address (unused)                 */
#define CC1101_CHANNR       0x0Au   /* channel number                          */
#define CC1101_FSCTRL1      0x0Bu   /* IF frequency                            */
#define CC1101_FSCTRL0      0x0Cu   /* frequency offset                        */
#define CC1101_FREQ2        0x0Du   /* carrier frequency [23:16]               */
#define CC1101_FREQ1        0x0Eu   /* carrier frequency [15:8]                */
#define CC1101_FREQ0        0x0Fu   /* carrier frequency [7:0]                 */
#define CC1101_MDMCFG4      0x10u   /* channel bandwidth + data rate exponent  */
#define CC1101_MDMCFG3      0x11u   /* data rate mantissa                      */
#define CC1101_MDMCFG2      0x12u   /* modulation format, Manchester, sync     */
#define CC1101_MDMCFG1      0x13u   /* FEC, preamble count, chan spacing exp   */
#define CC1101_MDMCFG0      0x14u   /* channel spacing mantissa                */
#define CC1101_DEVIATN      0x15u   /* FSK deviation (ignored for OOK)         */
#define CC1101_MCSM2        0x16u   /* RX timeout / main state machine         */
#define CC1101_MCSM1        0x17u   /* CCA mode, RXOFF/TXOFF next state        */
#define CC1101_MCSM0        0x18u   /* auto-calibration, PO timeout            */
#define CC1101_FOCCFG       0x19u   /* frequency offset compensation           */
#define CC1101_BSCFG        0x1Au   /* bit synchronization                     */
#define CC1101_AGCCTRL2     0x1Bu   /* max gain limits + magnitude target      */
#define CC1101_AGCCTRL1     0x1Cu   /* carrier-sense thresholds                */
#define CC1101_AGCCTRL0     0x1Du   /* hysteresis, wait time, OOK boundary     */
#define CC1101_WOREVT1      0x1Eu   /* wake-on-radio event0 hi (unused)        */
#define CC1101_WOREVT0      0x1Fu   /* wake-on-radio event0 lo (unused)        */
#define CC1101_WORCTRL      0x20u   /* wake-on-radio control                   */
#define CC1101_FREND1       0x21u   /* RX front-end current configuration      */
#define CC1101_FREND0       0x22u   /* TX front-end: selects the PATABLE index */
#define CC1101_FSCAL3       0x23u   /* frequency synthesizer calibration       */
#define CC1101_FSCAL2       0x24u
#define CC1101_FSCAL1       0x25u
#define CC1101_FSCAL0       0x26u
#define CC1101_TEST2        0x2Cu   /* various test settings (from SmartRF)    */
#define CC1101_TEST1        0x2Du
#define CC1101_TEST0        0x2Eu

/* ---- command strobes (write / read-single to 0x30-0x3D) ------------------ */
#define CC1101_SRES         0x30u   /* reset chip                              */
#define CC1101_SCAL         0x33u   /* calibrate synthesizer, then go IDLE     */
#define CC1101_SRX          0x34u   /* enable RX                               */
#define CC1101_STX          0x35u   /* enable TX                               */
#define CC1101_SIDLE        0x36u   /* leave RX/TX, disable frequency synth    */
#define CC1101_SFRX         0x3Au   /* flush the RX FIFO (only in IDLE/RXFIFO) */
#define CC1101_SFTX         0x3Bu   /* flush the TX FIFO (only in IDLE/TXFIFO) */
#define CC1101_SNOP         0x3Du   /* no operation: cheap way to fetch status */

/* ---- status registers (read-BURST only from 0x30-0x3D) ------------------- */
#define CC1101_PARTNUM      0x30u   /* part number, 0x00 on every CC1101       */
#define CC1101_VERSION      0x31u   /* silicon revision                        */
#define CC1101_RSSI         0x34u   /* received signal strength, 2's complement*/
#define CC1101_MARCSTATE    0x35u   /* main radio control state machine state  */
#define CC1101_PKTSTATUS    0x38u   /* GDO/CS/CCA/SFD status bits              */

/* ---- multi-byte access --------------------------------------------------- */
#define CC1101_PATABLE      0x3Eu   /* 8-entry PA power table                  */

/* ---- IOCFGx functions used here ------------------------------------------ */
#define CC1101_GDOx_SERIAL_DATA_OUT 0x0Du /* async serial data output (RX)     */
#define CC1101_GDOx_HIGH_Z          0x2Eu /* 3-state: chip releases the pin    */

/* ---- MARCSTATE values ---------------------------------------------------- */
#define CC1101_MARC_IDLE    0x01u

/* Crystal on the E07-M1101D (and on essentially every CC1101 module). Every
 * frequency/rate/bandwidth formula below is relative to it. */
#define CC1101_F_XOSC       26000000ull

/* How long the chip-ready handshake (MISO low after CSn low) may take. The
 * datasheet's worst case is the crystal start-up after power-on/SPWD, which is
 * hundreds of microseconds; 10 ms is generous enough that a timeout means real
 * trouble (no power, no crystal, wrong pin) rather than a slow start. */
#define CC1101_READY_TIMEOUT_US 10000

/* Bound on how long SIDLE may take to actually reach the IDLE state. */
#define CC1101_STATE_TIMEOUT_US 5000

struct cc1101_dev_s {
    cc1101_pins_t       pins;
    spi_device_handle_t spi;
    cc1101_mode_t       mode;
    bool                bus_initialized;
};

/* ========================================================================== */
/* SPI transport                                                              */
/* ========================================================================== */

/*
 * Assert CSn and wait out the chip-ready handshake (see the file header). On
 * timeout CSn is released again so a wedged chip cannot hold the bus.
 */
static esp_err_t cc1101_cs_assert(cc1101_handle_t dev)
{
    gpio_set_level(dev->pins.cs, 0);

    const int64_t deadline = esp_timer_get_time() + CC1101_READY_TIMEOUT_US;
    while (gpio_get_level(dev->pins.miso) != 0) {
        if (esp_timer_get_time() > deadline) {
            gpio_set_level(dev->pins.cs, 1);
            ESP_LOGE(TAG, "chip-ready timeout: MISO (GPIO%d) stayed high after CS low",
                     (int)dev->pins.miso);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(2);
    }
    return ESP_OK;
}

static inline void cc1101_cs_release(cc1101_handle_t dev)
{
    gpio_set_level(dev->pins.cs, 1);
}

/* One full-duplex transfer. Polling transmit: the payloads here are 2-9 bytes,
 * so an interrupt-driven transaction would cost far more than it saves. */
static esp_err_t cc1101_xfer(cc1101_handle_t dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(dev->spi, &t);
}

/* A whole framed access: CS low + handshake, transfer, CS high. */
static esp_err_t cc1101_access(cc1101_handle_t dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
    esp_err_t err = cc1101_cs_assert(dev);
    if (err != ESP_OK) {
        return err;
    }
    err = cc1101_xfer(dev, tx, rx, len);
    cc1101_cs_release(dev);
    return err;
}

esp_err_t cc1101_write_reg(cc1101_handle_t dev, uint8_t addr, uint8_t val)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t tx[2] = { (uint8_t)(addr & 0x3Fu), val };
    uint8_t rx[2];
    return cc1101_access(dev, tx, rx, sizeof(tx));
}

/*
 * Read one register. Addresses 0x30-0x3D are the status registers and MUST be
 * read with the burst bit set, otherwise the access is decoded as a command
 * strobe (a "read single" of 0x30 would fire SRES). Applying that rule here
 * rather than at every call site means the raw diagnostics path in the web UI
 * is safe by construction.
 */
esp_err_t cc1101_read_reg(cc1101_handle_t dev, uint8_t addr, uint8_t *val)
{
    if (dev == NULL || val == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool is_status = (addr >= 0x30u && addr <= 0x3Du);
    const uint8_t tx[2] = {
        (uint8_t)((addr & 0x3Fu) | CC1101_READ | (is_status ? CC1101_BURST : 0u)),
        0x00u,
    };
    uint8_t rx[2] = { 0, 0 };

    esp_err_t err = cc1101_access(dev, tx, rx, sizeof(tx));
    if (err == ESP_OK) {
        *val = rx[1];
    }
    return err;
}

/* Burst write, used for the PATABLE. */
static esp_err_t cc1101_write_burst(cc1101_handle_t dev, uint8_t addr,
                                    const uint8_t *data, size_t len)
{
    uint8_t tx[1 + 8];
    uint8_t rx[1 + 8];

    if (len > sizeof(tx) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    tx[0] = (uint8_t)((addr & 0x3Fu) | CC1101_BURST);
    memcpy(&tx[1], data, len);

    return cc1101_access(dev, tx, rx, len + 1);
}

esp_err_t cc1101_strobe(cc1101_handle_t dev, uint8_t strobe, uint8_t *status_out)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t tx[1] = { (uint8_t)(strobe & 0x3Fu) };
    uint8_t rx[1] = { 0 };

    esp_err_t err = cc1101_access(dev, tx, rx, sizeof(tx));
    if (err == ESP_OK && status_out != NULL) {
        *status_out = rx[0];
    }
    return err;
}

/* Poll MARCSTATE until the radio reaches `want`. Mode changes are not
 * instantaneous (SIDLE has to shut the synthesizer down), and writing IOCFG0
 * while the chip is still leaving RX is one way to lose the handover. */
static esp_err_t cc1101_wait_state(cc1101_handle_t dev, uint8_t want)
{
    const int64_t deadline = esp_timer_get_time() + CC1101_STATE_TIMEOUT_US;
    for (;;) {
        uint8_t marc = 0;
        esp_err_t err = cc1101_read_reg(dev, CC1101_MARCSTATE, &marc);
        if (err != ESP_OK) {
            return err;
        }
        if ((marc & 0x1Fu) == want) {
            return ESP_OK;
        }
        if (esp_timer_get_time() > deadline) {
            ESP_LOGW(TAG, "state timeout: MARCSTATE=0x%02X, wanted 0x%02X", marc & 0x1Fu, want);
            return ESP_ERR_TIMEOUT;
        }
        esp_rom_delay_us(50);
    }
}

/* ========================================================================== */
/* Reset                                                                      */
/* ========================================================================== */

/*
 * Manual power-on reset, datasheet §19.1. The CSn wiggle before SRES exists to
 * put the chip's SPI state machine into a known state even if it powered up
 * mid-transaction (for example after a warm ESP32 reset that left CSn low), and
 * the 40 us low period lets the internal regulator settle. Then SRES, then the
 * chip-ready handshake once more because SRES restarts the digital core.
 */
static esp_err_t cc1101_hard_reset(cc1101_handle_t dev)
{
    gpio_set_level(dev->pins.cs, 1);
    esp_rom_delay_us(5);
    gpio_set_level(dev->pins.cs, 0);
    esp_rom_delay_us(10);
    gpio_set_level(dev->pins.cs, 1);
    esp_rom_delay_us(45);

    esp_err_t err = cc1101_strobe(dev, CC1101_SRES, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SRES failed: %s", esp_err_to_name(err));
        return err;
    }

    /* SRES takes the chip through a full restart; give it a moment and then
     * confirm it is answering again via the same MISO-low handshake. */
    vTaskDelay(pdMS_TO_TICKS(2));

    err = cc1101_cs_assert(dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "chip did not become ready after SRES");
        return err;
    }
    cc1101_cs_release(dev);
    return ESP_OK;
}

/* ========================================================================== */
/* Register maths (datasheet formulas, not a magic table)                     */
/* ========================================================================== */

/*
 * f_carrier = (F_XOSC / 2^16) * FREQ.  FREQ is 22 bits (FREQ2 holds only
 * [21:16]), so 433.92 MHz becomes 0x10B071.
 */
static void cc1101_calc_freq(uint32_t freq_hz, uint8_t out[3])
{
    uint32_t word = (uint32_t)(((uint64_t)freq_hz << 16) / CC1101_F_XOSC);
    if (word > 0x3FFFFFu) {
        word = 0x3FFFFFu;
    }
    out[0] = (uint8_t)((word >> 16) & 0x3Fu);
    out[1] = (uint8_t)((word >> 8) & 0xFFu);
    out[2] = (uint8_t)(word & 0xFFu);
}

/*
 * R_DATA = ((256 + DRATE_M) / 2^28) * 2^DRATE_E * F_XOSC.
 * Pick the largest exponent for which the mantissa still lands in [0,255], then
 * round the mantissa. 5 kBaud resolves to E=7, M=147 (0x93) => 4996 Baud.
 */
static void cc1101_calc_drate(uint32_t bps, uint8_t *drate_e, uint8_t *drate_m)
{
    uint8_t e = 0;
    while (e < 15u && ((uint64_t)CC1101_F_XOSC << (e + 1)) <= ((uint64_t)bps << 20)) {
        e++;
    }

    const uint64_t denom = (uint64_t)CC1101_F_XOSC << e;
    uint64_t m = (((uint64_t)bps << 28) + denom / 2u) / denom;   /* = 256 + M */

    if (m < 256u) {
        m = 256u;                       /* below the lowest representable rate */
    } else if (m >= 512u) {
        m = 256u;                       /* rounded up into the next decade     */
        if (e < 15u) {
            e++;
        }
    }
    *drate_e = e;
    *drate_m = (uint8_t)(m - 256u);
}

/*
 * BW_channel = F_XOSC / (8 * (4 + CHANBW_M) * 2^CHANBW_E).
 * Only 16 combinations exist, so search them all and keep the closest — clearer
 * and less error-prone than inverting the formula with integer maths.
 * 203 kHz resolves to E=2, M=0 => 203125 Hz.
 */
static void cc1101_calc_chanbw(uint32_t bw_hz, uint8_t *bw_e, uint8_t *bw_m)
{
    uint32_t best_err = UINT32_MAX;
    *bw_e = 0;
    *bw_m = 0;

    for (uint8_t e = 0; e < 4u; e++) {
        for (uint8_t m = 0; m < 4u; m++) {
            const uint32_t bw = (uint32_t)(CC1101_F_XOSC / (8ull * (4u + m) * (1ull << e)));
            const uint32_t err = (bw > bw_hz) ? (bw - bw_hz) : (bw_hz - bw);
            if (err < best_err) {
                best_err = err;
                *bw_e = e;
                *bw_m = m;
            }
        }
    }
}

/* MDMCFG2.MOD_FORMAT encoding. */
static uint8_t cc1101_mod_format(cc1101_modulation_t mod)
{
    switch (mod) {
    case CC1101_MOD_2FSK:    return 0u;
    case CC1101_MOD_GFSK:    return 1u;
    case CC1101_MOD_ASK_OOK: return 3u;
    case CC1101_MOD_4FSK:    return 4u;
    case CC1101_MOD_MSK:     return 7u;
    default:                 return 3u;
    }
}

/*
 * PA settings for the 433 MHz band, datasheet Table 39 / DN013. These are the
 * only *documented* values for this band; anything in between is interpolation
 * the datasheet does not endorse, so the requested dBm is snapped to the nearest
 * documented entry rather than invented.
 */
typedef struct {
    int8_t  dbm;
    uint8_t pa;
} cc1101_pa_entry_t;

static const cc1101_pa_entry_t k_pa_433[] = {
    { -30, 0x12 }, { -20, 0x0E }, { -15, 0x1D }, { -10, 0x34 },
    {  -6, 0x2C }, {   0, 0x60 }, {   5, 0x84 }, {   7, 0xC8 },
    {  10, 0xC0 },
};

static uint8_t cc1101_pa_value(int8_t dbm, int8_t *actual_out)
{
    size_t best = 0;
    int best_err = 127;

    for (size_t i = 0; i < sizeof(k_pa_433) / sizeof(k_pa_433[0]); i++) {
        int err = (int)k_pa_433[i].dbm - (int)dbm;
        if (err < 0) {
            err = -err;
        }
        if (err < best_err) {
            best_err = err;
            best = i;
        }
    }
    if (actual_out != NULL) {
        *actual_out = k_pa_433[best].dbm;
    }
    return k_pa_433[best].pa;
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

void cc1101_radio_cfg_default(cc1101_radio_cfg_t *out)
{
    if (out == NULL) {
        return;
    }
    out->freq_hz         = 433920000u;  /* the European 433 MHz ISM centre used
                                         * by essentially every cheap doorbell */
    out->modulation      = CC1101_MOD_ASK_OOK;
    out->datarate_bps    = 5000u;       /* ~200 us symbols: matches EV1527-class
                                         * remotes without over-filtering edges */
    out->rx_bandwidth_hz = 203000u;     /* wide enough to swallow the several-
                                         * tens-of-kHz drift of a SAW resonator */
    out->tx_power_dbm    = 10;          /* module maximum for the E07-M1101D    */
}

esp_err_t cc1101_init(const cc1101_pins_t *pins, cc1101_handle_t *out)
{
    if (pins == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    cc1101_handle_t dev = calloc(1, sizeof(struct cc1101_dev_s));
    if (dev == NULL) {
        return ESP_ERR_NO_MEM;
    }
    dev->pins = *pins;
    dev->mode = CC1101_MODE_IDLE;

    /* CSn is a plain GPIO owned by us, not by the SPI driver. Park it high
     * before the bus comes up so the chip never sees a spurious selection. */
    const gpio_config_t cs_cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)dev->pins.cs,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cs_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CS GPIO%d config failed: %s", (int)dev->pins.cs, esp_err_to_name(err));
        free(dev);
        return err;
    }
    gpio_set_level(dev->pins.cs, 1);

    /* DMA disabled on purpose: see the file header. max_transfer_sz is the
     * CPU-driven limit and 64 bytes is far more than the 9-byte PATABLE burst. */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = dev->pins.mosi,
        .miso_io_num     = dev->pins.miso,
        .sclk_io_num     = dev->pins.sck,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 64,
    };
    err = spi_bus_initialize(dev->pins.host, &bus_cfg, SPI_DMA_DISABLED);
    if (err == ESP_OK) {
        dev->bus_initialized = true;
    } else if (err == ESP_ERR_INVALID_STATE) {
        /* Somebody else already opened this host; share it rather than fail,
         * but remember not to tear it down in deinit. */
        ESP_LOGW(TAG, "SPI host %d already initialized; attaching to it", (int)dev->pins.host);
    } else {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        free(dev);
        return err;
    }

    /* SPI mode 0 (CPOL=0, CPHA=0) per datasheet §10.1. spics_io_num = -1 hands
     * chip select to us so the MISO-low handshake is observable. */
    const spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = (dev->pins.clock_hz > 0) ? dev->pins.clock_hz : 4000000,
        .mode           = 0,
        .spics_io_num   = -1,
        .queue_size     = 1,
        .command_bits   = 0,
        .address_bits   = 0,
        .dummy_bits     = 0,
    };
    err = spi_bus_add_device(dev->pins.host, &dev_cfg, &dev->spi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        if (dev->bus_initialized) {
            spi_bus_free(dev->pins.host);
        }
        free(dev);
        return err;
    }

    err = cc1101_hard_reset(dev);
    if (err != ESP_OK) {
        cc1101_deinit(dev);
        return err;
    }

    /* GDO2 stays wired (board_pins.h) but must not drive anything until the
     * application asks for a function, so park it three-stated. GDO0 is left at
     * its reset value here; cc1101_set_mode() owns it from now on. Note that
     * GDO1 shares the MISO pad and is deliberately left at its reset value
     * (0x2E, three-state) so it never fights the SPI peripheral. */
    err = cc1101_write_reg(dev, CC1101_IOCFG2, CC1101_GDOx_HIGH_Z);
    if (err != ESP_OK) {
        cc1101_deinit(dev);
        return err;
    }

    ESP_LOGI(TAG, "initialized: host=%d sck=%d mosi=%d miso=%d cs=%d gdo0=%d gdo2=%d @ %d Hz",
             (int)dev->pins.host, (int)dev->pins.sck, (int)dev->pins.mosi,
             (int)dev->pins.miso, (int)dev->pins.cs, (int)dev->pins.gdo0,
             (int)dev->pins.gdo2, dev_cfg.clock_speed_hz);

    *out = dev;
    return ESP_OK;
}

esp_err_t cc1101_probe(cc1101_handle_t dev, cc1101_ident_t *ident)
{
    if (dev == NULL || ident == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(ident, 0, sizeof(*ident));

    /* Both of these live in the strobe-shared address range, so both go out as
     * burst reads (cc1101_read_reg enforces that). */
    esp_err_t err = cc1101_read_reg(dev, CC1101_PARTNUM, &ident->partnum);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe: PARTNUM read failed: %s", esp_err_to_name(err));
        return err;   /* transaction failure: a bus/driver problem, not wiring */
    }
    err = cc1101_read_reg(dev, CC1101_VERSION, &ident->version);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "probe: VERSION read failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 0x00 and 0xFF are what a floating MISO or a chip held in reset reads back,
     * so they are evidence of absence rather than of an unknown revision. */
    const bool dead_bus = (ident->version == 0x00u || ident->version == 0xFFu);

    ident->version_known = (ident->partnum == 0x00u) &&
                           (ident->version == 0x04u || ident->version == 0x14u ||
                            ident->version == 0x17u);
    ident->present = !dead_bus && (ident->partnum == 0x00u);

    if (ident->version_known) {
        ESP_LOGI(TAG, "probe: PARTNUM=0x%02X VERSION=0x%02X -> CC1101 detected (known revision)",
                 ident->partnum, ident->version);
    } else if (ident->present) {
        ESP_LOGW(TAG, "probe: PARTNUM=0x%02X VERSION=0x%02X -> chip answered, but this is not a "
                      "documented revision; continuing",
                 ident->partnum, ident->version);
    } else {
        ESP_LOGE(TAG, "probe: PARTNUM=0x%02X VERSION=0x%02X -> no CC1101 detected "
                      "(check 3V3, GND and the SPI wiring)",
                 ident->partnum, ident->version);
    }
    return ESP_OK;
}

esp_err_t cc1101_configure(cc1101_handle_t dev, const cc1101_radio_cfg_t *cfg)
{
    if (dev == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const cc1101_mode_t previous = dev->mode;
    esp_err_t err = cc1101_set_mode(dev, CC1101_MODE_IDLE);
    if (err != ESP_OK) {
        return err;
    }

    const bool is_ook = (cfg->modulation == CC1101_MOD_ASK_OOK);

    uint8_t freq[3];
    cc1101_calc_freq(cfg->freq_hz, freq);

    uint8_t drate_e, drate_m;
    cc1101_calc_drate(cfg->datarate_bps, &drate_e, &drate_m);

    uint8_t bw_e, bw_m;
    cc1101_calc_chanbw(cfg->rx_bandwidth_hz, &bw_e, &bw_m);

    const uint8_t mdmcfg4 = (uint8_t)((bw_e << 6) | (bw_m << 4) | (drate_e & 0x0Fu));
    const uint8_t mdmcfg2 = (uint8_t)(cc1101_mod_format(cfg->modulation) << 4);

    int8_t pa_actual = 0;
    const uint8_t pa_level = cc1101_pa_value(cfg->tx_power_dbm, &pa_actual);

    /* Register list. Each entry documents what it does; the derived ones are
     * computed above from cc1101_radio_cfg_t. Order follows the address map. */
    const struct { uint8_t addr; uint8_t val; } regs[] = {
        /* FIFOTHR - the FIFOs are unused in async serial mode, but bit 6
         * (ADC_RETENTION) must be set when the RX filter bandwidth is <= 325 kHz
         * to keep the documented sensitivity. */
        { CC1101_FIFOTHR,  0x47u },

        /* PKTCTRL1 - no address check, no appended status bytes, no preamble
         * quality threshold. Nothing about these transmitters is addressed. */
        { CC1101_PKTCTRL1, 0x00u },

        /* PKTCTRL0 - THE central register. PKT_FORMAT=3 (asynchronous serial),
         * CRC_EN=0, LENGTH_CONFIG=2 (infinite packet length), no whitening.
         * This is what turns the chip from a packet radio into a raw modem. */
        { CC1101_PKTCTRL0, 0x32u },

        /* CHANNR - channel 0: the carrier is set by FREQ2/1/0 alone. */
        { CC1101_CHANNR,   0x00u },

        /* FSCTRL1/0 - intermediate frequency ~152 kHz and zero frequency
         * offset; the SmartRF Studio value for a 433 MHz OOK receiver. */
        { CC1101_FSCTRL1,  0x06u },
        { CC1101_FSCTRL0,  0x00u },

        /* FREQ2/1/0 - carrier, derived: f = (F_XOSC / 2^16) * FREQ. */
        { CC1101_FREQ2,    freq[0] },
        { CC1101_FREQ1,    freq[1] },
        { CC1101_FREQ0,    freq[2] },

        /* MDMCFG4/3 - channel filter bandwidth (CHANBW_E/M) and data-rate
         * exponent/mantissa, all derived. Even though the host does the timing
         * in async mode, the data rate still sets the demodulator's post-
         * detection filter, so it decides how clean the captured edges are. */
        { CC1101_MDMCFG4,  mdmcfg4 },
        { CC1101_MDMCFG3,  drate_m },

        /* MDMCFG2 - modulation format from the config; DEM_DCFILT_OFF=0 (DC
         * blocking filter enabled), MANCHESTER_EN=0, and SYNC_MODE=0: no
         * preamble, no sync word, no carrier-sense gating of the data line.
         * Any nonzero SYNC_MODE would make the chip wait for a sync word that
         * these transmitters never send. */
        { CC1101_MDMCFG2,  mdmcfg2 },

        /* MDMCFG1/0 - FEC off, 2 preamble bytes (unused without sync), channel
         * spacing ~200 kHz. Reset values; nothing here is load-bearing while
         * CHANNR is 0. */
        { CC1101_MDMCFG1,  0x22u },
        { CC1101_MDMCFG0,  0xF8u },

        /* DEVIATN - FSK deviation. Ignored entirely in OOK; kept at the SmartRF
         * default so the FSK modes are not left with a nonsense value. */
        { CC1101_DEVIATN,  0x15u },

        /* MCSM1 - CCA_MODE=3 (only transmit when the channel is clear per RSSI
         * unless already receiving a packet - inert here since we never use the
         * TX FIFO), RXOFF_MODE=3 and TXOFF_MODE=3 so the radio *stays* in RX or
         * TX. In async serial mode there is no packet boundary to return from;
         * dropping back to IDLE would silently end the capture. */
        { CC1101_MCSM1,    0x3Fu },

        /* MCSM0 - FS_AUTOCAL=1: calibrate the synthesizer on every IDLE->RX/TX
         * transition. Mode switches happen constantly here (capture, replay,
         * capture) and this removes any need for manual SCAL. PO_TIMEOUT=2
         * (~150 us) covers the regulator/crystal settling after power-up. */
        { CC1101_MCSM0,    0x18u },

        /* FOCCFG - frequency offset compensation. Meaningless for OOK (there is
         * no frequency to track) but harmless; SmartRF's OOK value. */
        { CC1101_FOCCFG,   0x16u },

        /* BSCFG - bit synchronization / clock recovery. Also inert in async
         * serial mode, where no clock is recovered; reset value kept. */
        { CC1101_BSCFG,    0x6Cu },

        /* AGCCTRL2 - the register that decides whether RX is usable.
         * MAX_DVGA_GAIN=1 forbids the top digital VGA gain step: without that
         * limit the AGC winds all the way up in the silence between frames and
         * the data line becomes solid noise that swamps the RMT glitch filter.
         * MAX_LNA_GAIN=0 keeps the full LNA range (we still want weak remotes),
         * MAGN_TARGET=3 (33 dB) is the SmartRF ASK/OOK target. */
        { CC1101_AGCCTRL2, 0x43u },

        /* AGCCTRL1 - AGC_LNA_PRIORITY=1 (reduce LNA gain first, then DVGA),
         * relative carrier-sense threshold disabled, absolute threshold at the
         * magnitude target. That absolute threshold is what makes PKTSTATUS.CS
         * meaningful for the RF_ENERGY_NO_PULSES diagnostic. */
        { CC1101_AGCCTRL1, 0x40u },

        /* AGCCTRL0 - HYST_LEVEL=2 (medium hysteresis so the AGC does not
         * oscillate on a keyed carrier), WAIT_TIME=1 (16 samples), and in
         * ASK/OOK the FILTER_LENGTH field is reinterpreted as the OOK DECISION
         * BOUNDARY: 1 = 8 dB between the "on" and "off" levels. That is the
         * slicer threshold; too low slices noise, too high loses weak remotes. */
        { CC1101_AGCCTRL0, 0x91u },

        /* WORCTRL - wake-on-radio is unused, but the datasheet's recommended
         * value (EVENT1=7, RC oscillator calibration enabled) is written so the
         * RC oscillator settings are not left undefined. */
        { CC1101_WORCTRL,  0xFBu },

        /* FREND1 - RX front-end bias currents; the SmartRF ASK/OOK value. */
        { CC1101_FREND1,   0xB6u },

        /* FREND0 - PA_POWER selects which PATABLE index is the "on" level. For
         * OOK it MUST be 1: the modulator keys between PATABLE[0] (off) and
         * PATABLE[PA_POWER]. Leaving this at 0 makes both levels identical and
         * produces literally no RF output. FSK uses index 0 instead. */
        { CC1101_FREND0,   (uint8_t)(is_ook ? 0x11u : 0x10u) },

        /* FSCAL3..0 - synthesizer calibration constants from SmartRF Studio.
         * FS_AUTOCAL overwrites FSCAL3[3:0], FSCAL2 and FSCAL1 at each
         * calibration; these are the correct starting points. */
        { CC1101_FSCAL3,   0xE9u },
        { CC1101_FSCAL2,   0x2Au },
        { CC1101_FSCAL1,   0x00u },
        { CC1101_FSCAL0,   0x1Fu },

        /* TEST2/TEST1 - the datasheet's "RX filter bandwidth <= 325 kHz" pair
         * (0x81 / 0x35); the other documented pair is for wider filters.
         * TEST0 - 0x09 selects the VCO used below 861 MHz. All three are
         * undocumented internals: SmartRF's values, copied verbatim. */
        { CC1101_TEST2,    0x81u },
        { CC1101_TEST1,    0x35u },
        { CC1101_TEST0,    0x09u },
    };

    for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        err = cc1101_write_reg(dev, regs[i].addr, regs[i].val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "configure: write 0x%02X failed: %s", regs[i].addr, esp_err_to_name(err));
            return err;
        }
    }

    /*
     * PATABLE. In OOK the PA is keyed between two table entries, so entry 0 is
     * the OFF level and must be 0x00 while entry 1 (selected by FREND0.PA_POWER
     * above) carries the actual power. For the FSK modes there is no keying and
     * the level goes in entry 0. The remaining entries are zeroed so a stale
     * table from a previous configuration cannot leak through.
     */
    uint8_t patable[8] = { 0 };
    if (is_ook) {
        patable[0] = 0x00u;
        patable[1] = pa_level;
    } else {
        patable[0] = pa_level;
    }
    err = cc1101_write_burst(dev, CC1101_PATABLE, patable, sizeof(patable));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configure: PATABLE write failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Report what the hardware actually got, not what was asked for: the
     * quantized values are what will be on the air. */
    const uint32_t freq_actual =
        (uint32_t)((((uint64_t)freq[0] << 16 | (uint64_t)freq[1] << 8 | freq[2]) *
                    CC1101_F_XOSC) >> 16);
    const uint32_t drate_actual =
        (uint32_t)((((uint64_t)(256u + drate_m) << drate_e) * CC1101_F_XOSC) >> 28);
    const uint32_t bw_actual =
        (uint32_t)(CC1101_F_XOSC / (8ull * (4u + bw_m) * (1ull << bw_e)));

    ESP_LOGI(TAG, "configured: %lu Hz (req %lu), %s, %lu Baud (req %lu), BW %lu Hz (req %lu), "
                  "TX %d dBm (req %d, PATABLE 0x%02X)",
             (unsigned long)freq_actual, (unsigned long)cfg->freq_hz,
             is_ook ? "ASK/OOK" : "FSK/MSK",
             (unsigned long)drate_actual, (unsigned long)cfg->datarate_bps,
             (unsigned long)bw_actual, (unsigned long)cfg->rx_bandwidth_hz,
             (int)pa_actual, (int)cfg->tx_power_dbm, pa_level);

    /* Put the radio back where the caller had it. */
    return cc1101_set_mode(dev, previous);
}

esp_err_t cc1101_set_mode(cc1101_handle_t dev, cc1101_mode_t mode)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Always transit through IDLE. Going straight from RX to TX would leave the
     * old IOCFG0 in force while the state machine is already changing direction,
     * which is precisely the window in which the CC1101 and the ESP32 end up
     * driving GDO0 at the same time.
     */
    esp_err_t err = cc1101_strobe(dev, CC1101_SIDLE, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = cc1101_wait_state(dev, CC1101_MARC_IDLE);
    if (err != ESP_OK) {
        return err;
    }

    switch (mode) {
    case CC1101_MODE_IDLE:
        /* Flush both FIFOs. They are not used in async serial mode, but a FIFO
         * left in an overflow/underflow state blocks later state transitions,
         * and flushing is only legal from IDLE - which is where we now are. */
        err = cc1101_strobe(dev, CC1101_SFRX, NULL);
        if (err == ESP_OK) {
            err = cc1101_strobe(dev, CC1101_SFTX, NULL);
        }
        if (err != ESP_OK) {
            return err;
        }
        /* GDO0 three-stated so nothing is driven while the radio is quiet. */
        err = cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDOx_HIGH_Z);
        break;

    case CC1101_MODE_RX_ASYNC:
        /* GDO0 becomes the demodulator's asynchronous serial data OUTPUT. The
         * ESP32 side must be an input on this pin (RMT RX); this driver never
         * touches the host-side direction, it only tells the chip to drive. */
        err = cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDOx_SERIAL_DATA_OUT);
        if (err == ESP_OK) {
            err = cc1101_strobe(dev, CC1101_SRX, NULL);
        }
        break;

    case CC1101_MODE_TX_ASYNC:
        /* GDO0 becomes the modulator's data INPUT. In async serial TX the chip
         * samples the pin regardless of IOCFG0, so IOCFG0 must be set to
         * three-state first - otherwise the CC1101 keeps driving the line and
         * fights the ESP32's RMT transmitter, and nothing is keyed. This
         * ordering (release, then STX) is the whole point of the function. */
        err = cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDOx_HIGH_Z);
        if (err == ESP_OK) {
            err = cc1101_strobe(dev, CC1101_STX, NULL);
        }
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_mode(%d) failed: %s", (int)mode, esp_err_to_name(err));
        return err;
    }

    dev->mode = mode;
    ESP_LOGD(TAG, "mode -> %s",
             (mode == CC1101_MODE_RX_ASYNC) ? "RX_ASYNC" :
             (mode == CC1101_MODE_TX_ASYNC) ? "TX_ASYNC" : "IDLE");
    return ESP_OK;
}

cc1101_mode_t cc1101_get_mode(cc1101_handle_t dev)
{
    return (dev != NULL) ? dev->mode : CC1101_MODE_IDLE;
}

esp_err_t cc1101_rssi_dbm(cc1101_handle_t dev, int *dbm_out)
{
    if (dev == NULL || dbm_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw = 0;
    esp_err_t err = cc1101_read_reg(dev, CC1101_RSSI, &raw);
    if (err != ESP_OK) {
        return err;
    }

    /* Datasheet §17.3: the register is a 2's-complement value in half-dB steps
     * with a band-dependent offset (74 dB at 433 MHz). */
    if (raw >= 128u) {
        *dbm_out = ((int)raw - 256) / 2 - 74;
    } else {
        *dbm_out = (int)raw / 2 - 74;
    }
    return ESP_OK;
}

esp_err_t cc1101_carrier_sense(cc1101_handle_t dev, bool *asserted)
{
    if (dev == NULL || asserted == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t status = 0;
    esp_err_t err = cc1101_read_reg(dev, CC1101_PKTSTATUS, &status);
    if (err != ESP_OK) {
        return err;
    }

    /* PKTSTATUS bit 6 = CS (carrier sense), driven by the AGC thresholds set in
     * AGCCTRL1. This is the evidence that separates "RF energy present but
     * nothing decodes" from "the band is silent" (PLAN.md §7). */
    *asserted = (status & 0x40u) != 0u;
    return ESP_OK;
}

esp_err_t cc1101_set_gdo2_function(cc1101_handle_t dev, uint8_t iocfg2)
{
    if (dev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "GDO2 (GPIO%d) IOCFG2 = 0x%02X", (int)dev->pins.gdo2, iocfg2);
    return cc1101_write_reg(dev, CC1101_IOCFG2, iocfg2);
}

void cc1101_deinit(cc1101_handle_t dev)
{
    if (dev == NULL) {
        return;
    }

    if (dev->spi != NULL) {
        /* Best effort: quiet the radio and release both GDO pins before the bus
         * goes away, so a later re-init starts from a known state. */
        (void)cc1101_strobe(dev, CC1101_SIDLE, NULL);
        (void)cc1101_write_reg(dev, CC1101_IOCFG0, CC1101_GDOx_HIGH_Z);
        (void)cc1101_write_reg(dev, CC1101_IOCFG2, CC1101_GDOx_HIGH_Z);
        spi_bus_remove_device(dev->spi);
    }
    if (dev->bus_initialized) {
        spi_bus_free(dev->pins.host);
    }
    gpio_set_level(dev->pins.cs, 1);
    gpio_reset_pin(dev->pins.cs);

    free(dev);
}
