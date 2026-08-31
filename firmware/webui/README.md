# Klingelbox web UI

> *Die Klingel, lokal und ohne Cloud.*

The browser interface for the Klingelbox — an ESP32-S3 appliance that receives,
stores and replays 433 MHz wireless doorbell signals and routes button presses
through a node graph.

**The node graph is the product.** There are three screens — **Dashboard**,
**Settings**, **Diagnostics** — and the Dashboard *is* the graph, with the live
activity feed underneath it. There is no Signals screen and no Learn screen: a
signal is learned, synthesized, inspected, replayed and rebound from inside the
node that uses it. Everything revolves around the nodes.

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
| *(base)* | Single column. Fixed bottom tab bar (3 cells, 120×56 px at 360 px). Sheets slide up from the bottom, and every multi-step flow is one of them. |
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

## Signals are reached *through* nodes

The user's framing, and the whole information architecture: *"when we add a
433 MHz button we're asked to learn or choose a signal, when we create a virtual
button we can configure the virtual signal — do not make this separate, it all
revolves around the nodes."*

**Adding a node that needs a signal never creates an unbound node.** Picking
`Signal` from *➕ Add node* opens a second picker asking where the signal comes
from, and only then is the node created — already bound, and named after the
signal:

**Learn · Select · Configure**, in that order:

| Choice | What happens |
|---|---|
| 🎓 **Learn a new button** | `POST /api/learn/arm` (auto-armed), live countdown, `GET /api/learn` at 1 Hz, the candidate with confidence/repeats/RSSI, name it, `POST /api/learn/accept` |
| 📚 **Use a signal you already have** | the signal picker: every stored signal, its decoded identity, last seen, and which nodes use it |
| ✨ **Configure by hand** | name / button nibble / base pulse / 20-bit address → `POST /api/signals/virtual` |

All three are bottom sheets, so they work on a phone; each ends in exactly one
confirmation and a working node. Closing a learn sheet mid-flow cancels learn
mode on the box (`POST /api/learn/cancel`) rather than leaving it armed.

### Configure by hand, and why its wording no longer forks

This sheet used to say different things depending on which end of a wire you
were standing at, because a synthesized code meant the opposite thing on a
listen-only node and a send-only one. A `signal` node is both ends, so there is
one sheet and it states both directions:

* it is a code you can **invent** — nothing on the band uses it — so your own
  chime, relay or socket can be paired to it, and the node carries the **Pair
  with a receiver** panel to do exactly that;
* and it is a code the box **listens for**, which an amber note makes plain:
  *a code on its own does nothing — link something into this node’s input and
  the box transmits it; its output stays quiet until something actually sends
  that code over the air.* Without that sentence someone hand-crafts a code,
  hears nothing, and reasonably concludes the box is broken.

The address field takes what the UI shows everywhere else — hex:

* `0xA685A`, `A685A` (any string with a-f is unambiguous) and plain decimal
  `682074` are all accepted; anything else is refused with a sentence instead of
  being silently clamped, because a truncated address yields a signal that
  simply never matches.
* validated to 20 bits (`0`…`0xFFFFF`); out of range says which value and what
  the limit is.
* **🎲 Randomize** fills a fresh `1…0xFFFFF` address client-side. Blank (or `0`)
  still means *let the firmware pick*, which is what `id20: 0` already does.
* a live preview line renders the result in exactly the format shown on every
  signal elsewhere — `EV1527 id=0xA685A btn=0x8` — so a hand-typed code is
  verifiable at a glance.

Node types that need no signal — `source.gpio`, `source.virtual`,
`source.any_rf`, `logic.*`, `sink.mqtt` — go straight to their editor.

**The node editor absorbed the old Signals detail view.** Opening a node that
references a signal shows that signal inline, above the node's own parameters:
decoded identity, base pulse, confidence meter, pulse count, last seen, seen
count, the inline SVG waveform with the 49-vs-50-pulse note for synthesized
frames, **📡 Transmit** to test, the **Pair with a receiver** panel with its
numbered steps and 20× *Pair now* burst for `origin: "synthesized"`, plus
rename, **🔀 Use a different signal**, **🎓 Re-learn** and (on a transmit sink)
**✨ Create a virtual signal**. Rebinding applies immediately; it is an action,
not a form field.

### The store outlives the graph

A learned signal is a *recording of a physical remote*; the graph is only
wiring. So **deleting a node, unlinking it or rebinding it never touches the
store** — the waveform stays under its name and can be picked again tomorrow.
Rewiring must never cost someone a walk to the front door with a remote in hand.

Consequently nothing on the Dashboard can destroy a signal: the picker is purely
constructive. Removing one for good is a separate, deliberate act with exactly
one home, **Settings → Stored signals**.

That list stays compact — name, decoded identity, learned/virtual, in-use
marker, age — and a tap opens the full detail sheet, whose body is *the same
`signalBlock()` the node editor embeds*. So the waveform, confidence meter, base
pulse, pulse count, RSSI, seen/last-seen, the 49-vs-50 note, the pairing panel
for a synthesized code and **📡 Transmit** are all there by construction and can
never drift from the node editor's version. Only the destructive action is added
on top: **delete**, behind a confirmation that names the signal and says plainly
which nodes it will leave without one.

Transmit being there is also what makes an **unbound signal** first-class: a
waveform no node references can still be test-fired straight from the store,
without wiring it into a node first.

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
* Nodes are ordered **sources → signals → logic → sinks**, because on a phone
  the reading order *is* the diagram. A `signal` node has both ports, so no
  single position is right for it; it sits where its commonest role reads
  correctly, which is the same compromise `nextPosition()` makes on the canvas.
* A link pointing at a deleted node renders in red rather than vanishing.

The **canvas** (`≥ 900px`, behind a `List / Map` segmented control that does not
exist below that width) draws the same data as an SVG: nodes at `ui_x`/`ui_y`,
links as cubic béziers. Dragging a node persists `ui_x`/`ui_y`; tapping one
opens the *same* editor sheet the list uses, so the two views can never diverge
in capability.

### Node types

`signal` · `source.gpio` · `source.virtual` · `source.any_rf` ·
`logic.group` · `logic.throttle` · `logic.repeat` · `sink.mqtt`

* **`signal`** is one stored 433 MHz code and the only type with **both** ports.
  Its **output** fires when that code is heard on air; anything linked into its
  **input** transmits it, `repeats` copies `gap_us` apart. It replaced
  `source.button` (in only) and `sink.transmit` (out only), which between them
  made the box able to do both while showing neither — wiring a Virtual trigger
  into a 433 MHz button was refused, for a reason nothing on the screen
  explained. It gets its own group, its own colour, its own column in
  `nextPosition()`, and both connector dots on the canvas. Its card button says
  **▶ Simulate heard**, not *Test fire*: firing a node runs its output, so on
  this type that means “pretend the code arrived” and deliberately does **not**
  transmit. Sending on demand is the 📡 Transmit button inside the node’s signal
  section.
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
  addition to* a matching `signal` node (intended, not double-firing), and
  that a `logic.throttle` belongs between it and its sink on a busy band.

### Recipes

The Dashboard closes with a short recipe list, because the graph's most useful
patterns need no special node type — only links:

* **Several buttons, one chime** — each `signal` → `logic.group`
  (mode `any`) → `signal` (the chime’s code).
* **Two buttons pressed together** — same, with mode `all` and a window.
* **Proxy the whole band to Home Assistant** — `source.any_rf` → `sink.mqtt`.
* **Ring the chime from HA** — `source.virtual` (trigger topic) → `signal`.
* **Stop a stuck button** — `signal` → `logic.throttle` → `signal`.

---

## Screens

Three tabs, plus the recovery wizard that replaces the whole page.

| Screen | Endpoints |
|---|---|
| **Dashboard** — graph | `/api/graph`, `/api/graph/nodes[/{id}[/fire]]`, `/api/graph/links`, `/api/gpio/available`, `/api/config`, `/api/signals` |
| **Dashboard** — activity + status | `/api/events?since=`, `/api/system`, `/api/radio` |
| ↳ *add-node flow: learn* | `/api/learn`, `/api/learn/arm`, `/api/learn/cancel`, `/api/learn/accept` |
| ↳ *add-node flow: configure by hand* | `/api/signals/virtual` |
| ↳ *node editor, signal inline* | `/api/signals`, `/api/signals/{id}` (GET/POST), `/api/signals/{id}/transmit` |
| **Settings** | `/api/config`, `/api/ap`, `/api/radio`, `/api/wifi/scan`, `/api/system/hostname`, `/api/ota*`, `/api/update*`, `/api/restart` |
| ↳ *Stored signals* | `/api/signals`, `/api/signals/{id}` (GET/POST/DELETE), `/api/signals/{id}/transmit`, `/api/graph` (to say who uses what) |
| **Diagnostics** | `/api/diagnostics` |
| **Recovery wizard** | `/api/system`, `/api/wifi/scan`, `/api/wifi` |

A few notes on the less obvious ones:

**The Dashboard's order is deliberate**: view switch and *➕ Add node*, then the
graph, then **Activity**, then the recipes. The feed sits under the graph
because watching presses arrive is exactly what you do *while* wiring. The
box-status chips (radio, frequency, noise floor, Wi-Fi, uptime) ride along with
it, and the two warnings that would make the whole screen a lie — no CC1101, no
home network — sit at the very top instead.

**The waveform** draws `durations_us` as a HIGH/LOW square wave in inline SVG
(`preserveAspectRatio="none"` with a non-scaling stroke). Frames longer than
110 pulses get a real pixel width and scroll horizontally inside their own
container rather than being squeezed into an unreadable comb. A signal with
`decoded: null` is presented as a normal, supported state — the timings are
stored and replay works; only the human-readable identity is missing.

**Virtual signals** keep their explanation in the creation flow *and* in the
node that ends up using one — as the pairing panel on a transmit sink, as the
"something else has to send this" note on a source — because the screen you are
no longer looking at by the time nothing rings is the creation form.

**Learn** states plainly that the receiver is always listening and that learn
mode only changes the fate of an *unrecognised* burst. The admission thresholds
(`repeats ≥ 2`, `confidence ≥ 65`) are shown with the bench numbers behind them:
real presses score 67–92 %, band noise 24–48 %. In the flow that copy is
collapsed behind a `<details>`, so the countdown and the candidate still fit
above the fold in a 360 px bottom sheet.

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
| `learn` | 1 s | only while a learn sheet is open; torn down on close |
| `diag` | 5 s | Diagnostics visible |

`/api/learn` has no loader of its own any more: it is polled by `openLearnFlow`
for exactly as long as its sheet is on screen. `openSheet` therefore takes an
`onClose` callback that fires however the sheet goes away — the ✕, Escape, or a
tap on the scrim — which is what lets the flow stop its timer *and* send
`POST /api/learn/cancel` when someone backs out of an armed box.

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

Roughly 189 KB raw across the three flashed files, well inside the ~250 KB
budget (`app.js` ≈ 155 KB, `style.css` ≈ 30 KB, `index.html` ≈ 4 KB), and
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
