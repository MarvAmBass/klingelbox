---
title: REST API
layout: default
nav_order: 6
---

# Klingelbox REST API

The contract between `firmware/main/http_api.c` and `firmware/webui/`. Both are
written against this document; neither may invent endpoints.

* Base: `http://<hostname>.local/` (also reachable on the softAP IP).
* No auth, no TLS — by design, a trusted-LAN / AP appliance.
* All responses are JSON. **Every failure is `{"error": "..."}`** with a real
  HTTP status (400 bad input, 404 unknown id, 409 wrong state, 503 no radio).
  `error` is always a **human sentence** — an `ESP_ERR_*` constant is never part
  of it. A few failures add machine-readable fields next to the sentence; those
  are documented at the endpoint that produces them.
* A response that is too large to build on the free heap is a **503** with an
  `error` sentence, never a silently empty body.
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

### `POST /api/signals/import`
Creates **one** signal from raw pulses. This is the restore half of backup /
restore (see [Backup bundles](#backup-bundles) below).

```json
{ "name": "Front door", "first_level": 1, "durations_us": [919,273,297],
  "origin": "captured" }
```

| field | required | meaning |
|---|---|---|
| `name` | yes | non-empty, trimmed, truncated to 32 bytes |
| `durations_us` | yes | 1..512 pulse widths, each 1..65535 µs, strictly alternating levels |
| `first_level` | no | level of `durations_us[0]`; 0 or 1, default 0 |
| `origin` | no | `captured` / `synthesized` / `imported`; anything else, or absent, becomes `imported` |

Returns the created signal, in exactly the shape `GET /api/signals/{id}` reports
it (without the waveform).

**Only the waveform crosses the wire.** `base_us`, `confidence`, `fingerprint`
and the decode are re-derived here from the pulses by the same code that
analyses a burst arriving off the air — so an imported signal is not merely
similar to a locally learned one, it is produced identically and matches
identically. A `fingerprint` in the request body is ignored; trusting a
hand-edited one would poison matching.

* **400** — bad name, missing/empty `durations_us`, more than 512 pulses, or a
  duration outside 1..65535 µs (the message names the offending index).
* **413** — body over 8192 bytes. This route takes one waveform, not a file.
* **507** — the signal store is full (32 signals).

---

## Backup bundles

Moving a box's learned signals and its node graph to another box is done **by
the client**, out of endpoints that already exist. There is deliberately **no
`GET /api/backup` and no `POST /api/backup`**.

**Why.** Free heap on the device is ~126 KB. A full backup of a filled store is
~86 KB of JSON (32 signals × ~2.7 KB of `durations_us`, plus the graph), and
cJSON needs the body string *and* a parse tree of two to three times the
document live simultaneously — roughly 300 KB, on a box that has 126 KB, while
Wi-Fi buffers and an open HTTP connection also hold heap. A whole-bundle
endpoint would not be slow; it would run the box out of memory. So the firmware
never holds the document, and the client streams it in and out one item at a
time. Please do not "simplify" this into a single endpoint.

**Export** — assemble client-side:

1. `GET /api/signals` for the list.
2. `GET /api/signals/{id}` per signal, for `first_level` + `durations_us`
   (already a chunked, streamed response for the same memory reason).
3. `GET /api/graph` for nodes and links.

**Import** — replay item by item, and **remap ids in this order**, because a
graph from another box references signal ids and node ids that will not exist
here:

1. `POST /api/signals/import` per signal → record `old signal id → new id`.
2. `POST /api/graph/nodes` per node, with `signal_id` rewritten through that map
   → record `old node id → new node id`.
3. `POST /api/graph/links` per link, with **both** `from` and `to` rewritten.

A node whose signal did not import must not be created carrying a dangling
`signal_id`. The web UI's importer creates it **unbound** (`signal_id: 0`,
`enabled: false`) and says so in the summary, rather than dropping the node and
silently losing its wiring.

An import is **not atomic** — it cannot be, given the above. A client must
report what actually got in.

### Bundle format

```json
{ "kind": "klingelbox-backup", "version": 1,
  "exported_at": 1756600000,
  "device": { "hostname": "klingelbox", "version": "0.1.0", "idf": "v5.3.1" },
  "radio": { "freq_hz": 433920000, "modulation": "ook", "datarate_bps": 5000,
             "bandwidth_hz": 203000, "tx_power_dbm": 10,
             "tx_repeats": 6, "tx_gap_us": 8000 },
  "signals": [ { "id": 1, "name": "Front door", "origin": "captured",
                 "first_level": 1, "durations_us": [919,273] } ],
  "graph": { "nodes": [ { "id": 1, "type": "signal.rx", "signal_id": 1, "...": "..." } ],
             "links": [ { "from": 1, "to": 2 } ] } }
```

`kind` and `version` are checked before anything is written: an unknown `kind`
or a `version` newer than the reader is refused with a sentence rather than
imported approximately. The `id` fields inside `signals` and `graph.nodes` are
the *source* box's ids and exist only so the remapping above can be performed;
they are never restored as-is.

`device` is descriptive only — it is shown to the user before an import and is
otherwise ignored.

`radio` is the settable subset of `GET /api/radio` — the live `rssi_dbm` reading
is a measurement, not a setting, and is left out. It is **optional on import**
and off by default in the UI: frequency, bandwidth, TX power and repeat counts
are device *behaviour*, not identity, and the receiving box may be a different
build with a different antenna. When accepted it is applied with one
`POST /api/radio`.

**A bundle carries no secrets.** No Wi-Fi SSID or passphrase, no AP password, no
MQTT host or credentials — not even hashes. A backup is a file that gets mailed
around and dropped in cloud folders; it holds what a doorbell *knows*, never
what it can *log in to*. Re-enter network and broker settings on the new box.

---

## Listening sessions

**There is no `/api/learn` any more.** It was removed, not renamed: it admitted
an unrecognized burst as a candidate only when it repeated at least twice *and*
normalized to at least 65 % confidence, and both numbers were measured on
EV1527-class remotes. That made it, in effect, an endpoint for one protocol
family — a transmitter of any other shape produced no candidate at all and no
explanation either, which is the opposite of what this box promises.

Registering a button now goes through a **listening session** under `/api/raw`,
which was already the permissive, protocol-agnostic recorder. Detection keeps
everything; what the box does instead is **rank**:

* the number of times a waveform was heard **dominates** the order, because a
  real remote repeats itself and band noise does not — and counting that
  requires no knowledge of any protocol;
* a decoded protocol and a high normalization confidence raise a candidate
  further, but only ever as a tie-break. **An undecoded candidate is
  first-class**: it can rank first, and it saves, replays and matches exactly
  like a decoded one.

Saving a candidate produces an ordinary stored signal with `origin: "captured"`,
indistinguishable from any other in `GET /api/signals`.

---

## Listening / raw sessions

A time-boxed recording of **everything the radio hears**, with the filters that
normally sit in front of the receiver relaxed. It exists because those filters
encode assumptions that are correct for EV1527-class remotes and may be wrong
for anything else, and a transmitter that violates one of them is not merely
mis-decoded — it is *invisible*, with nothing said about it anywhere.

| filter | normal path | raw session |
|---|---|---|
| RSSI squelch | −75 dBm, fixed | `rssi_floor_dbm`, default −80, `-120` = off |
| minimum frame length | 32 pulses | `min_pulses`, 2–64, default 4 |
| frame boundary (idle) | 8000 µs, fixed | `idle_us`, 1000–32000, default 8000 |
| burst coalescing | 250 ms window, fuzzy merge | none — every frame kept separately |

A session holds **32 frames in RAM, never flash**, and stops when the slots are
full or the time runs out, whichever comes first. Stopping does **not** free the
frames — you cannot trim what has been freed — so the memory goes back on
`DELETE /api/raw`, when the next session starts, or after 10 minutes with no
`GET /api/raw`.

While a session runs the box keeps working normally: only frames that would have
passed the *ordinary* thresholds are still routed to the node graph, so noise
recorded here can never fire a node.

`from` / `to` are **zero-based indices into `durations_us`**, `from` inclusive
and `to` exclusive (`Array.prototype.slice` semantics). `to: 0` means "to the
end". Levels alternate, so a slice starting on an odd index starts on the
opposite level; `first_level` is adjusted for you.

### `POST /api/raw/start` — all fields optional
```json
{ "seconds": 30, "idle_us": 8000, "min_pulses": 4, "rssi_floor_dbm": -80 }
```
Out-of-range values are clamped, not rejected. Returns the session state below.
**409** if a session is already running, **503** with no radio, and **503** with
a sentence about memory if the ~36 KB of frame slots cannot be allocated (in
which case nothing is allocated and capture is left exactly as it was).

### `POST /api/raw/stop` — `{}` → the session state. Stops recording, keeps the frames.
### `DELETE /api/raw` — `{}` → `{"ok":true}`. Stops if needed, then frees the frames.

### `GET /api/raw`
```json
{ "running": true, "held": true, "elapsed_s": 3, "remaining_s": 27,
  "count": 2, "capacity": 32, "stop_reason": "",
  "settings": { "seconds": 30, "idle_us": 8000, "min_pulses": 4,
                "rssi_floor_dbm": -80, "squelch_off": false },
  "dropped": { "below_floor": 118, "too_short": 39, "too_long": 0,
               "overruns": 0, "no_room": 0 },
  "radio": { "heard": 2, "carrier_seen": true, "peak_rssi_dbm": -41,
             "quiet_rssi_dbm": -85, "present": true },
  "fragmentation": { "detected": true, "runs": 3, "frames": 6, "rejoined": 1,
                     "max_gap_us": 11800, "suggest_idle_us": 36000 },
  "candidates_total": 12,
  "candidates": [ { "id": 1, "seen": 3, "merged": false, "pulse_count": 49,
                    "airtime_us": 24310, "base_us": 292, "confidence": 88,
                    "rssi_dbm": -41, "score": 3714, "age_s": 1.2,
                    "truncated": false, "frames": [1,3,5],
                    "why": "seen 3 times, decoded ev1527, 88% confidence",
                    "decoded": { "protocol":"ev1527", "id":681562, "button":8,
                                 "text":"EV1527 id=0xA685A btn=0x8" } } ],
  "limits": { "seconds_min": 5, "seconds_max": 300, "idle_us_min": 1000,
              "idle_us_max": 32000, "min_pulses_min": 2, "min_pulses_max": 64,
              "rssi_off_dbm": -120, "max_pulses": 512, "normal_squelch_dbm": -75 },
  "frames": [ { "index": 1, "ts_s": 16, "age_s": 3.29, "pulse_count": 49,
                "rssi_dbm": -41, "airtime_us": 24310, "base_us": 292,
                "confidence": 88, "truncated": false,
                "decoded": { "protocol":"ev1527", "id":681562, "button":8,
                             "text":"EV1527 id=0xA685A btn=0x8" } } ] }
```
`held` is true whenever frames are in memory, running or not. `stop_reason` is
`""`, `"full"`, `"time"`, `"user"` or `"radio"`.

**The `dropped` object is the diagnostic**, and it is split by cause on purpose:
"nothing was received" and "something was received but did not fit our
assumptions" are completely different faults and only the second one is fixable
by trimming. `too_long` counts frames that hit the 512-pulse ceiling and were
therefore thrown away whole — a truncated recording would replay as a different
waveform. `radio.carrier_seen` plus `peak_rssi_dbm` answer "did the radio see
*any* energy", which is what separates a wrong frequency or a missing antenna
from a wrong threshold. `peak_rssi_dbm`/`quiet_rssi_dbm` are `null` until the
band has been sampled at least once.

`decoded` is `null` for an unknown protocol — the ordinary, fully supported
state. A raw session changes what *reaches* the decoders, never what they do.

`index` is 1-based and stable for the life of the session.

### Candidates — the ranked list

`candidates` is the list a UI should build its screen around; `frames` is the
unranked raw material underneath it, kept so grouping can never hide anything.

`GET /api/raw` embeds only the **top 10**, because a full session already holds
~43 KB and the state + every candidate + every frame will not fit beside it.
`candidates_total` is the real count, so a truncated list is never mistaken for
a complete one. `GET /api/raw/candidates` returns all of them (with no frame
array), and that is the endpoint to use when the count matters.

Every frame becomes a candidate. Frames that are the same waveform repeated are
collapsed into one candidate with `seen` counting the repeats. **Nothing is
filtered**: a candidate seen once, with `confidence: 0` and `decoded: null`, is
still listed and still fully usable.

| field | meaning |
|---|---|
| `id` | 1-based **rank**. Stable only while no new frame arrives, i.e. after the session stops. |
| `seen` | how many times this waveform was heard — the dominant ranking term |
| `merged` | true when this was assembled from fragments rather than heard whole |
| `frames` | the frame `index` values it is built from (one, unless `merged`) |
| `gaps_us` | present when `merged`: the **measured** silence before each piece |
| `score` | the ranking number. Exposed for debugging; `why` is what to show a human. |
| `why` | e.g. `"seen 5 times, decoded ev1527, 92% confidence"` |
| `decoded` | `null` for an unknown protocol, which is an ordinary supported state |

The weights guarantee that **one extra repeat outranks every decode and
confidence bonus combined**, so a candidate no decoder understands legitimately
sits above a pristine decoded one that was heard fewer times.

### Fragmentation — a diagnosis, not a statistic

A frame ends after `idle_us` of silence. Set that shorter than a transmitter's
own inter-word gap and **one press arrives as several dissimilar pieces**, none
of which replays the whole thing. `fragmentation.detected` says that happened.
The test is deliberately narrow: the silence between two frames was no more than
twice `idle_us` (so the threshold cut it rather than the transmitter stopping)
**and** the two frames are not similar to each other (identical neighbours are
honest repeats, not fragments).

`suggest_idle_us` is what `idle_us` should become — derived from the widest gap
actually measured, not from a table — and is `null` when there is nothing to
suggest. `rejoined` counts the runs that were stitched back into a candidate of
their own, using the measured gaps rather than an invented one; those appear in
`candidates` with `merged: true`.

### `GET /api/raw/candidates`
The **complete** ranked list on its own, for a client that does not want the
frame array — and the endpoint to use when a session is full:
```json
{ "running": false, "count": 6, "candidates_total": 12, "candidates": [ ... ] }
```

### `GET /api/raw/candidates/{n}`
One candidate as above, plus its waveform (streamed):
```json
{ "id": 1, "...": "...", "first_level": 0, "durations_us": [919,273,297,...] }
```
For a `merged` candidate this is the **stitched-together whole**, so what you
inspect is what `/transmit` sends and what `/save` stores. **404** if there is no
such candidate; **409** if it could not be assembled.

### `POST /api/raw/candidates/{n}/transmit`
### `POST /api/raw/candidates/{n}/save`
Identical bodies, semantics and status codes to the `/api/raw/{i}` forms below,
operating on the candidate's waveform instead of one recorded frame.

### `GET /api/raw/{i}`
One frame as above, plus its waveform (streamed, like `GET /api/signals/{id}`):
```json
{ "index": 1, "...": "...", "first_level": 0, "durations_us": [919,273,297,...] }
```
**404** if there is no such frame in the current session.

### `POST /api/raw/{i}/transmit` — `{"from":0,"to":50,"repeats":6,"gap_us":8000}` (all optional)
Replays the frame, or the selected pulses of it, **without storing anything** —
the fast loop for "does this actually trigger the bell?". Same `gap_us`
semantics and the same software-level-success caveat as
`POST /api/signals/{id}/transmit`. **503** with no radio, **404** for an unknown
frame or an empty selection.

### `POST /api/raw/{i}/save` — `{"name":"Front door","from":0,"to":50}`
Stores the (optionally trimmed) frame as an ordinary signal with
`origin: "captured"`, and returns it in the `GET /api/signals` shape. The store
re-analyses it exactly as it would any registration, so a trim that turns out to
be a known protocol acquires its decoded identity at this point. **400** without
a name, **404** for an unknown frame or an empty selection, **409** when the
signal store is full.

---

## Node graph

### `GET /api/graph`
```json
{ "nodes": [ { "id":1, "type":"signal.rx", "name":"Front door",
               "enabled":true, "signal_id":1,
               "gpio_pin":-1, "gpio_active_low":true, "gpio_debounce_ms":50,
               "repeats":6, "gap_us":8000,
               "window_s":10, "window_ms":10000, "group_mode":"any",
               "topic":"", "mqtt_enabled":true, "ui_x":40, "ui_y":40 } ],
  "links": [ {"from":1,"to":2} ] }
```
`type` is one of: `signal.rx`, `signal.tx`, `source.gpio`, `source.virtual`,
`source.any_rf`, `logic.group`, `logic.throttle`, `logic.repeat`,
`logic.switch`, `sink.mqtt`, `sink.monitor`.

#### `signal.rx` and `signal.tx` — one code, one direction each

A 433 MHz signal can be received or sent, so it gets **two node types** over the
same `signal_id` pool. Each has one port and one job:

| type | ports | what it does |
|---|---|---|
| `signal.rx` | out only | fires when `signal_id` is heard on air |
| `signal.tx` | in only | transmits `signal_id` when reached (`repeats` copies, `gap_us` apart) |

A signal you both listen for and send is **two nodes**, and a relay is written
down as `signal.rx (A) → … → signal.tx (B)` — which reads left to right like
every other chain.

`POST /api/graph/nodes/{id}/fire` simply starts a traversal at the node, so it
does whatever that node does:

| firing a | does |
|---|---|
| `signal.rx` | "pretend this code was just heard" — its output fires, **nothing is transmitted** |
| `signal.tx` | transmits, exactly as an inbound link would have |

An RF match starts `signal.rx` nodes only. A `signal.tx` bound to the same code
is never started by hearing it, which is why the box cannot echo a code back out:
the node that heard it has no send side to reach.

`repeats` and `gap_us` are read on `signal.tx` alone; they are still present on
an `rx` node's JSON (every field is, see below) and ignored there.

A third type reads `signal_id` from the same pool: on `logic.switch` it is an
optional **control** signal that toggles the switch when it is heard — see
`logic.switch` below.

**Stored graphs migrate automatically** (nodes blob v2 → v3). `signal.rx` keeps
the enum slot the old two-ported `signal` type had, so a stored node is already
an `rx` node, and the migration only decides which ones were being used as
senders — by the links attached to them:

| the stored `signal` node | becomes |
|---|---|
| has an outgoing link | `signal.rx` |
| has only incoming links | `signal.tx` |
| has no links at all | `signal.rx` |
| has **both** directions | `signal.rx`, plus a logged warning and a `system` event naming the node |

Nodes keep their id, name, `signal_id`, `repeats`, `gap_us`, canvas position and
every link; **nothing is created and no link is rewired**. The both-directions
case is the one that genuinely needs a second node now, and the box reports it
rather than inventing one — add a `signal.tx` for that code and move the inbound
links onto it. The wire name `signal` is no longer accepted, and neither are the
older `source.button` and `sink.transmit`.

**`source.any_rf` is a wildcard**: it fires on EVERY received burst, including
ones matching no stored signal. Wire one to a `sink.mqtt` and Home Assistant
sees every press on the band — registered or not. It fires in addition to any
matching `signal.rx` node, which is intended, not double-firing.
Irrelevant fields for a given type are present but ignored.

**A link from a node to itself is refused** (400): it is a cycle of one. A
longer cycle is accepted but walked only once — the engine enters each node at
most once per traversal and logs a `system` event when it stops.

**Time windows are in SECONDS** (`window_s`, 1–6000). `window_ms` is emitted
alongside it and still accepted on write, but `window_s` wins when both are sent.

**`logic.group` has two modes that do genuinely different things**, and only one
of them uses `window_s`:

| `group_mode` | behaviour | uses `window_s` |
|---|---|---|
| `any` | A **merge point**. Anything arriving is passed straight on, immediately. | no |
| `all` | A **coincidence detector**. Nothing passes until EVERY inbound link has carried an event inside the window; it then fires once, forgets them all and re-arms. | yes |

`any` does not wait, does not compare and never consults the clock — sending a
`window_s` with it is accepted and stored, but changes nothing. Its value is
structural: several sources meet at one node, so the chain hanging off it is
wired and edited in one place. An `all` group with no inbound links can never be
satisfied and never fires.

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

So `signal.rx (front door) → logic.repeat (repeats 3, window_s 5) → signal.tx
(chime)` rings the chime at 0 s, 5 s and 10 s from a single press. `repeats: 1`
is a legal pass-through that adds nothing. `repeats` is the same struct field a
`signal.tx` node uses for its frame copies; on a repeat node it counts emissions
instead, and is capped at 20 rather than 32.

A new event arriving while a repeat is still running **restarts** it — it does
not stack, so five impatient presses do not queue fifteen rings. Deleting or
disabling the node, or changing its `repeats`, `window_s` or type, cancels the
emissions it still owes. Nothing survives a reboot.

Sixteen repeat sequences may run at once across the whole graph; beyond that the
immediate emission still happens and the repeats are dropped with a `system`
event. Wiring a repeat node back into itself is bounded too: the engine stops
such a chain after 8 laps and logs a `system` event.

**`logic.switch` is a switch in the wire.** Every other type is a source, a
transform or a sink; this one sits IN a link and decides whether it conducts.
While it is ON an event passes through untouched; while it is OFF nothing gets
past it, and everything wired after it is dead until it is switched back on. The
nodes and links all stay exactly where they are — that is the whole point, since
the only previous way to stop a path was to delete the link.

**Its position IS `enabled`.** Not a second field beside it: the engine already
skips a disabled node when it walks the graph, and "the walk does not enter it"
is exactly "the wire does not conduct", so the two are one idea. Read the
position out of `enabled` in `GET /api/graph`; move it with the route below.

```
POST /api/graph/nodes/{id}/switch   {"on": false}   → the updated node object
```
`{"enabled": false}` is accepted as a synonym for `{"on": false}`. `409` if the
node is not a `logic.switch`.

Posting `{"enabled": false}` to the ordinary `POST /api/graph/nodes/{id}` moves
the same flag and is not an error — but it **persists synchronously**, the way
every graph mutation does. The `/switch` route does not: it puts the change in
RAM immediately (so the very next traversal sees it) and lets the box write the
node blob back once the position has been stable for ~10 s, and at most once
every 2 minutes. That is deliberate. The graph blob is rewritten in full on every
save, and this is the one control a home-automation rule may flip as often as it
likes; a write per toggle would be a flash-wear bug. Use `/switch` from anything
automated. The cost is bounded staleness — a power cut seconds after a toggle
comes back in the previous position — and the box always republishes its
**actual** position retained on connect, so no subscriber is left believing a
switch it cannot see.

**`signal_id` on a `logic.switch` is a CONTROL input.** Optional, `0` by default,
and nothing to do with the switch's input port — the port is a *data* input,
carrying events that pass *through* the switch while it is on. Set `signal_id` to
a stored signal and **every time that code is recognised on air, the switch
toggles**. One button on a fob becomes a physical on/off for a path: press it by
the door and the outside chime stops ringing; press it again and it rings.

```
POST /api/graph/nodes/9   {"signal_id": 4}    → this switch now toggles on signal 4
POST /api/graph/nodes/9   {"signal_id": 0}    → back to MQTT/UI/REST only
```

| | |
|---|---|
| `signal_id: 0` | unchanged behaviour: only MQTT, the UI and REST move it |
| `signal_id: N` | that code, heard on air, **toggles** the switch |

It is a **toggle**, not a set — one button, both directions. Forcing a specific
position from two different buttons would need a mode field and there is no free
one; see the note at the end of this section.

Six things follow from it being a control action rather than a traversal:

* **Toggling injects nothing into the switch's outputs.** The wire changes state;
  no event travels down it. A remote that both flipped a switch and rang through
  it would be indistinguishable from a broken switch.
* **The move goes through the same path as every other switch move**, so the
  retained `<base>/switch/<topic>/state` is republished immediately and Home
  Assistant follows — and the deferred write applies, so a fob hammered by a
  child costs no more flash than an automation flapping the switch does.
* **One press is one toggle.** A remote repeats its frame several times per
  press; `rf_service.c` coalesces those into a single burst before the graph sees
  anything, so a press arrives once (with `repeats` counting the copies).
* **If the same signal also drives a `signal.rx` node, both happen** — the rx
  node fires its chain *and* the switch flips. That is correct, and it is the
  same thing `source.any_rf` already does alongside a matching `signal.rx`. In
  the log it looks like double-firing and is not.
* **`POST /api/graph/nodes/{id}/fire` on a `signal.rx` node also toggles it.**
  Firing a receiver means "pretend this code was just heard", so it means the
  whole of it. This is how the feature is tested with no transmitter. Firing any
  other node type moves no switch.
* **It works with MQTT off.** Neither `mqtt_enabled` nor the global MQTT setting
  gates it — it is local behaviour between the radio and the graph.

The switch's own position is **not** a gate: a switch that is OFF still reacts,
which is what makes it a toggle rather than a one-way off button. (On this type
`enabled` *is* the position, so there is no separate "disabled" state to honour —
the UI offers a `logic.switch` no Enabled checkbox at all.)

No new field was added for this: `signal_id` exists on every node and was unused
on this type. Stored graphs are unaffected and no blob version changed.

**`topic` serves three node types.** On `sink.mqtt` it is published to as
`<base>/<topic>`. On `source.virtual` it is SUBSCRIBED to as
`<base>/trigger/<topic>` — any message there fires the node, which is how a
virtual input becomes reachable from Home Assistant or a shell one-liner with no
RF involved. On `logic.switch` it names a pair of topics:

| topic | direction | payload |
|---|---|---|
| `<base>/switch/<topic>/set` | subscribed | `ON` `OFF` `1` `0` `true` `false` `open` `close`, any case; or `{"state":"ON"}` / `{"on":true}` / `{"value":1}` |
| `<base>/switch/<topic>/state` | published, **retained**, QoS 1 | `ON` or `OFF` |

The state is published when the switch moves, on every broker connect, and after
a reboot, so Home Assistant never shows a position the box is not in. With
discovery enabled the topic also becomes a native HA **`switch`** entity on the
shared Klingelbox device — a real toggle, not a template or a button pair.

**The topic a switch answers on** is its `topic` if it has one, otherwise a slug
of its `name` (`All Bells Switch` → `all_bells_switch`), otherwise nothing. One
resolver decides this for the subscription, the Home Assistant entity, the
routing of an arriving `set` and the reported state alike — they are the same
question, and when the routing side asked it differently a switch relying on the
name fallback got an entity that could not be commanded and never published a
state.

**Several `logic.switch` nodes may share one `topic`**, and that is a feature:
one Home Assistant toggle then gates several paths at once. Two nodes sharing a
NAME share a toggle for the same reason, since they resolve to the same suffix. A
`set` command moves every node that resolves to the suffix; the state reported for the topic is `ON` if
**any** of them is conducting (an all-of rule would report `OFF` while a path
still rang). Discovery announces one entity **per distinct topic**, not per node
— two toggles that always move together and command each other would read as a
bug — named after the first node on the topic, with `(N paths)` appended when
several share it.

Empty on a `source.virtual` means UI/REST only: the node still works, nothing
subscribes to it and no HA entity appears. Empty on a `logic.switch` does **not**
mean that — it falls back to a slug of the node's `name`, so a blank topic still
produces a topic and an entity. Use `mqtt_enabled` to keep a switch off MQTT.

#### `mqtt_enabled` — is this node visible outside the box?

Boolean, **`true` by default**, on every node type. Absent from a write means
"leave it as it is"; a node created without it is exposed, like everything that
existed before the field did.

With it `false` the MQTT bridge behaves as though the node were not in the graph:

| node type | what stops |
|---|---|
| `source.virtual` | no `<base>/trigger/<topic>` subscription, no HA button entity |
| `logic.switch` | no `<base>/switch/<topic>/set` subscription, no HA switch entity, no retained state, and a `set` command no longer moves it even when another node shares the topic |
| `sink.mqtt` | publishes nothing — not on `<base>/<topic>`, and not into the `<base>/event` stream either |

**Anything already announced is CLEARED, not orphaned.** Turning it off publishes
an empty retained payload over the node's discovery config — and, for a switch
topic no exposed node carries any more, over its retained `state` as well — so
Home Assistant removes the entity instead of showing it permanently unavailable.
This is the same path a deleted node takes.

**Nothing inside the graph changes.** A switch with `mqtt_enabled: false` still
gates its wire, a `source.virtual` still fires from
`POST /api/graph/nodes/{id}/fire`, and a `sink.mqtt` is still reached by a
traversal. The flag answers one question only: can the outside world see it.

It exists because a blank `topic` stopped being a way to say "no MQTT" once
`logic.switch` gained the name fallback. A sentinel topic value was considered
and rejected — `-` is a perfectly legal MQTT topic level, so any magic string
collides with something a user could legitimately want.

#### Topic validation

Every topic a user can type is checked against **one rule**, and a bad value is
refused with `400 Bad Request` naming both the field and the offending character
rather than being stored or silently repaired. It applies to `topic` on
`POST /api/graph/nodes` and `POST /api/graph/nodes/{id}` (create and update
alike), and to `mqtt.base_topic` and `mqtt.discovery_prefix` on
`POST /api/config`.

| refused | why |
|---|---|
| `#` or `+` anywhere | MQTT wildcards. Publishing to a topic containing one is illegal: the broker refuses the message or drops the connection. In `base_topic` that takes the entire bridge down, not one entity. |
| control characters, DEL, any byte >= 0x80 | Not printable, so not debuggable. A newline pasted out of a config file is the usual way one arrives. |
| a leading or trailing `/` | Legal MQTT, but it means an empty first or last topic level and here it is always a mistake. The box supplies the separators itself. |
| an empty level (`a//b`) | Same reasoning. Refused rather than silently producing a topic nobody can read. |
| longer than 47 characters | The field stores 48 bytes including the terminator. Checked before truncation, so an over-long value is refused rather than quietly cut short. |

An **empty** value is always accepted: emptiness is the caller's business, not a
syntax question. On a node it means "no topic"; on `base_topic` and
`discovery_prefix` it means "use the default" (`klingelbox` and `homeassistant`),
which is the behaviour those fields have always had.

Values are trimmed of surrounding whitespace before both validation and storage,
so the string that is checked is the string that is stored. The web UI applies
the identical rule as you type, so the same message appears before anything is
sent.

```
$ curl -sX POST http://klingelbox.local/api/graph/nodes/3 -d '{"topic":"a/#"}'
{"error":"\"topic\" contains '#', which is an MQTT wildcard. A message cannot be
published to a topic containing '#' or '+' — the broker refuses it."}
```

**`sink.monitor` acts on nothing.** It is a visualizer: reaching it records a
timestamp and that is all — no transmit, no publish, no GPIO. Drop one anywhere
in a chain to see that the chain fires, and read the hits back from
`GET /api/monitor`.

| field | meaning | range | default |
|---|---|---|---|
| `window_s` | how long the UI's indicator stays lit per hit | 1–60 | 3 |

It reuses `window_s` rather than adding a field, the same way `logic.repeat`
reuses `repeats`. Its hits live in RAM only and are **never written to flash** —
this is debug telemetry, and persisting a press every time someone rings the
bell would wear NVS out for data nobody reads a minute later. They are dropped
on delete, on disable, on a retype and on reboot. At most 64 hits are kept per
node, and anything older than the 600 s retention window is pruned as new hits
arrive, so a busy band cannot flush the history in seconds. At most 8 monitor
nodes hold a hit ring at once; a ninth is logged and simply records nothing.

**Combining several buttons into one virtual signal** needs no special node type:
link each `signal.rx` node into a `logic.group` (mode `any` or `all`) and link
that to a `signal.tx` node carrying the virtual signal.

### `POST /api/graph/nodes` — a node object without `id`; returns the created node.
### `POST /api/graph/nodes/{id}` — partial update.
### `DELETE /api/graph/nodes/{id}` — also removes its links.
### `POST /api/graph/nodes/{id}/fire` — test-fire (or trigger a `source.virtual`).
### `POST /api/graph/nodes/{id}/switch` — `{"on":true}` on a `logic.switch`; see above.
### `POST /api/graph/links` — `{"from":1,"to":2}`
### `DELETE /api/graph/links` — `{"from":1,"to":2}`

### `GET /api/monitor`
Every `sink.monitor` node and when it recently fired, in **one call** however
many exist — a client polls this once per tick regardless of how many monitors
are on the canvas.
```json
{ "now_s": 1234,
  "nodes": [ { "id": 7, "name": "Front door watch", "hold_s": 3,
               "retention_s": 600, "hits": [1231, 1180, 1104] } ] }
```
`hits` are **device-uptime seconds, newest first**, on the same clock as
`now_s` — so an age is `now_s - hit` and no wall-clock sync is needed. That is
deliberate: the box may have no time source at all, and an epoch timestamp
would be a lie on a cold boot. `hold_s` is the node's `window_s`, clamped to
1–60. `retention_s` is how far back `hits` can reach (600 s).

`nodes` is `[]` when no monitor node exists — which is how a client tells "this
firmware has no monitors" (404) apart from "none has been added" (200).

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
`transmit`, `learn`, `system`. (`learn` is a wire name kept for compatibility;
it means "a signal was registered".)

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
  "ota": { "url":"https://github.com/MarvAmBass/klingelbox/releases/latest/download/klingelbox.bin",
           "default_url":"https://github.com/MarvAmBass/klingelbox/releases/latest/download/klingelbox.bin",
           "default_webui_url":"https://github.com/MarvAmBass/klingelbox/releases/latest/download/storage.bin" } }
```

`mqtt.base_topic` and `mqtt.discovery_prefix` are validated on write by the same
rule as a node's `topic` — see [Topic validation](#topic-validation). A bad value
is refused with `400 Bad Request` naming the field, and **nothing else in the
request body is applied**: this handler is checked before it mutates anything,
so a rejected topic cannot leave the Wi-Fi half of a POST saved and the MQTT half
not. Both may be empty, which means "use the default" as it always has.

`ota.url` is the app image URL this box will use for a manual update; it is
writable and defaults to the stable release asset, so nobody has to type a GitHub
path from memory. `default_url` and `default_webui_url` are **read-only** and are
what the web UI prefills the two manual-update fields with. They point at
`releases/latest/download/<asset>`, which GitHub redirects to the newest
release's asset, so they never carry a version.

A fork changes all of this in **one place**: `DB_UPDATE_REPO_SLUG` in
`main/update_check.h`, which the update checker, the stored default and these two
fields are all derived from. (The web UI carries one matching constant of its
own, used only as a fallback against a firmware too old to serve these fields.)

None of it is used by the **automatic** check or install below: those read the
exact asset URLs out of the release document they just fetched.

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

* `POST /api/ota` — `{"url":"https://.../klingelbox.bin"}`
* `POST /api/ota/webui` — `{"url":"https://.../storage.bin"}`
* `POST /api/ota/upload` — raw app `.bin` as the body
* `POST /api/ota/webui/upload` — raw SPIFFS `storage.bin` as the body

All reboot on success. The app image and the web UI are separate partitions: an
app OTA leaves the old UI in place until the UI is updated too.

`url` may be omitted on either URL route. `POST /api/ota` then uses the stored
`ota.url` (what this box was last told to use, so a custom build is never
silently replaced by upstream's) and falls back to the built-in default;
`POST /api/ota/webui`, which stores nothing, goes straight to its default. Both
defaults are also served by `GET /api/config` so a client can prefill the fields.
They are for this manual path only — the automatic install below never needs
them, and reads the exact asset URLs out of the release it fetched.

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
