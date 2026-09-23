---
title: Hardware
layout: default
nav_order: 2
---

# Hardware
{: .no_toc }

Two parts and eight wires.
{: .fs-5 .fw-300 }

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Bill of materials

| Part | Notes |
|---|---|
| **ESP32-S3** board | Either of two fully supported boards, running the **same release image**: an ESP32-S3-WROOM-1 **N16R8** dev board (16 MB flash, what the project was developed on) or the tiny **Waveshare ESP32-S3-Zero** (4 MB) — see [below](#on-the-waveshare-esp32-s3-zero). Must expose the **native USB** port for the browser flasher. |
| **CC1101 module, 433 MHz** | Developed with an **Ebyte E07-M1101D V2.0**. Any CC1101 breakout with the standard 8-pin header works. |
| Antenna | A 433 MHz whip, or a 17.3 cm piece of wire (quarter wave). |
| Jumper wires | Eight. |

PSRAM is **deliberately unused**, so the R8 in "N16R8" is not a requirement — it just
happens to be what the development board has.

## Wiring

Both sides are 3.3 V logic. **No level shifter, no resistors, nothing in between.**

| CC1101 (E07-M1101D V2.0) | ESP32-S3 | Purpose |
|---|---|---|
| 1 GND | GND | |
| 2 VCC | **3V3** | Never 5 V — the CC1101 is a 3.3 V part. |
| 3 GDO0 | **GPIO 4** | Async OOK data in/out; the pin the RMT peripheral watches. |
| 4 CSN | **GPIO 10** | SPI chip select. |
| 5 SCK | **GPIO 12** | SPI clock. |
| 6 MOSI | **GPIO 11** | SPI, host → radio. |
| 7 MISO | **GPIO 13** | SPI, radio → host. |
| 8 GDO2 | **GPIO 5** | Secondary status output. |

```
   ESP32-S3                     CC1101 (E07-M1101D)
   ┌──────────┐                 ┌──────────┐
   │      3V3 ├─────────────────┤ VCC (2)  │
   │      GND ├─────────────────┤ GND (1)  │
   │  GPIO 10 ├─────────────────┤ CSN (4)  │
   │  GPIO 12 ├─────────────────┤ SCK (5)  │
   │  GPIO 11 ├─────────────────┤ MOSI (6) │
   │  GPIO 13 ├─────────────────┤ MISO (7) │
   │   GPIO 4 ├─────────────────┤ GDO0 (3) │
   │   GPIO 5 ├─────────────────┤ GDO2 (8) │
   └──────────┘                 └──────────┘
```

Every pin lives in one place —
[`firmware/main/board_pins.h`](https://github.com/MarvAmBass/klingelbox/blob/main/firmware/main/board_pins.h).
Porting to another board is one edit to that file.

### On the Waveshare ESP32-S3-Zero

The table above is not dev-board-only: `board_pins.h` was written so that every pin in it
is free on the **Waveshare ESP32-S3-Zero** (ESP32-S3FH4R2: 4 MB flash, 2 MB PSRAM) as
well. Same eight wires, same GPIO numbers — this table only adds where they sit on the
Zero, using the board's `GP` silkscreen labels:

| CC1101 (E07-M1101D V2.0) | ESP32-S3-Zero | `board_pins.h` macro |
|---|---|---|
| 1 GND | GND — left edge, beside the USB-C | |
| 2 VCC | **3V3** — left edge, marked `3V3(OUT)` | |
| 3 GDO0 | **GP4** — left edge | `DB_PIN_CC1101_GDO0` |
| 4 CSN | **GP10** — right edge | `DB_PIN_CC1101_CS` |
| 5 SCK | **GP12** — right edge | `DB_PIN_CC1101_SCK` |
| 6 MOSI | **GP11** — right edge | `DB_PIN_CC1101_MOSI` |
| 7 MISO | **GP13** — right edge | `DB_PIN_CC1101_MISO` |
| 8 GDO2 | **GP5** — left edge | `DB_PIN_CC1101_GDO2` |

All eight are on the castellated **edge** headers — none land on the Zero's three
solder-only pads (GP14–GP16) or the pads on the underside — so ordinary pin headers or
jumper wires reach everything.

Zero-specific notes, all of them harmless here:

- **`3V3(OUT)`** is the output of the Zero's onboard regulator. The CC1101's tens of
  milliamps are well within it; power the Zero itself over its USB-C.
- The onboard **WS2812 LED sits on GPIO 21**, which this firmware never touches
  (`DB_PIN_STATUS_LED` is deliberately unset).
- **GPIO 33–37 are not led out** on the Zero (reserved for the in-package PSRAM). The
  firmware uses none of them.
- The wired-button defaults **GPIO 6** (left edge) and **GPIO 7** (right edge) are on the
  headers too — see [the optional wired button](#an-optional-wired-button).
- The USB-C socket **is** the ESP32-S3's native USB, so the browser flasher talks to it —
  and it flashes the same release image as every other board; see
  [Board variants](#board-variants) below.

### Why GDO0 and not an interrupt on any pin

These remotes use pulses a few hundred microseconds wide. A `micros()`-in-an-ISR approach
has to beat Wi-Fi, lwIP and flash writes to every single edge, and the resulting jitter
corrupts captures exactly when you care most — while the web UI is open and the radio is
busy. GDO0 goes to a pin the **RMT peripheral** can watch, so edges are timestamped in
hardware and the CPU is interrupted once per frame. The ISR is IRAM-safe, so capture
survives even a config save that disables the flash cache.

## Power and antenna

- **Power the board over USB** for bring-up. A CC1101 transmitting draws tens of
  milliamps in bursts; a weak USB supply shows up as brownouts during transmit, not
  during receive, which makes it look like a radio fault. If replays fail but captures
  work, suspect the supply before the wiring.
- **Fit an antenna before judging reception.** Without one, sensitivity collapses and you
  will see the AGC noise described below and nothing else. A 17.3 cm wire soldered to the
  antenna pad is enough to get started.
- Keep the CC1101 module and its antenna a few centimetres away from the ESP32-S3's own
  Wi-Fi antenna. They are different bands, but a module sitting directly on top of a
  radiating Wi-Fi antenna still raises the noise floor.

## An optional wired button

A physical button — your actual front-door switch — can be wired to **any free GPIO** and
added as a `source.gpio` node. The pin is chosen **in the web UI**, not compiled in, so no
rebuild is needed.

Simplest wiring, and the default the firmware assumes:

```
GPIO ──┬── button ── GND
       └── (internal pull-up, enabled by the firmware)
```

That is `active_low = true`: an unpressed or entirely unconnected pin reads as "not
pressed" instead of floating. Mechanical buttons bounce for milliseconds, so the node also
carries a debounce time (50 ms by default) — without it a single press fires the chain
several times.

`GET /api/gpio/available` tells the UI which pins it may offer: pins 4, 5, 10, 11, 12 and
13 are taken by the radio, and the picker will not show them.

## Board variants

| Board | Flash | Image | Notes |
|---|---|---|---|
| ESP32-S3-WROOM-1 (N16R8) | 16 MB | The one release image | The development board. Upper 12 MB of flash unused. |
| **Waveshare ESP32-S3-Zero** (FH4R2) | 4 MB | The one release image | Fully supported — same image, same flasher, same OTA. |

**One image serves both boards.** The single partition layout (`partitions.csv`) is sized
for 4 MB flash — 0x170000 (~1.4 MB) per app slot, 1 MB web-UI SPIFFS, 80 KB NVS — so the
release image boots on any supported board; a 16 MB board just leaves the rest of its
flash unused. Picking a board is purely a wiring question, never a firmware one.

The layout uses **dual OTA app slots**, so an update is rollback-safe: the updater writes
the inactive slot, reboots, and a bad image is reverted on the next boot. The web UI lives
in its own `storage` SPIFFS partition, so it can be replaced without recompiling the
firmware.

Boxes flashed with an **older release** (v0.8.0 and earlier used a 16 MB-only table) need
nothing: OTA never rewrites the partition table, so they keep their old layout and update
exactly as before. See [Flashing](flashing.html#one-image-every-board).

## Bring-up: is the radio actually there?

"It doesn't work" has at least five different causes on an RF bring-up, so every layer
reports into one shared vocabulary that appears identically in the serial log, at
`GET /api/diagnostics`, and in the web UI:

| State | What it means for you |
|---|---|
| `CC1101_NOT_DETECTED` | SPI wiring or power. Check CSN/SCK/MOSI/MISO and 3V3 first. |
| `CC1101_OK` | The chip answered with its part number and version. Wiring is good. |
| `SPI_ERROR` | The bus is talking but unreliably — loose jumper, long wires. |
| `RADIO_CONFIG_SUSPECT` | The chip is there but its registers do not read back as written. |
| `RF_ENERGY_NO_PULSES` | Energy on the band, no usable edges. Usually a missing antenna. |
| `PULSES_CAPTURED` | Edges are arriving. The radio path works. |
| `REPEAT_FRAME_DETECTED` | The same frame arrived several times — a real remote, held down. |
| `PROTOCOL_DECODED` | A decoder plugin recognised the frame. |
| `UNKNOWN_PROTOCOL_RAW` | Captured but not decoded — **fully supported**, still replayable. |
| `TX_OK` | The transmit completed in software. It does *not* claim a chime reacted. |
| `TX_FAILED` | The transmit did not complete. |

Work down that list in order: no `CC1101_OK` means it is a wiring problem, and nothing
above the radio layer is worth looking at yet.

## Radio defaults

| Setting | Default | Where |
|---|---|---|
| Frequency | 433.92 MHz | `GET`/`POST /api/radio` |
| Modulation | ASK/OOK | |
| Data rate | 5000 bps | Sets demodulator filtering even in async mode. |
| Channel bandwidth | 203 kHz | |
| TX power | 10 dBm | Modest on purpose — regional limits apply. |
| TX repeats | 6 | |
| TX gap | 8000 µs | Silence between repeats. |

**Repeats are not cosmetic.** Real doorbell receivers integrate several copies of a frame
before they accept it, because the original remote transmits for as long as the button is
held. A single replay is routinely ignored; six copies is the difference between "the
chime rings" and "nothing happens".

None of these are hardcoded constants — they are configuration, editable live over the
REST API, and they persist across reboots.
