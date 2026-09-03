---
title: Flashing
layout: default
nav_order: 3
---

# Flashing
{: .no_toc }

Three ways to get firmware onto the box: your browser, `idf.py`, or OTA.
{: .fs-5 .fw-300 }

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Which route?

| Situation | Use |
|---|---|
| First flash, or a board in an unknown state | **[Browser flasher](flasher/)** — nothing to install. |
| You are developing the firmware | **`idf.py`** — you need the build anyway. |
| A working Klingelbox you want to update | **OTA** — keeps all configuration. |
| A 4 MB ESP32-S3 Zero | Build it yourself, then browser flasher or `idf.py`. |

A full-image flash **factory-resets** the device: Wi-Fi credentials, hostname, MQTT
settings, every learned signal and the whole node graph are wiped. Only OTA preserves
them.

---

## Route 1 — the browser flasher

<p>
  <a class="btn btn-primary" href="flasher/">Open the Klingelbox Web Flasher</a>
</p>

It runs entirely in the page: it fetches the release image from this site, verifies its
SHA-256, and writes it to your board over the ESP32-S3's native USB. No firmware,
credentials or serial data leave your computer.

### Requirements

- **Chrome or Edge on a desktop** (Windows, macOS or Linux). The
  [Web Serial API](https://developer.mozilla.org/en-US/docs/Web/API/Web_Serial_API) does
  not exist in Safari, does not exist in Firefox, and does not exist in *any* browser on
  iOS or Android — including Chrome on those platforms. The page detects this up front and
  says so rather than failing halfway through.
- A **data** USB cable. Charge-only cables are the single most common reason the port list
  comes up empty.
- The board's **native USB** socket (USB-Serial/JTAG), not a separate UART bridge port, if
  it has both.

### Download mode (the "BOOT dance")

Many boards enumerate without any of this. If yours does not appear in the browser's port
prompt:

1. Unplug the board.
2. **Hold `BOOT`** (labelled `0` or `IO0` on some boards).
3. Still holding it, **plug the USB cable in**.
4. **Release `BOOT`.**

Already plugged in? Hold `BOOT`, tap `RESET` (`RST`/`EN`), release `BOOT`.

{: .warning }
> **Let go of `BOOT` once the board is plugged in.**
> `BOOT` is not a "flash mode" switch you hold down for the duration. It is read *only* at
> the instant of power-up or reset. If you keep it pressed — or wedge it down with tape
> because it seemed to help — the chip re-enters the ROM download loader on **every** boot
> and the firmware you just wrote never runs. The flash reports success, the board looks
> dead, and nothing anywhere tells you why. This genuinely cost the author an afternoon.

### What it writes

One segment: `klingelbox-esp32s3-full.bin` at offset **`0x0`**. That is the merged image —
bootloader, partition table, otadata, app and the web-UI SPIFFS in one file — so a board
that had nothing on it is complete afterwards.

### After flashing

1. Make sure `BOOT` is free, then let the board reset (unplug/replug if needed).
2. It boots with no Wi-Fi and raises an open recovery hotspot called
   **`Klingelbox-XXXX`** (the last two bytes of its MAC), on channel 1.
3. Join it. The captive portal opens the Wi-Fi wizard automatically; if it does not,
   browse to `http://192.168.66.1`.
4. Enter your Wi-Fi. The box reboots onto your network at `http://klingelbox.local`.

### Can I brick the board?

No. The ESP32-S3's first-stage bootloader is in mask ROM and cannot be overwritten.
Whatever is (or is not) in flash, the BOOT dance always brings the chip back into download
mode. The flasher also offers a full **erase** for boards in a state you no longer trust —
that too is recoverable.

---

## Route 2 — `idf.py`

ESP-IDF **v5.3.1**, target `esp32s3`.

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

The port is `/dev/ttyACM0`-ish on Linux for the native USB port (`/dev/ttyUSB0` if you are
going through a UART bridge), `/dev/cu.usbmodem*` on macOS, `COMx` on Windows.

The web UI lives in the `storage` SPIFFS partition and is built and flashed as a separate
image. `idf.py flash` handles the app; the SPIFFS image is produced by the build and
written the same way.

### The 4 MB ESP32-S3 Zero

```sh
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.4mb" idf.py build
```

This selects `partitions-4mb.csv` (1.5 MB per app slot, 512 KB web-UI SPIFFS). **Keep the
firmware inside 1.5 MB or that table stops fitting.**

A 4 MB and a 16 MB build are not interchangeable — the partition table is baked into the
image, and a 16 MB table on a 4 MB chip points past the end of the flash. If the release
does not ship a 4 MB image, build it yourself and flash the merged result through the
browser flasher's **local .bin** option at offset `0x0`, or with `idf.py`.

---

## Route 3 — OTA (updating a working device)

OTA is the right way to update a Klingelbox that already has your Wi-Fi, signals and
graph, because it keeps all of them. The app has **two slots**: the updater writes the
inactive one, reboots into it, and a bad image is rolled back on the next boot.

The app and the web UI are **separate partitions and separate updates**. Updating the app
leaves the old UI in place until you update the UI too — so after a release that changes
both, do both.

```sh
# app image
curl -X POST http://klingelbox.local/api/ota \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://github.com/MarvAmBass/klingelbox/releases/download/<tag>/klingelbox.bin"}'

# web UI image
curl -X POST http://klingelbox.local/api/ota/webui \
     -H 'Content-Type: application/json' \
     -d '{"url":"https://github.com/MarvAmBass/klingelbox/releases/download/<tag>/storage.bin"}'
```

Or upload the files directly, without the device needing internet access:

```sh
curl -X POST -H 'Content-Type: application/octet-stream' \
     --data-binary @klingelbox.bin http://klingelbox.local/api/ota/upload
curl -X POST -H 'Content-Type: application/octet-stream' \
     --data-binary @storage.bin    http://klingelbox.local/api/ota/webui/upload
```

Both reboot on success. See the
[REST API](https://github.com/MarvAmBass/klingelbox/blob/main/docs/API.md) for the full
contract.

---

## The release assets

| File | What it is | Where it goes |
|---|---|---|
| `klingelbox-esp32s3-full.bin` | Merged image: bootloader + partition table + otadata + app + web UI. | Flash at offset **`0x0`** (browser flasher, or `esptool.py write_flash 0x0`). |
| `klingelbox.bin` | The app only. | `POST /api/ota` — **never** flash this at `0x0`. |
| `storage.bin` | The web-UI SPIFFS image. | `POST /api/ota/webui`. |

Flashing an app-only or SPIFFS image at `0x0` overwrites the bootloader with something
that is not a bootloader. The board will not boot; it is recoverable with a full image,
but it is an entirely avoidable half hour.

---

## Troubleshooting

**The port list is empty.**
Charge-only cable, or the board is not in download mode. Swap the cable first, then redo
the BOOT dance. On Linux you also need access to the device node — usually membership of
the `dialout` group, then log out and back in.

**"Failed to connect" / the connection stalls.**
Something else owns the serial port. Close any serial monitor, Arduino IDE, ESPHome
dashboard or `idf.py monitor` session; only one process can hold a port at a time. Then
redo the BOOT dance.

**"Checksum mismatch" or "size mismatch" in the browser flasher.**
The download did not arrive intact and the page refused to write it. That is the intended
behaviour — a truncated image at `0x0` leaves a board that cannot boot and cannot report
why. Hard-reload the page (`Ctrl`/`Cmd`+`Shift`+`R`) and try again.

**It flashed fine and the board does nothing.**
Check `BOOT` is not still held or jammed — see the warning above. Otherwise unplug and
replug; the USB-Serial/JTAG port re-enumerates silently after a reset. If it still does
nothing, attach a serial monitor at 115200 baud: a wrong image for the board (a 16 MB
image on a 4 MB Zero) shows up there as a partition-table or SPIFFS mount failure.

**`klingelbox.local` does not resolve.**
mDNS is not universal. Find the device's IP in your router's DHCP list and use that. Some
Android versions and some corporate networks block mDNS entirely.

**I flashed it and it never joins my Wi-Fi.**
It falls back to the `Klingelbox-XXXX` recovery hotspot. Join that and re-enter the
credentials — a typo in the password is by far the most common cause. The recovery portal
is deliberately exempt from every "disable the AP" setting, so the box can never make
itself unreachable.
