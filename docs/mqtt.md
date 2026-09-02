---
title: MQTT & Home Assistant
layout: default
nav_order: 5
---

# MQTT & Home Assistant
{: .no_toc }

Every press, in both directions, on topics a human can type.
{: .fs-5 .fw-300 }

<details open markdown="block">
  <summary>On this page</summary>
  {: .text-delta }
- TOC
{:toc}
</details>

---

## Setup

MQTT is off by default. Turn it on with `POST /api/config`, or in the web UI:

```json
{ "mqtt": { "enabled": true, "host": "192.168.1.10", "port": 1883,
            "user": "klingelbox", "password": "…",
            "base_topic": "klingelbox",
            "homeassistant": true, "discovery_prefix": "homeassistant" } }
```

| Setting | Default | Notes |
|---|---|---|
| `host` / `port` | — / `1883` | No TLS. Trusted-LAN appliance. |
| `user` / `password` | empty | Optional. Passwords are never read back — `GET /api/config` reports only `has_pass`. |
| `base_topic` | `klingelbox` | The `<base>` in every topic below. |
| `homeassistant` | `true` | Publish MQTT discovery. |
| `discovery_prefix` | `homeassistant` | Only change it if your HA install did. |

An empty string in a `POST` means "leave unchanged", so you can update the host without
re-sending the password.

{: .note }
> **`base_topic` and `discovery_prefix` are validated on write.** They are the prefixes of
> every topic the box publishes and every topic Home Assistant reads, so a `#` or a `+` in
> either does not break one entity — it takes the whole bridge down, because publishing to
> a topic containing an MQTT wildcard is illegal and the broker refuses the message. The
> same rule rejects control characters, a leading or trailing `/`, an empty level (`a//b`)
> and anything over 47 characters, and the error names both the field and the offending
> character. Node topics are checked by the identical rule — see
> [API.md](API.md#topic-validation). Leaving either empty is still fine and still means
> "use the default"; the whole request is refused before anything is applied, so a bad
> topic never leaves half a form saved.

{: .note }
> Write the broker password under the key **`password`**. `pass` is accepted as an alias,
> because the two config endpoints once disagreed and the mismatch failed *silently* — the
> request succeeded and the key was quietly dropped.

## Topic map

`<base>` is `base_topic`, `klingelbox` by default. `<slug>` is a slug of the signal's name
— `Front door` becomes `front_door`.

| Topic | Direction | Retained | Payload |
|---|---|---|---|
| `<base>/status` | publish | **yes** | `online` / `offline`. Also the Last-Will. |
| `<base>/button/<slug>/state` | publish | no | One recognised press. [Trigger JSON](#the-trigger-payload). |
| `<base>/button/<slug>/press` | **subscribe** | — | Any message transmits that signal. |
| `<base>/trigger/<suffix>` | **subscribe** | — | Any message fires **every** MQTT button (`source.virtual`) answering on that suffix. The payload is never inspected. |
| `<base>/unknown/state` | publish | no | An **unregistered** burst. Trigger JSON. |
| `<base>/unknown` | publish | **yes** | The last unregistered burst, so you can look it up later. |
| `<base>/event` | publish | no | Every node firing, plus system events. |
| `<base>/<suffix>` | publish | no | A `sink.mqtt` node's own topic, when it has one set. |
| `<base>/switch/<suffix>/set` | **subscribe** | — | Moves every `logic.switch` node answering on that suffix. `ON` `OFF` `1` `0` `true` `false` `open` `close`, any case, or a JSON object. |
| `<base>/switch/<suffix>/state` | publish | **yes** | `ON` or `OFF` — the position of that suffix. |
| `<base>/radio` | publish | **yes** | [Radio telemetry](#radio-telemetry). |

{: .note }
> **Which suffix does a Switch answer on?** Its **Topic** if you typed one, otherwise a slug
> of its **name** — a switch called "All Bells Switch" with an empty topic answers on
> `all_bells_switch`. The same rule decides the subscription, the Home Assistant entity, the
> routing of an arriving `set` and the retained state, because they are all the same
> question. They have not always agreed: a switch relying on the name fallback used to get
> an entity that no command could reach and that never published a state. If you have one
> that looked broken in Home Assistant, this is why, and it is fixed.

### Why presses are not retained and telemetry is

Retention answers one question: *what should a subscriber that just connected be told?*

For radio telemetry the honest answer is "the last reading". For a doorbell press it is
**nothing** — a press is a moment, not a condition. A retained press payload would ring
every chime in the house every time Home Assistant restarts.

The one exception is `<base>/unknown` (retained) alongside `<base>/unknown/state` (not).
The unretained topic is the event; the retained one exists so you can *see* the code of the
remote you are about to register, even hours later.

### Slugs, not ids

Topics are addressed by a slug of the signal name, not its numeric id, because
`klingelbox/button/front_door/press` is a topic you can type into `mosquitto_pub` and an
automation someone can still read in six months.

Names are not unique, and slugging collapses more of them together (`Front door` and
`front-door!` both become `front_door`), so collisions are resolved deterministically with
an `_<id>` suffix. Home Assistant `unique_id`s, by contrast, are keyed on the **numeric
signal id**, so renaming a signal renames the entity instead of orphaning it and creating a
second one.

## The trigger payload

Every press payload says **what caused it**, not merely that something happened:

```json
{
  "signal_id": 1,
  "label": "Front door",
  "fingerprint": "5487745f",
  "rssi_dbm": -31,
  "repeats": 4,
  "decoded": { "protocol": "ev1527", "id": 681562, "button": 8 },
  "node": { "id": 7, "name": "Proxy to HA" },
  "ts_s": 1756600000
}
```

| Field | Notes |
|---|---|
| `signal_id` | `0` for a burst matching no stored signal. Normal behind a `source.any_rf` proxy. |
| `label` | Signal name, node name, or `"unknown"`. |
| `fingerprint` | Identity of the waveform. This is what identifies an *undecodable* remote. |
| `rssi_dbm` | Signal strength. Real presses land around −24 to −42 dBm; noise, −91 to −97. |
| `repeats` | How many copies of the frame arrived in the burst. |
| `decoded` | `null` when no decoder recognised it — a supported state, not an error. |
| `node` | The graph node that published this, or `null`. |

## Radio telemetry

`<base>/radio`, retained, refreshed every 10 seconds:

```json
{ "present": true, "rssi_dbm": -96, "signals": 4,
  "last_press": "Front door", "last_press_id": 1,
  "last_press_rssi_dbm": -31, "last_press_ago_s": 42 }
```

This is the diagnostic that answers "why did nothing ring?", which on a 433 MHz box is
almost always one of two things: **no radio** (`present: false` — check the wiring) or
**too much noise** (an `rssi_dbm` that sits far above the −96 dBm-ish floor means something
nearby is transmitting continuously and drowning your remote).

## Home Assistant discovery

With `homeassistant: true`, the box announces itself under `<discovery_prefix>/…`. Every
payload carries the same `device` block keyed on identifiers derived from the **MAC
address** — not the hostname, because the hostname is user-editable and renaming the box
must move the device, not fork it. So everything lands as **one HA device** with one page
and one availability state.

### What appears

| Entity | Type | For |
|---|---|---|
| *(per signal)* "Front door pressed" | **device trigger** | Receiving. Fires on each press. |
| *(per signal)* "Front door" | `button` | Transmitting. Pressing it replays the signal. |
| *(per MQTT button topic)* | `button` | Pressing an MQTT button (`source.virtual`) from the dashboard. One per topic, not one per node. |
| "Unregistered remote pressed" | **device trigger** | Any burst matching no stored signal. |
| "Last unknown code" | `sensor` (diagnostic) | The fingerprint of the last unknown burst, with decode and RSSI as attributes. |
| "Radio RSSI" | `sensor` (diagnostic) | Live noise floor, dBm. |
| "Radio" | `binary_sensor` (diagnostic) | Is a CC1101 actually there. |

### Why device triggers and not binary sensors

HA offers two plausible shapes for a momentary button: a `binary_sensor` that goes on and
must be reset, or a device trigger.

The binary sensor is a trap. The reset can be lost — a dropped connection, a reboot
mid-press — and the entity then sits `on` forever, and every automation has to be written
against a state edge rather than an event.

A device trigger (`device_automation`, `automation_type: "trigger"`) is HA's native
momentary event: it needs no reset, cannot get stuck, and appears directly in the
automation editor as *"Front door pressed"* under this device.

Each stored signal is therefore announced **twice**, as two genuinely different things: a
trigger for *receiving*, and a `button` entity for *transmitting*.

### Deleting a signal

Deleting a signal publishes an empty retained discovery payload, which is how MQTT
discovery says "forget this entity". Without that, a deleted signal would leave a
permanently unavailable entity behind that only a manual purge removes.

### Keeping one node off MQTT

Every node has an **Expose to Home Assistant / MQTT** checkbox in its editor, on by
default. It is on the three node types the bridge actually looks at — **MQTT button**,
**Switch** and **MQTT publish** — and unchecking it makes that one node invisible to the
broker while leaving it completely unchanged inside the graph.

| | with the box ticked | with it cleared |
|---|---|---|
| MQTT button | subscribed on `<base>/trigger/<topic>`, HA button entity | neither |
| Switch | subscribed on `<base>/switch/<topic>/set`, HA switch entity, retained state | none of them, and a `set` command no longer moves it |
| MQTT publish | publishes on `<base>/<topic>` and into `<base>/event` | publishes nothing at all |

Whatever the node had already announced is **cleared**, not abandoned: an empty retained
payload goes out over its discovery config, and over a switch topic's retained `state`
when no exposed node carries that topic any more. Home Assistant therefore removes the
entity rather than showing it unavailable for ever — the same thing that happens when you
delete the node.

Inside the graph nothing changes. A Switch still gates its wire, an MQTT button still
fires from its ▶ button and from the REST API, and an MQTT publish node is still reached
by the chain. The checkbox answers one question only: can anything outside the box see it.

{: .note }
> This exists because a **blank topic no longer means "no MQTT"** — on a Switch or on an
> MQTT button. Both fall back to a slug of the node's name, so a Switch called "Outside
> bell" gets the topic `outside_bell`, and an MQTT button called "Ring the chime" gets
> `ring_the_chime`, whether you asked for one or not. A magic topic value such as `-` was
> considered and rejected: `-` is a perfectly legal MQTT topic level, and so is every other
> string a sentinel could use, so any of them would collide with a topic somebody could
> legitimately want.

## Recipes

### Press a button in Home Assistant, ring the chime

Add an **MQTT button** (`source.virtual`) called "Ring the chime" and link it to a
`signal.tx` carrying the chime's code. That is the whole setup: with discovery on, a
`button` entity for it appears on the Klingelbox device by itself, and pressing it puts the
code on air. No YAML at all.

The topic follows the node's name unless you type one, so the same button is reachable
from an automation, a script, or a shell:

```yaml
action:
  - service: mqtt.publish
    data:
      topic: klingelbox/trigger/ring_the_chime
      payload: ""
```

Any message fires it — the payload is never looked at. Give a second `source.virtual` the
same topic and one press fires both chains.

Or press the auto-created `button` entity. Or, if you would rather transmit a stored signal
directly by name:

```sh
mosquitto_pub -h 192.168.1.10 -t klingelbox/button/front_door/press -m ''
```

### Automate on a doorbell press

In the HA automation editor, pick the Klingelbox device and choose the
*"Front door pressed"* trigger. No YAML needed.

By hand, if you prefer the raw topic:

```yaml
trigger:
  - platform: mqtt
    topic: klingelbox/button/front_door/state
action:
  - service: notify.mobile_app
    data:
      message: >-
        Doorbell ({{ trigger.payload_json.rssi_dbm }} dBm,
        {{ trigger.payload_json.repeats }} repeats)
```

### See every remote in the neighbourhood

Add a `source.any_rf` node linked to a `sink.mqtt`, then watch:

```sh
mosquitto_sub -h 192.168.1.10 -t 'klingelbox/#' -v
```

The retained `klingelbox/unknown` topic and the "Last unknown code" sensor both hold the
fingerprint of the most recent unregistered burst — which is exactly what you want to read
before deciding whether to register it.

### Presence of the box itself

`<base>/status` is retained and is also the Last-Will, so it flips to `offline` when the
box drops off the network without saying goodbye. Every discovered entity uses it as its
availability topic, so an unplugged Klingelbox greys out in HA rather than showing stale
values.

## Troubleshooting

**Nothing appears in Home Assistant.**
Check `<base>/status` says `online` — if not, the box is not connected to the broker at
all (wrong host, wrong credentials). Then check `discovery_prefix` matches your HA install
(`homeassistant` unless you changed it), and that HA's MQTT integration has discovery
enabled.

**Entities appear but are greyed out.**
That is the availability topic doing its job: the box is not currently connected. It will
come back on its own.

**A press fires twice.**
Almost certainly a `source.any_rf` node *and* a `source.button` node both reaching the same
sink. A recognised burst legitimately fires both — see
[the note in Automations](automations.html#sourceany_rf).

**Renaming a signal changed its topic.**
Yes — topics follow the slug of the name. HA entities do not, because their `unique_id` is
keyed on the numeric signal id. If you have automations written against a raw topic string,
they will need updating; automations built on the device trigger will not.
