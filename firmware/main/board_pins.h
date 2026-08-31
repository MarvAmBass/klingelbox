/*
 * board_pins.h - THE single place every GPIO number in this firmware comes from.
 *
 * Why this file exists: the firmware is developed on a large ESP32-S3-N16R8 dev
 * board but must move unchanged to a much smaller ESP32-S3 Zero. Nothing outside
 * this header may hardcode a pin number, and nothing may depend on a peculiarity
 * of the development board (no onboard-LED assumptions, no strapping-pin reuse).
 * Porting to another board should be exactly one edit: this file.
 *
 * Every pin below is free on both boards.
 *
 * Wiring — Ebyte E07-M1101D V2.0 (CC1101, 433 MHz), 8-pin header:
 *
 *   CC1101 pin 1  GND        -> GND
 *   CC1101 pin 2  VCC        -> 3V3        (both sides 3.3 V: no level shifter)
 *   CC1101 pin 3  GDO0       -> GPIO 4     <- demodulated async OOK data (RX+TX)
 *   CC1101 pin 4  CSN        -> GPIO 10
 *   CC1101 pin 5  SCK        -> GPIO 12
 *   CC1101 pin 6  MOSI (SI)  -> GPIO 11
 *   CC1101 pin 7  MISO (SO)  -> GPIO 13    (doubles as GDO1 on the CC1101)
 *   CC1101 pin 8  GDO2       -> GPIO 5     <- kept wired and driver-configurable
 *
 * GDO0 note: in the CC1101's asynchronous serial mode this one pin is the
 * demodulated data OUTPUT while receiving and the data INPUT while transmitting.
 * The ESP32 side therefore switches its direction with the radio mode — RMT RX
 * claims it to capture, RMT TX claims it to replay. See components/rfpulse.
 *
 * GDO2 note: not required for the basic capture/replay path, but it stays wired
 * and exposed so it can carry a second signal (e.g. carrier sense / sync detect)
 * without a hardware change — a stated architectural requirement.
 */
#ifndef DB_BOARD_PINS_H
#define DB_BOARD_PINS_H

#include "driver/gpio.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CC1101 SPI bus ---- */
#define DB_PIN_CC1101_SCK    GPIO_NUM_12
#define DB_PIN_CC1101_MOSI   GPIO_NUM_11
#define DB_PIN_CC1101_MISO   GPIO_NUM_13
#define DB_PIN_CC1101_CS     GPIO_NUM_10

/* SPI2_HOST (a.k.a. FSPI) is the general-purpose host on the S3 and is free on
 * both target boards; SPI3 is left alone. The CC1101 tops out around 6.5 MHz for
 * burst access, so 4 MHz is a conservative, reliable choice on jumper wiring. */
#define DB_CC1101_SPI_HOST   SPI2_HOST
#define DB_CC1101_SPI_HZ     (4 * 1000 * 1000)

/* ---- CC1101 GDO pins ---- */
/* GDO0: the async OOK data line. Direction flips with the radio mode. */
#define DB_PIN_CC1101_GDO0   GPIO_NUM_4
/* GDO2: wired, configurable, unused by default. */
#define DB_PIN_CC1101_GDO2   GPIO_NUM_5

/* ---- Optional user I/O ----
 * Deliberately unset: the dev board and the S3 Zero disagree about what is on
 * these, and the firmware must not assume either. Set to a real GPIO only if a
 * given build actually has the hardware.
 */
#define DB_PIN_STATUS_LED    GPIO_NUM_NC   /* -1 = none */

/* ---- Wired button inputs ----
 * A physical button (e.g. the one at the front door) can be wired straight to
 * the ESP32 and act as another event source in the node graph, so pressing it
 * triggers the wireless chimes exactly as an RF button would.
 *
 * WIRING: button between the GPIO and GND. The firmware enables the internal
 * pull-up and treats the input as ACTIVE LOW, so no external resistor is needed
 * and an unconnected pin reads as "not pressed" rather than floating.
 *
 * These are only the DEFAULTS offered in the UI; the actual pin for each wired
 * input is stored per-node in the graph, so it is configurable without a
 * rebuild. GPIO 6 and 7 are free on both the dev board and the S3 Zero, and are
 * not strapping pins.
 */
#define DB_PIN_WIRED_IN_0    GPIO_NUM_6
#define DB_PIN_WIRED_IN_1    GPIO_NUM_7

#ifdef __cplusplus
}
#endif

#endif /* DB_BOARD_PINS_H */
