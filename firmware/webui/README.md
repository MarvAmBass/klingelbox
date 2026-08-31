# Klingelbox web UI

> *Die Klingel, lokal und ohne Cloud.*

The browser interface for the Klingelbox — an ESP32-S3 appliance that receives,
stores and replays 433 MHz wireless doorbell signals and routes button presses
through a node graph.

Three files, no build step, no dependencies:

| File | Purpose |
|---|---|
| `index.html` | The shell: header, the two navigations, one empty `<section>` per screen. |
| `style.css` | Design tokens (light/dark) plus the whole mobile-first layout. |
| `app.js` | Everything else — every screen is built from live API data. |

They are flashed as a SPIFFS image on the `storage` partition, so the UI can be
replaced without recompiling the firmware. The product name is German; **the
interface is English on purpose**.

---

## Ground rules

**Vanilla only.** No framework, no bundler, no external font, no CDN. The page
is served off a microcontroller, frequently over its own access point with no
internet at all — anything fetched from elsewhere would simply never load.

**`docs/API.md` is the contract.** Every request in `app.js` appears in that
document and nothing else is invented. Failures always arrive as
`{"error": "..."}` with a real status; `api()` turns that into an `Error`
carrying `.message` (the server's sentence) and `.status`.

**A missing feature hides, it never breaks the page.** `404` means "this
firmware does not have that", `503`/`409` means "the hardware is not there".
Either way the affected control disappears or is disabled *with the reason
shown*, and the rest of the UI keeps working. A box with an unplugged CC1101
must stay fully navigable — that is precisely when someone needs the
Diagnostics screen.

---

## Mobile-first, inverted from the reference UI

The base stylesheet **is** the phone layout. Every media query is `min-width`
and only ever adds; there is no `max-width` rule anywhere in `style.css`, so a
narrow viewport can never be handed a desktop rule that then has to be undone.
The reference target is a 360 px portrait phone.

| Breakpoint | What it adds |
|---|---|
| *(base)* | Single column. Fixed bottom tab bar. Sheets slide up from the bottom. |
| `600px` | 2-up card grids, roomier padding, sheets become centred dialogs. |
| `720px` | The tab strip moves into the header; the bottom bar retires. |
| `900px` | The node-graph **canvas** view becomes available; forms go 2-column. |
| `1100px` | Centred max-width column, 3-up cards. |

Concretely:

* **Touch targets** are at least 44 px (`--tap: 2.75rem`): buttons, list rows,
  link chips, nav cells, the theme toggle.
* **Inputs are 16 px** (`font-size: 1rem`). Anything smaller makes iOS Safari
  zoom the page on focus, which reads as the layout jumping about.
* **No hover-only affordances.** Every `:hover` rule lives inside a
  `@media (hover: hover) and (pointer: fine)` block and is purely decorative;
  nothing is discoverable only by hovering.
* **Two navigations, one behaviour.** Both the header `.tabs` and the
  `.bottomnav` are always in the DOM and select the same panes; CSS decides
  which is visible, so there is no JS branch on viewport width.
* `env(safe-area-inset-*)` is honoured top and bottom for notched phones.

---

## The node graph on a phone

A drag-and-drop canvas is unusable with a thumb, so the **list is the primary
representation and the canvas is a strict extra**. Everything is doable from
the list alone.

Each node is a card showing its icon, name, type, a one-line config summary,
and its links as chips:

```
🔘  Front door                        [disabled?]
    433 MHz BUTTON
    Listens for: Front door

    INPUTS   (sources have none)
    OUTPUTS  [→ Front chime ✕]  [＋ Add output]

    [Edit]  [Test fire]  [Delete]
```

* **Adding a link** is a tap on `＋ Add output` (or `＋ Add input`), then a
  picker sheet of eligible nodes. Ineligible ones are not offered at all:
  sources have no input, sinks have no output, and an existing link shows as
  "Already linked" and is disabled.
* **Removing a link** is a tap on the chip's `✕` plus a confirmation that spells
  out the connection in words (`Front door → Front chime`).
* Nodes are ordered **sources → logic → sinks**, because on a phone the reading
  order *is* the diagram.
* A link pointing at a deleted node renders in red rather than vanishing.

The **canvas** (`≥ 900px`, behind a `List / Map` segmented control that does not
exist below that width) draws the same data as an SVG: nodes at `ui_x`/`ui_y`,
links as cubic béziers. Dragging a node persists `ui_x`/`ui_y`; tapping one
opens the *same* editor sheet the list uses, so the two views can never diverge
in capability.

### Node types

`source.button` · `source.gpio` · `source.virtual` · `source.any_rf` ·
`logic.group` · `logic.throttle` · `sink.transmit` · `sink.mqtt`

* **`source.gpio`** is optional. The pin list comes from `GET /api/gpio/available`
  — `suggested` pins first, `in_use` pins listed but disabled. If that endpoint
  answers 404 the node type is removed from the "Add node" picker entirely. The
  editor states the wiring in one line: *button between the GPIO and GND, the
  internal pull-up does the rest*.
* **`source.virtual`** carries a **trigger topic** bound to `topic`, subscribed
  as `<base>/trigger/<topic>`. The editor shows the resulting full topic as
  read-only helper text so it can be copied straight into Home Assistant or an
  `mosquitto_pub` line. The base comes from `GET /api/config`
  (`mqtt.base_topic`) — `klingelbox` appears only as placeholder text before
  that answer arrives. Empty topic is fine: the node still fires from the
  **Trigger** button and from `POST /api/graph/nodes/{id}/fire`. If MQTT is
  disabled the topic is still settable, with an inline note that it starts
  working once MQTT is enabled.
* **`source.any_rf`** is a wildcard with no parameters. The editor explains that
  it fires on every burst including unregistered ones, that it fires *in
  addition to* a matching `source.button` (intended, not double-firing), and
  that a `logic.throttle` belongs between it and its sink on a busy band.

### Recipes

The Automations screen carries a short recipe list, because the graph's most
useful patterns need no special node type — only links:

* **Several buttons, one chime** — each `source.button` → `logic.group`
  (mode `any`) → `sink.transmit`.
* **Two buttons pressed together** — same, with mode `all` and a window.
* **Proxy the whole band to Home Assistant** — `source.any_rf` → `sink.mqtt`.
* **Ring the chime from HA** — `source.virtual` (trigger topic) → `sink.transmit`.
* **Stop a stuck button** — `source.button` → `logic.throttle` → `sink.transmit`.

---

## Screens

| Screen | Endpoints |
|---|---|
| **Dashboard** | `/api/system`, `/api/radio`, `/api/events?since=`, `/api/signals`, `/api/signals/{id}/transmit` |
| **Signals** | `/api/signals`, `/api/signals/{id}` (GET/POST/DELETE), `/api/signals/{id}/transmit`, `/api/signals/virtual` |
| **Learn** | `/api/learn`, `/api/learn/arm`, `/api/learn/cancel`, `/api/learn/accept` |
| **Automations** | `/api/graph`, `/api/graph/nodes[/{id}[/fire]]`, `/api/graph/links`, `/api/gpio/available`, `/api/config` |
| **Settings** | `/api/config`, `/api/ap`, `/api/radio`, `/api/wifi/scan`, `/api/system/hostname`, `/api/ota*`, `/api/restart` |
| **Diagnostics** | `/api/diagnostics` |
| **Recovery wizard** | `/api/system`, `/api/wifi/scan`, `/api/wifi` |

A few notes on the less obvious ones:

**Signals** draws `durations_us` as a HIGH/LOW square wave in inline SVG
(`preserveAspectRatio="none"` with a non-scaling stroke). Frames longer than
110 pulses get a real pixel width and scroll horizontally inside their own
container rather than being squeezed into an unreadable comb. A signal with
`decoded: null` is presented as a normal, supported state — the timings are
stored and replay works; only the human-readable identity is missing.

**Virtual signals** get their own explanation on that screen: a fresh EV1527
code exists so the user can pair *their own* receivers to this box by
transmitting it while the receiver is in learning mode.

**Learn** states plainly that the receiver is always listening and that learn
mode only changes the fate of an *unrecognised* burst. The admission thresholds
(`repeats ≥ 2`, `confidence ≥ 65`) are shown with the bench numbers behind them:
real presses score 67–92 %, band noise 24–48 %.

**Diagnostics** renders each state with its firmware-supplied `help` sentence,
its count, its last-seen age and its `detail` string, plus a short "what to do
next" line per state and a plain-language verdict banner derived from which
states have fired. The capture counters each carry an explanation of what a
rising number means. This is the page someone reads when nothing works, so it
is written to be read cold.

**Recovery wizard** replaces the entire page (the tab bars are hidden and the
header badge turns amber) when `GET /api/system` reports
`wifi_mode == "recovery"`. It is served through a captive portal on a phone, so
it is scan → tap → password → save at 360 px with nothing else on screen, and
ends in an explicit "rebooting, reconnect to your home Wi-Fi" state that names
the hostname to come back to.

---

## Polling

One named timer per job, torn down on tab change, and every tick skipped while
`document.hidden` — a backgrounded tab or a locked phone must not keep the
radio and the battery busy. `visibilitychange` refreshes immediately on return.

| Timer | Interval | Runs when |
|---|---|---|
| `system` | 10 s | always |
| `events` | 2 s | Dashboard visible |
| `dashclock` | 1 s | Dashboard visible — relabels ages only, no re-render |
| `learn` | 1 s | Learn visible |
| `diag` | 5 s | Diagnostics visible |

`GET /api/events?since=<serial>` is guarded by its serial: an unchanged serial
with no new events returns early and the feed DOM is left completely alone, so
the common idle case costs one request and zero layout work. A `button_press`
or `rf_unmatched` event additionally refreshes `/api/signals`, because it
changes `seen_count`/`last_seen_s` — but only when something actually happened.

Uptime is interpolated locally between `/api/system` samples so "12s ago"
labels tick smoothly at 1 Hz off a 0.1 Hz poll.

---

## Two readings of the API, pinned down here

`docs/API.md` leaves two things implicit. The UI made a choice; if
`http_api.c` disagrees, this is the place to reconcile.

1. **`ts_s` and `last_seen_s` are device-uptime seconds, not epoch.** The
   epoch field in the contract is `created_at` (`1756600000`), while `ts_s`
   sits at `12` next to an `uptime_s` of `1234`. Ages are therefore rendered as
   `uptime_now − ts`. As a safety net, any value above 1 000 000 000 is treated
   as an epoch timestamp and formatted as a date instead.

2. **Wi-Fi passphrases are written to `/api/config` as
   `sta.networks[].password`.** `GET` returns `has_pass` and never the secret;
   the write key is not documented. `password` was chosen to match
   `POST /api/wifi`, the only place the contract names a password field. An
   empty string is never sent — per the contract that means "leave unchanged",
   so a blank field keeps the stored secret rather than clearing it. The same
   rule applies to the MQTT password and the AP recovery passphrase.

---

## Theme

CSS custom properties define a full light palette on bare `:root`. Dark is
applied twice: through `@media (prefers-color-scheme: dark)` guarded with
`:root:not([data-theme="light"])`, and through `:root[data-theme="dark"]`, so an
explicit choice beats the system in *both* directions. The header toggle cycles
**Auto → Light → Dark → Auto** and stores only `"light"` or `"dark"` in
`localStorage`; the absence of the key *is* Auto. The first tap always shows the
opposite of what the system is currently displaying, so it visibly does
something.

---

## Size

Roughly 145 KB raw across the three files, well inside the ~250 KB budget, and
it compresses to a fraction of that in the SPIFFS image. Nothing is minified on
purpose: the comments explain *why* each non-obvious decision was made, and a
future maintainer reading this off a microcontroller has no source map.

---

## Fallback page

`firmware/main/www/index.html` is a separate, single-file placeholder flashed
when `firmware/webui/` is absent at build time. It has no external references at
all — a stylesheet or a script would be exactly the thing that is missing — and
beyond explaining the situation it offers a `storage.bin` upload straight to
`POST /api/ota/webui/upload`, so the real UI can be installed from a phone over
the setup hotspot with no internet on the box.

---

## Working on it

There is no build step. Edit the files and reflash the storage partition:

```sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 storage-flash
```

Or, faster during development, upload `storage.bin` from the browser under
**Settings → Firmware & web UI update**.

`app.js` must stay parseable by a plain ES5-era engine style (`var`, no
optional chaining, no arrow functions) beyond `Promise` and `fetch`, which every
target browser has. Check it before flashing:

```sh
node --check app.js
```
