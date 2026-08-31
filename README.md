# doorbell433

A **generic 433 MHz receive / analyze / replay appliance** on an ESP32-S3 with a
CC1101 transceiver — built around a wireless doorbell, but deliberately not a
doorbell hack.

Register your doorbell buttons, replay their signals, synthesize new "virtual"
signals to pair your own chimes to, and wire it all together in a node graph:
*this button rings those chimes, these three all ring the upstairs one, and Home
Assistant sees every press.*

> **Status:** the RF chain is **validated on real hardware** — a live unit
> captures, decodes and replays a real doorbell, and the replay rings the actual
> chime. The appliance layer (web UI, MQTT, OTA) is in active development.

---

## What makes this different

Most 433 MHz projects hardcode one protocol and stop. This one is layered so the
protocol is the *last* thing that matters:

```
RF ─▶ CC1101 (async OOK) ─▶ GDO0 ─▶ RMT hardware edge capture
                                       │
                                 raw pulse frame          ← protocol-agnostic
                                       │
                            timing normalization           ← learns the base width
                                       │
                            decoder plugins (EV1527, …)    ← optional
```

**An undecodable capture is still a first-class citizen.** It can be stored,
matched and replayed exactly like a recognized one, because the recording is raw
timings, not decoded bits. EV1527 is one plugin on top — not the architecture.

### Why the RMT peripheral, not a GPIO interrupt

These remotes use pulses a few hundred microseconds wide. A `micros()`-in-an-ISR
approach has to beat Wi-Fi, lwIP and flash writes to every single edge, and the
jitter corrupts captures exactly when you care most — while the web UI is open.
The RMT peripheral timestamps edges **in hardware** and interrupts once per
frame. The ISR is IRAM-safe, so capture survives even a config save that disables
the flash cache.

### Timings are learned, never hardcoded

Cheap remotes use RC oscillators that drift with temperature and battery age, so
the "correct" pulse width is a property of each capture. The normalizer
histograms the durations, finds the dominant short cluster, and expresses
everything as multiples of it. The classic 1:3 short/long ratio is an *output* of
that analysis, not an input assumption.

---

## Hardware

| CC1101 (Ebyte E07-M1101D V2.0, 433 MHz) | ESP32-S3 |
|---|---|
| 1 GND | GND |
| 2 VCC | 3V3 |
| 3 GDO0 | GPIO 4 |
| 4 CSN | GPIO 10 |
| 5 SCK | GPIO 12 |
| 6 MOSI | GPIO 11 |
| 7 MISO | GPIO 13 |
| 8 GDO2 | GPIO 5 |

Both sides are 3.3 V — no level shifter. Every pin lives in
[`firmware/main/board_pins.h`](firmware/main/board_pins.h); porting to another
board is one edit.

Developed on an ESP32-S3-WROOM-1 (N16R8) and kept portable to the **ESP32-S3
Zero**: PSRAM is deliberately unused, and a 4 MB partition table ships alongside
the 16 MB one.

**Optional wired button.** A physical button (your front door switch) can be
wired to any free GPIO and added as a `source.gpio` node — configured *in the web
UI*, not compiled in — so pressing it triggers the wireless chimes just like an
RF button.

---

## Build

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

ESP-IDF **v5.3.1**, target `esp32s3`. For a 4 MB board:

```sh
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" idf.py build
```

---

## Layout

```
firmware/
├── components/
│   ├── cc1101/     chip driver: SPI, registers, async OOK, RSSI. No app knowledge.
│   └── rfpulse/    pulse frames, RMT capture/replay, normalization, decoders.
├── main/
│   ├── board_pins.h      every GPIO, in one place
│   ├── db_diag.*         the shared diagnostic vocabulary
│   ├── rf_service.*      sole radio owner; RX/TX handover, burst coalescing
│   ├── signal_store.*    persisted signals, matching, learn mode
│   ├── node_graph.*      sources → logic → sinks routing engine
│   ├── event_log.*       recent activity ring for the UI
│   ├── http_api.*        REST + web UI serving
│   ├── mqtt_bridge.*     MQTT + Home Assistant discovery
│   ├── db_config.*       versioned NVS config
│   ├── wifi_mgr.*        APSTA + Tasmota-style recovery portal
│   ├── dns_server.*      captive DNS
│   └── ota.*             dual-slot OTA + rollback
├── webui/          mobile-first UI, vanilla JS, flashed as a SPIFFS image
└── host-test/      the pulse logic, tested off-target with plain gcc
```

Both components are reusable outside this project — neither knows what a doorbell
is.

---

## Diagnostics you can act on

"It doesn't work" has at least five different causes on an RF bring-up, so every
layer reports into one vocabulary that appears identically in the serial log, at
`GET /api/diagnostics`, and in the UI:

`CC1101_NOT_DETECTED` · `CC1101_OK` · `SPI_ERROR` · `RADIO_CONFIG_SUSPECT` ·
`RF_ENERGY_NO_PULSES` · `PULSES_CAPTURED` · `REPEAT_FRAME_DETECTED` ·
`PROTOCOL_DECODED` · `UNKNOWN_PROTOCOL_RAW` · `TX_OK` · `TX_FAILED`

`TX_OK` claims only that the transmit completed in software. It deliberately does
not assert that any receiver reacted — the box has no way to know that.

---

## A finding worth repeating

On the bench, real presses and background noise separated like this:

| | pulses | RSSI | confidence |
|---|---|---|---|
| real press | 49–53 | **−24 to −42 dBm** | 67–92% |
| AGC noise | 33–92 | **−91 to −97 dBm** | 24–28% |

Pulse counts **overlap** — no minimum-length filter can separate them. Signal
strength splits the two by ~50 dB with nothing in between, because a silent band
leaves the AGC amplifying thermal noise while a transmitter in the same room is
overwhelmingly louder. An RSSI squelch at −75 dBm took the noise from ~13
captures/minute to **zero**, with real presses unaffected.

---

## Documentation

* [`docs/API.md`](docs/API.md) — the complete REST surface.
* `firmware/webui/README.md` — the UI's structure and the endpoints it needs.

## License

MIT.
