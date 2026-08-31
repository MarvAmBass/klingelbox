---
title: REST API
layout: default
nav_order: 6
---

# doorbell433 REST API

The contract between `firmware/main/http_api.c` and `firmware/webui/`. Both are
written against this document; neither may invent endpoints.

* Base: `http://<hostname>.local/` (also reachable on the softAP IP).
* No auth, no TLS — by design, a trusted-LAN / AP appliance.
* All responses are JSON. **Every failure is `{"error": "..."}`** with a real
  HTTP status (400 bad input, 404 unknown id, 409 wrong state, 503 no radio).
  `error` is always a **human sentence** — an `ESP_ERR_*` constant is never part
  of it. A few failures add machine-readable fields next to the sentence; those
  are documented at the endpoint that produces them.
* `POST` bodies are JSON unless stated otherwise (the OTA upload routes take raw
  binary).

---

## System

### `GET /api/system`
```json
{ "version": "0.1.0", "idf": "v5.3.1", "hostname": "doorbell",
  "uptime_s": 1234, "free_heap": 210000, "partition": "ota_0",
  "wifi_mode": "normal|recovery|connecting",
  "sta_connected": true, "sta_ip": "192.168.1.42", "sta_ssid": "home",
  "ap_ip": "192.168.66.1", "ap_ssid": "Doorbell433",
  "radio": { "present": true, "partnum": 0, "version": 20 } }
```
`wifi_mode == "recovery"` is the signal for the UI to replace the normal page
with the first-run Wi-Fi wizard.

### `POST /api/system/hostname` — `{"hostname":"doorbell"}` (applies on reboot)
### `POST /api/restart` — `{}` → reboots

---

## Radio + diagnostics

### `GET /api/radio`
```json
{ "freq_hz": 433920000, "modulation": "ook", "datarate_bps": 5000,
  "bandwidth_hz": 203000, "tx_power_dbm": 10,
  "tx_repeats": 6, "tx_gap_us": 8000, "rssi_dbm": -96 }
```
### `POST /api/radio` — any subset of the above; reconfigures the chip live.

### `GET /api/diagnostics`
The states of `db_diag.h`, so the UI can explain a fault instead of showing a
dead page. `name` is the stable machine name; `help` is the human sentence.
```json
{ "states": [ { "name": "CC1101_OK", "help": "The CC1101 responded...",
                "count": 1, "last_us": 115000, "detail": "PARTNUM=0x00 VERSION=0x14" } ],
  "capture": { "frames": 12, "dropped_short": 3, "dropped_full": 0, "overruns": 0 } }
```

---

## Signals

A signal is a stored waveform. A "button" is a signal referenced by a source node.

### `GET /api/signals`
```json
{ "signals": [ { "id": 1, "name": "Front door", "origin": "captured",
                 "created_at": 1756600000, "fingerprint": "5487745f",
                 "base_us": 292, "confidence": 92, "pulse_count": 49,
                 "decoded": { "protocol": "ev1527", "id": 681562, "button": 8,
                              "text": "EV1527 id=0xA685A btn=0x8" },
                 "last_seen_s": 12, "seen_count": 7 } ] }
```
`decoded` is `null` for an unknown protocol — a normal, fully supported state.

### `GET /api/signals/{id}`
As above plus the raw waveform, so the UI can draw it:
```json
{ "id": 1, "...": "...", "first_level": 1, "durations_us": [919,273,297,...] }
```
### `POST /api/signals/{id}` — `{"name":"..."}`
### `DELETE /api/signals/{id}`
### `POST /api/signals/{id}/transmit` — `{"repeats":6,"gap_us":8000}` (both optional)
**503** if no radio is present. Response `{"ok":true}` means *software-level*
success only — it does not assert that any receiver reacted.

`gap_us` is a **minimum** idle between copies, not an addition. If the frame's
own last pulse is idle (carrier off) it already supplies part of that idle and
only the shortfall is appended; and if that trailing idle dwarfs every other
pulse in the frame it *is* the protocol's framing gap, so the frame goes out
with exactly the period it was authored with and nothing is appended. A
synthesized EV1527 word is in the second case (its sync gap is 31x the base
pulse); a captured frame is almost always in the first, because the idle that
ENDS a recording is the very gap that would have been at its end.

### `POST /api/signals/virtual`
Creates a synthesized EV1527 signal to pair one of the user's own receivers to.
```json
{ "name": "Virtual chime 1", "id20": 0, "button": 8, "base_us": 350,
  "allow_duplicate": false }
```
`id20: 0` (or omitted) draws a random address, avoiding every address already
stored. Returns the created signal.

**409** when another stored signal already carries the same protocol + address +
button, because an incoming burst could not be attributed to one of them:

```json
{ "error": "Signal 'Test 1' already uses address 0xA685A with button 0x8, ...",
  "conflict_signal_id": 1 }
```

The bar is the full code, not the address: one address with several button
nibbles is an ordinary multi-button remote and is accepted without comment.
`allow_duplicate: true` creates the signal anyway — which is how you re-create a
code you have captured as a *synthesized* one, e.g. to check that the box can
generate a code it can otherwise only replay.

---

## Learn mode

The receiver always listens; learn mode only decides whether an *unrecognized*
burst is offered for registration.

### `GET /api/learn`
```json
{ "active": true, "remaining_s": 42,
  "candidate": { "fingerprint":"...", "base_us":292, "confidence":88,
                 "pulse_count":49, "repeats":4, "rssi_dbm":-31,
                 "decoded": { "protocol":"ev1527", "id":681562, "button":8,
                              "text":"EV1527 id=0xA685A btn=0x8" } } }
```
`candidate` is `null` until a qualifying burst arrives (needs `repeats >= 2` and
`confidence >= 65` — noise measured 24-28%, real presses 67-92%).

### `POST /api/learn/arm` — `{"timeout_s":60}` (optional)
### `POST /api/learn/cancel` — `{}`
### `POST /api/learn/accept` — `{"name":"Front door"}` → the created signal. 409 if no candidate.

---

## Node graph

### `GET /api/graph`
```json
{ "nodes": [ { "id":1, "type":"signal", "name":"Front door",
               "enabled":true, "signal_id":1,
               "gpio_pin":-1, "gpio_active_low":true, "gpio_debounce_ms":50,
               "repeats":6, "gap_us":8000,
               "window_s":10, "window_ms":10000, "group_mode":"any",
               "topic":"", "ui_x":40, "ui_y":40 } ],
  "links": [ {"from":1,"to":2} ] }
```
`type` is one of: `signal`, `source.gpio`, `source.virtual`,
`source.any_rf`, `logic.group`, `logic.throttle`, `logic.repeat`,
`sink.mqtt`.

**`signal` has BOTH ports** — it is one stored 433 MHz signal, and a signal is
not inherently an input or an output:

| direction | what it means |
|---|---|
| out | the node fires when `signal_id` is heard on air |
| in  | reaching the node over a link transmits `signal_id` (`repeats` copies, `gap_us` apart) |

Firing a signal node directly (`POST /api/graph/nodes/{id}/fire`) runs the
**output** side: it is "pretend that code was just heard", and it does not
transmit. It is also what an RF match does, which is why hearing a code never
makes the box echo it straight back out. To send one on demand use
`POST /api/signals/{id}/transmit`, or link something into the node's input.

`signal` replaces the former `source.button` (input only) and `sink.transmit`
(output only). **Stored graphs migrate automatically**: `signal` occupies the
enum slot `source.button` had, so every stored button node is already a signal
node, and a stored `sink.transmit` is retyped to `signal` on the first boot of
this firmware, keeping its id, name, `signal_id`, `repeats`, `gap_us`, canvas
position and every link. Neither wire name is accepted any more.

**`source.any_rf` is a wildcard**: it fires on EVERY received burst, including
ones matching no stored signal. Wire one to a `sink.mqtt` and Home Assistant
sees every press on the band — registered or not. It fires in addition to any
matching `signal` node, which is intended, not double-firing.
Irrelevant fields for a given type are present but ignored.

**A link from a node to itself is refused** (400): it is a cycle of one. A
longer cycle is accepted but walked only once — the engine enters each node at
most once per traversal and logs a `system` event when it stops.

**Time windows are in SECONDS** (`window_s`, 1–6000). `window_ms` is emitted
alongside it and still accepted on write, but `window_s` wins when both are sent.

**`logic.throttle` is a leading-edge throttle** (a cooldown / rate limit): the
first event passes straight through, then everything inside the window is
dropped. So a `window_s` of 10 means the chime rings on the first press and stays
quiet for 10 s however often the button is pressed. It limits whatever is linked
into it — RF remote, wired GPIO button or MQTT trigger alike.

**`logic.repeat` is the auto-repeat.** It passes the event on IMMEDIATELY and
then emits it again `repeats - 1` more times, `window_s` apart, each emission
carrying the original trigger unchanged. It uses two fields:

| field | meaning | range | default |
|---|---|---|---|
| `repeats` | total emissions, the immediate one included | 1–20 | 3 |
| `window_s` | interval between emissions | 1–6000 | 5 |

So `signal (front door) → logic.repeat (repeats 3, window_s 5) → signal (chime)`
rings the chime at 0 s, 5 s and 10 s from a single press. `repeats: 1` is a legal
pass-through that adds nothing. `repeats` is the same struct field a `signal`
node uses for its frame copies; on a repeat node it counts emissions instead, and
is capped at 20 rather than 32.

A new event arriving while a repeat is still running **restarts** it — it does
not stack, so five impatient presses do not queue fifteen rings. Deleting or
disabling the node, or changing its `repeats`, `window_s` or type, cancels the
emissions it still owes. Nothing survives a reboot.

Sixteen repeat sequences may run at once across the whole graph; beyond that the
immediate emission still happens and the repeats are dropped with a `system`
event. Wiring a repeat node back into itself is bounded too: the engine stops
such a chain after 8 laps and logs a `system` event.

**`topic` serves two node types.** On `sink.mqtt` it is published to as
`<base>/<topic>`. On `source.virtual` it is SUBSCRIBED to as
`<base>/trigger/<topic>` — any message there fires the node, which is how a
virtual input becomes reachable from Home Assistant or a shell one-liner with no
RF involved. Empty on a `source.virtual` means UI/REST triggering only.

**Combining several buttons into one virtual signal** needs no special node type:
link each `signal` node into a `logic.group` (mode `any` or `all`) and link
that to a `signal` node carrying the virtual signal.

### `POST /api/graph/nodes` — a node object without `id`; returns the created node.
### `POST /api/graph/nodes/{id}` — partial update.
### `DELETE /api/graph/nodes/{id}` — also removes its links.
### `POST /api/graph/nodes/{id}/fire` — test-fire (or trigger a `source.virtual`).
### `POST /api/graph/links` — `{"from":1,"to":2}`
### `DELETE /api/graph/links` — `{"from":1,"to":2}`

### `GET /api/gpio/available`
Which GPIOs the UI may offer for a `source.gpio` node — the wired-button feature
is optional and the pin is chosen here, not compiled in.
```json
{ "suggested": [6,7], "available": [1,2,3,6,7,8,9,14,15,16,17,18,21],
  "in_use": [4,5,10,11,12,13] }
```

---

## Events

### `GET /api/events?since=<serial>`
Newest first. `serial` lets the UI poll cheaply and skip re-rendering.
```json
{ "serial": 42, "events": [
  { "ts_s": 12, "kind": "button_press", "signal_id": 1, "node_id": 0,
    "rssi_dbm": -31, "repeats": 4, "text": "Front door" } ] }
```
`kind`: `rf_unmatched`, `button_press`, `wired_press`, `node_fired`,
`transmit`, `learn`, `system`.

### MQTT sink payloads carry the trigger
A `sink.mqtt` publishes what CAUSED it to fire, not merely that it fired:
```json
{ "signal_id": 1, "label": "Front door", "fingerprint": "5487745f",
  "rssi_dbm": -31, "repeats": 4,
  "decoded": { "protocol": "ev1527", "id": 681562, "button": 8 },
  "node": { "id": 7, "name": "Proxy to HA" }, "ts_s": 1756600000 }
```
`signal_id` is `0` and `decoded` may be `null` for an unrecognized burst — which
is the normal case behind a `source.any_rf` proxy.

---

## Configuration

### `GET /api/config` / `POST /api/config`
Non-secret config only; **passwords are never returned**, and an empty string in
a POST means "leave unchanged".

Write a secret with the key **`password`** — for `sta.networks[]` and for `mqtt`
alike, matching `POST /api/wifi`. (`pass` is accepted as an alias, because the
two endpoints once disagreed and the mismatch failed *silently*: the request
succeeded and the key was dropped.) Reads report only `has_pass`.
```json
{ "hostname":"doorbell",
  "sta": { "networks": [ {"ssid":"home","has_pass":true}, {"ssid":""}, {"ssid":""} ] },
  "mqtt": { "enabled":false, "host":"", "port":1883, "user":"",
            "base_topic":"doorbell", "homeassistant":true,
            "discovery_prefix":"homeassistant" },
  "ota": { "url":"" } }
```

### `GET /api/ap` / `POST /api/ap`
```json
{ "ssid":"Doorbell433", "security":2, "channel":6, "ip":"192.168.66.1",
  "enabled":true, "fallback_enabled":true, "has_recovery_pass":false }
```

### `GET /api/wifi/scan`
```json
{ "networks": [ {"ssid":"home","rssi":-52,"auth":3,"channel":6,"known":true} ] }
```

### `POST /api/wifi` — the recovery wizard's save
`{"slot":0,"ssid":"home","password":"..."}` → saves and reboots into normal mode.

---

## OTA

* `POST /api/ota` — `{"url":"https://.../doorbell433.bin"}`
* `POST /api/ota/webui` — `{"url":"https://.../storage.bin"}`
* `POST /api/ota/upload` — raw app `.bin` as the body
* `POST /api/ota/webui/upload` — raw SPIFFS `storage.bin` as the body

All reboot on success. The app image and the web UI are separate partitions: an
app OTA leaves the old UI in place until the UI is updated too.

---

## Update check

"Is there a newer release?" — asked of the GitHub Releases API of
`MarvAmBass/klingelbox`, compared against `esp_app_get_description()->version`.
Answered entirely from a RAM cache, so **no route here ever waits on GitHub**.

### `GET /api/update`
```json
{ "valid": true, "checking": false, "update_available": true,
  "current": "0.1.0", "latest": "v0.2.0", "published_at": "2026-08-30T09:12:44Z",
  "html_url": "https://github.com/MarvAmBass/klingelbox/releases/tag/v0.2.0",
  "app_url": "https://github.com/.../download/v0.2.0/klingelbox.bin",
  "webui_url": "https://github.com/.../download/v0.2.0/storage.bin",
  "error": "", "checked_at_s": 1180, "min_interval_s": 21600,
  "sta_connected": true }
```
* `valid` — a check has *completed* at least once. Everything but `current` is
  meaningless until it is true.
* `checking` — a fetch is in flight. Poll **this** endpoint until it clears;
  never poll `/api/update/check`.
* `error` — `""` when the last check succeeded. A failed check keeps the
  previous good result visible and only sets this.
* `checked_at_s` — device-uptime seconds, the same monotonic clock as an event's
  `ts_s`. `0` means never checked.
* `app_url` / `webui_url` — the `klingelbox.bin` and `storage.bin` release
  assets. Either may be `""` if that release did not publish one.

Versions are compared **numerically**, component by component, tolerating a
missing leading `v` and any `-rc` / `+build` suffix. `0.10.0` is newer than
`0.9.0`; a textual comparison gets that backwards.

### `POST /api/update/check` — `{"force":true}` (optional)
Starts an asynchronous re-query and returns the `GET /api/update` object
immediately (usually with `"checking": true`).

**503** when the STA is down — the box cannot reach the internet over its own
softAP alone.

GitHub allows 60 unauthenticated requests per hour per IP, so a check is refused
(and the cached answer returned, still `200`) when the previous one is younger
than `min_interval_s` — 6 hours. `force` shortens that to a 60 second floor. It
is never an error to ask too often; you simply get the cache.

### `POST /api/update/install` — `{"webui":true}` (optional)
Starts an OTA from the URL the check discovered, via the same machinery as
`POST /api/ota`. Returns `{"ok":true,"webui":false,"status":"..."}` and the box
reboots.

`webui` selects **which** image, it does not add a second one: only one update
of any kind may run at a time and each ends in a reboot. Install the firmware,
let the box come back, then install the web UI.

* **409** — no check has completed, no update is available, the release carries
  no asset of that kind, or another update is already running.
* **503** — the STA is not connected; use `POST /api/ota/upload` instead.
