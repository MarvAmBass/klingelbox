# Klingelbox web UI

> *Die Klingel, lokal und ohne Cloud.*

The browser interface for the Klingelbox — an ESP32-S3 appliance that receives,
stores and replays 433 MHz wireless doorbell signals and routes button presses
through a node graph.

**The node graph is the product.** There are five screens — **Dashboard**,
**Activity**, **Settings**, **Diagnostics**, **Handbook** — and the Dashboard
*is* the graph and nothing else.

The live feed and the recipe list used to sit under the graph. Between them they
cost about two screenfuls of scrolling on a phone, so the thing you opened the
app to look at was underneath them; both moved out. The feed became
**Activity**, a whole page of it with a search box and a kind filter. The
recipes went into **Handbook**, together with everything that was explanation
rather than control — three paragraphs of intro above the map and a nine-line
badge legend on top of the canvas. The Dashboard is now a title, one clause, the
two blocking warnings, one toolbar row and the map.

There is still no Signals screen and no Learn screen: a signal is heard,
synthesized, inspected, replayed and rebound from inside the node that uses it.
Everything revolves around the nodes.

**The Handbook is the manual, flashed into the box.** A fresh Klingelbox hands
you its own access point and a captive portal, and a phone joined to that AP has
no internet — `docs/` on the web is unreachable at exactly the moment someone is
working out what a Group node does. So the documentation ships inside the image.
Its node reference is **generated from `NODE_TYPES`**, so a node added to the
palette can be under-documented but never missing.

Four files, no bundler, no dependencies:

| File | Purpose |
|---|---|
| `index.html` | The shell: header, the two navigations, one empty `<section>` per screen. |
| `style.css` | Design tokens (light/dark) plus the whole mobile-first layout. |
| `app.js` | Everything else — every screen is built from live API data. |
| `lang-de.js` | The German dictionary. Data only, no logic. See **Language** below. |

They are flashed as a SPIFFS image on the `storage` partition, so the UI can be
replaced without recompiling the firmware.

**They are gzipped on the way into the image, and only the `.gz` is shipped.**
That is the one build step, it is three lines of CMake plus
`firmware/tools/gzip_webui.py`, and it never touches these files — see **Size**
below for why it exists and what the server does with a client that does not
ask for gzip.

The product name is German, and now so is one of the two interface languages.

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
| *(base)* | Single column. Fixed bottom tab bar (**5 cells, 56–86 × 56 px at 360 px**, measured). Sheets slide up from the bottom, and every multi-step flow is one of them. |
| `600px` | 2-up card grids, roomier padding, sheets become centred dialogs. The brand version and the status badge return; status chips reach tier 2. |
| `720px` | The tab strip moves into the header; the bottom bar retires. The status chips step aside here — see below. |
| `900px` | The node-graph **canvas** view becomes available; forms go 2-column. Status chips return. |
| `1100px` | Centred max-width column, 3-up cards. Status chips reach tier 2 again. |
| `1300px` | All six status chips. |

Concretely:

* **Touch targets** are at least 44 px (`--tap: 2.75rem`): buttons, list rows,
  link chips, nav cells, and both header toggles — theme *and* language, which
  share the `.theme-toggle` box exactly.
* **Inputs are 16 px** (`font-size: 1rem`). Anything smaller makes iOS Safari
  zoom the page on focus, which reads as the layout jumping about.
* **No hover-only affordances.** Every `:hover` rule lives inside a
  `@media (hover: hover) and (pointer: fine)` block and is purely decorative;
  nothing is discoverable only by hovering.
* **Two navigations, one behaviour.** Both the header `.tabs` and the
  `.bottomnav` are always in the DOM and select the same panes; CSS decides
  which is visible, so there is no JS branch on viewport width.
* **Five cells is the bottom bar's ceiling.** At 360 px they measure 56–86 px
  wide and 56 px tall — past 44 px in both axes — using the short labels
  (*Map*, *Health*) and `white-space: nowrap`, so a label that will not fit
  overflows the strip (which scrolls) rather than wrapping and making the bar
  taller. A sixth cell would land near 60 px and start wrapping. If another
  destination is ever needed, something has to leave.
* `env(safe-area-inset-*)` is honoured top and bottom for notched phones.

---

## Signals are reached *through* nodes

The user's framing, and the whole information architecture: *"when we add a
433 MHz button we're asked to listen for or choose a signal, when we create a virtual
button we can configure the virtual signal — do not make this separate, it all
revolves around the nodes."*

**Adding a node that needs a signal never creates an unbound node.** Picking
`Signal` from *➕ Add node* opens a second picker asking where the signal comes
from, and only then is the node created — already bound, and named after the
signal:

**Listen · Select · Configure**, in that order:

| Choice | What happens |
|---|---|
| 🎧 **Listen for a new button** | `POST /api/raw/start` (auto-started), live countdown, `GET /api/raw` at 1 Hz, a **ranked candidate list**, then inspect → trim → `POST /api/raw/candidates/{n}/transmit` → `POST /api/raw/candidates/{n}/save` |
| 📚 **Use a signal you already have** | the signal picker: every stored signal, its decoded identity, last seen, and which nodes use it |
| ✨ **Configure by hand** | name / button nibble / base pulse / 20-bit address → `POST /api/signals/virtual` |

All three are bottom sheets, so they work on a phone; each ends in exactly one
confirmation and a working node. Closing a listening sheet mid-flow stops the
session on the box (`POST /api/raw/stop`) rather than leaving it recording — the
frames are kept, so reopening it shows them again.

### One flow, where there used to be two

There was a *Learn* flow and a separate *Raw capture* flow. Learn armed the box
and waited for a burst that repeated at least twice **and** normalized to 65 %
confidence; raw capture recorded everything and let you cut a signal out by
hand. Both thresholds were measured on EV1527 remotes, so learn was in practice
a flow for one protocol family — a remote of any other shape produced no
candidate, no error, and no explanation. Raw capture, offered as the escape
hatch for exactly that case, picked the same remote up without trouble.

They are now one flow. Detection keeps everything; the box **ranks** instead:

* **repetition dominates** — a real remote repeats itself and noise does not, and
  counting that needs no protocol knowledge;
* a decode and a high confidence only break ties. An undecoded candidate is
  first-class and can be top of the list.

Each row shows `why` it is ranked where it is (`"seen 5 times, decoded ev1527,
92% confidence"`) rather than a score. Under the ranked list, a collapsed
*Every frame recorded* section holds the unranked frames, so grouping can never
hide anything from the user.

**Fragmentation** is surfaced in words. If one press arrives cut into several
dissimilar pieces the box says so, offers a one-tap retry at a longer
`idle_us` derived from the widest gap it measured, and — where the pieces fit
back together — lists the rejoined whole as a 🧩 candidate stitched with the
*measured* silence. That is the fix for "it split the signal into pieces and I
had to test many of them".

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
rename, **🔀 Use a different signal**, **🎧 Listen again** and (on a transmit sink)
**✨ Create a virtual signal**. Rebinding applies immediately; it is an action,
not a form field.

### The store outlives the graph

A captured signal is a *recording of a physical remote*; the graph is only
wiring. So **deleting a node, unlinking it or rebinding it never touches the
store** — the waveform stays under its name and can be picked again tomorrow.
Rewiring must never cost someone a walk to the front door with a remote in hand.

Consequently nothing on the Dashboard can destroy a signal: the picker is purely
constructive. Removing one for good is a separate, deliberate act with exactly
one home, **Settings → Stored signals**.

That list stays compact — name, decoded identity, captured/virtual, in-use
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

A node box may carry up to two **badges**. The layout is **one design**, not two
additions: the ports are the fixed furniture — they sit on the vertical centre
line at `(0, NH/2)` and `(NW, NH/2)`, the output one hiding a 13-unit
`.port-grab` disc. The badges get a strip of their own along the **inside of the
top edge**, right-aligned, and the box grew from `168 × 52` to `168 × 60` to
give them that strip rather than making them sit on the title.

Every badge is the same shape — a filled disc with a border and a centred glyph,
so it reads as a button and not as a loose mark. ✕ is always outermost, so its
position never shifts with the node type; the slot to its left holds whatever
that type has.

| badge | centre | drawn r | hit r | on |
|---|---|---|---|---|
| ▶ act | `(NW-56, 14)` = `(112, 14)` | 9 | 13 | `signal`, `source.virtual` |
| 💡 lamp | `(NW-56, 14)` = `(112, 14)` | 9 | — (readout, not a button) | `sink.monitor` |
| ✕ delete | `(NW-26, 14)` = `(142, 14)` | 9 | 13 | every node |

No type carries both ▶ and 💡, so two slots is the whole vocabulary. The
separations:

| pair | centre-to-centre | combined radii |
|---|---|---|
| ✕ ↔ output grab `(NW, NH/2)` = `(168, 30)` | `√(26² + 16²) = 30.5` | `13 + 13 = 26` |
| ✕ ↔ act badge | `30` | `13 + 13 = 26` |
| act badge ↔ input port `(0, 30)` | `√(112² + 16²) = 113.1` | `13 + 0` (no grab disc) |
| ✕ ↔ input port | `√(142² + 16²) = 142.9` | `13 + 0` |

The natural inset corner `(NW-16, 14)` = `(152, 14)` is only **22.6** from the
output port — a 13-unit hit disc there already overlaps the 13-unit grab — which
is why ✕ is inset a further ten units instead. Vertically the badges end at
`y = 27` and the title's cap height starts at `y = 31`, so **no title is
truncated to make room**: every type keeps the same 22 characters at the same
`x = 12`, and a node with no badges reads exactly as one with them.

**▶ always means "do this node's own thing, now."** The action follows the type:

| type | ▶ does |
|---|---|
| `signal` | `POST /api/signals/{id}/transmit` — sends the code **over the air**, with the node's own `repeats`/`gap_us` |
| `source.virtual` | `POST /api/graph/nodes/{id}/fire` — fires its output |
| `source.gpio`, `source.any_rf` | **none** — they are driven by the physical world and a fake press would be a lie |
| `logic.*`, `sink.mqtt` | **none** — firing a logic node starts a traversal *at* it, and `traverse()` never gates its own start, so ▶ on a Rate limit would push an event through the very node whose job is to block one; ▶ on an MQTT sink would publish a real message with no trigger behind it. Both keep **Test fire** on the list card, where the wording can say what it really is. |

▶ on a `signal` node is **audible** — one click can ring a chime in someone's
house — so it is drawn as an outlined disc rather than a bare glyph, sits well
clear of the drag surface, says *OVER THE AIR* in its `title`, and always
reports back on the canvas message line. Deliberately **no** confirmation: it is
not destructive, and a dialog on a play button is the wrong trade. It greys out,
with the reason shown on click, when the node has no bound signal or
`/api/system` reports no radio.

Two acts, two glyphs, everywhere: **📡 sends a code OUT**, **📥 pretends one came
IN**. That is why the signal card's simulate button is **📥 Simulate heard** and
not `▶ Simulate heard` — ▶ is reserved for "do this node's thing", and on a
signal node that is transmitting, so the same arrow would otherwise mean two
opposite directions on one screen.

Each interactive one calls `stopPropagation()` + `preventDefault()` on
`pointerdown` — without it a click drags the node — and acts on `pointerup`, and
each draws a transparent disc larger than its glyph, exactly as `.port-grab`
does. While a link drag is in flight they stand aside entirely, so releasing a
wire over a ✕ completes the link instead of deleting the node. ✕ goes through
the *same* `confirmSheet` the editor's Delete uses; there is one delete path
(`deleteNodeConfirmed`), shared by the card, the sheet and the map.

### Node types

`signal.rx` · `signal.tx` · `source.gpio` · `source.virtual` · `source.any_rf` ·
`logic.group` · `logic.throttle` · `logic.repeat` · `logic.switch` ·
`sink.mqtt` · `sink.monitor`

**This list is not maintained by hand anywhere the user can see it.** The
Handbook's node reference iterates `NODE_TYPES` and looks each type up in
`NODE_DOC` for its longer description, ports and settings; a type with no
`NODE_DOC` entry still renders with its label, icon, group-derived ports and
`help` string. A new node is therefore under-documented, never absent. Ports
follow from the group with no exceptions — `source` is output-only, `sink` is
input-only, `logic` has both.

* **`signal`** is one stored 433 MHz code and the only type with **both** ports.
  Its **output** fires when that code is heard on air; anything linked into its
  **input** transmits it, `repeats` copies `gap_us` apart. It replaced
  `source.button` (in only) and `sink.transmit` (out only), which between them
  made the box able to do both while showing neither — wiring a Virtual trigger
  into a 433 MHz button was refused, for a reason nothing on the screen
  explained. It gets its own group, its own colour, its own column in
  `nextPosition()`, and both connector dots on the canvas. Its card button says
  **📥 Simulate heard**, not *Test fire*: firing a node runs its output, so on
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
  **▶ Trigger** button and from `POST /api/graph/nodes/{id}/fire`. If MQTT is
  disabled the topic is still settable, with an inline note that it starts
  working once MQTT is enabled. Firing by hand is what this type is *for*, so
  ▶ Trigger is offered in three places and is the card's **primary** button
  rather than a muted test affordance: on the list card, at the top of the
  editor sheet, and as a ▶ hit target on the canvas node.
* **`source.any_rf`** is a wildcard with no parameters. The editor explains that
  it fires on every burst including unregistered ones, that it fires *in
  addition to* a matching `signal` node (intended, not double-firing), and
  that a `logic.throttle` belongs between it and its sink on a busy band.
* **`sink.monitor`** is the one node that *does* nothing. It exists to be looked
  at: a 💡 lamp that lights whenever the chain reaches it and a rolling
  ten-minute timeline of its hits, drawn as inline SVG in the same style as the
  waveform plot. The lamp appears on the node's list card **and** on its box on
  the canvas, so a chain can be watched firing on the map. The strip is on the
  card as well as in the editor sheet — 26 px tall and full-bleed, so it costs
  one line and does not crowd anything at 360 px. Data comes from a single
  `GET /api/monitor` on a named 1 s timer, started only while the Dashboard is
  visible and only when at least one monitor node exists; a 404 there hides the
  lamp, the strip and the palette entry together.

### Recipes

**These live on the Handbook tab now**, not at the foot of the Dashboard. They
are reference material rather than a control, and the graph's most useful
patterns need no special node type — only links:

* **Several buttons, one chime** — each `signal` → `logic.group`
  (mode `any`) → `signal` (the chime’s code).
* **Two buttons pressed together** — same, with mode `all` and a window.
* **Proxy the whole band to Home Assistant** — `source.any_rf` → `sink.mqtt`.
* **Ring the chime from HA** — `source.virtual` (trigger topic) → `signal`.
* **Stop a stuck button** — `signal` → `logic.throttle` → `signal`.
* **Test a chain without ringing anything** — `source.virtual` (▶) →
  `sink.monitor` (💡). Tap ▶ on the card, in the editor or on the canvas node and
  watch the lamp light and the mark land on the timeline.

---

## Screens

Five tabs, plus the recovery wizard that replaces the whole page.

| Screen | Endpoints |
|---|---|
| **Dashboard** — the graph, and only the graph | `/api/graph`, `/api/graph/nodes[/{id}[/fire]]`, `/api/graph/links`, `/api/gpio/available`, `/api/config`, `/api/signals`, `/api/monitor` |
| **Activity** — the live feed | `/api/events?since=` |
| **Handbook** — the offline manual | `/api/config` (once, for the real MQTT base topic) |
| *header status chips* — on every tab | `/api/system`, `/api/radio` |
| ↳ *add-node flow: listen* | `/api/raw`, `/api/raw/start`, `/api/raw/stop`, `/api/raw/candidates/{n}`, `/api/raw/candidates/{n}/transmit`, `/api/raw/candidates/{n}/save` |
| ↳ *add-node flow: configure by hand* | `/api/signals/virtual` |
| ↳ *node editor, signal inline* | `/api/signals`, `/api/signals/{id}` (GET/POST), `/api/signals/{id}/transmit` |
| **Settings** | `/api/config`, `/api/ap`, `/api/radio`, `/api/wifi/scan`, `/api/system/hostname`, `/api/ota*`, `/api/update*`, `/api/restart` |
| ↳ *Stored signals* | `/api/signals`, `/api/signals/{id}` (GET/POST/DELETE), `/api/signals/{id}/transmit`, `/api/graph` (to say who uses what) |
| ↳ *Backup* — export/import | export: `/api/system`, `/api/radio`, `/api/signals`, `/api/signals/{id}`, `/api/graph`. import: `/api/signals/import`, `/api/graph/nodes` (POST/DELETE), `/api/graph/links`, `/api/signals/{id}` (DELETE), `/api/radio` (optional) |
| **Diagnostics** | `/api/diagnostics` |
| **Recovery wizard** | `/api/system`, `/api/wifi/scan`, `/api/wifi` |

A few notes on the less obvious ones:

**The Dashboard is down to four things**: the two warnings that would make the
whole screen a lie (no CC1101, no home network), one toolbar row, and the graph.
Everything else left. The toolbar is a single row — *➕ Add node* on the left,
the `Map | List` switch and the canvas zoom controls pushed right by
`margin-left: auto` on the switch. The zoom controls are emptied (not hidden) in
List view and do not exist below 900 px, and because they are `:empty`-collapsed
their absence closes up without moving the switch, which is anchored to the right
edge rather than to them. Below 900 px the row is the one primary button,
stretched full width so it reads as a deliberate primary action.

**Activity** is a flat list, newest first, with the search box and the kind
filter above it and a count between. There is no collapse and no "show all" —
those existed only because the feed sat under the graph. The filter controls are
built **once**, in `buildActivity()`; `renderFeed()` only ever replaces the
`<ul>` beneath them, because the feed repaints every 2 s and an input rebuilt on
that cadence drops focus and swallows the word you were halfway through typing.
A ✕ Clear button appears only while something is filtered.

**The header status chips** are six chips about the box — radio state, CC1101
part/version, frequency, noise floor, Wi-Fi, uptime — right-aligned *inside* the
header row, not in a band under it, and two chip-lines tall. They fit for free:
what sets the header's height is the 44 px theme toggle, and two chips at
`line-height: 1.15` with `.18rem` of vertical padding come to ~40 px, so the
header is exactly as tall as it was before they existed.

Two things had to be dealt with for a row that relabels itself at 1 Hz:

1. **Rebuilding.** The chips are created once, each with its own text node, and
   every update writes `nodeValue` on a node that already exists. `textContent`
   would replace the node instead — a `childList` mutation per chip per second.
   Verified with a `MutationObserver`: **0 node churn**, only `characterData`.
2. **Reflow.** `"9s"→"10s"` and `"59m 59s"→"1h 0m"` change a chip's *width*, and
   in a right-aligned row that shoves every neighbour sideways. So:
   `font-variant-numeric: tabular-nums`, plus a `min-width` on the two chips
   whose length actually changes, sized *above* their natural width (a
   `min-width` below it locks nothing — that was the first attempt and it did
   nothing at all). The two lines are also **two elements with fixed
   composition**, not one wrapping row, so the wrap point cannot move: line one
   is what the box *is*, line two is what it is *measuring*, and every ticking
   value is on line two. Verified across a 59s→1m 0s boundary: **one distinct
   geometry** for the whole run.

Chips are dropped in tiers as the header tightens, and the numbers are
arithmetic rather than taste — the brand, toggle, badge, padding and gaps come
to ~315 px and the five-tab strip to ~412 px, so a tier may appear only once the
viewport clears `315 + 412 + block width`. Hence 900 / 1100 / 1300, and a
deliberate blackout between 720 and 899 where the tab strip has just arrived and
takes essentially the whole row. Getting this wrong does not overflow the page —
it silently turns `.tabs` into a scroller, and five destinations you cannot see
is the exact failure this restructure exists to fix.

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

**Listening** states plainly that the receiver is always listening and that a
session only changes the fate of an *unrecognised* burst. It also states that
nothing is admitted or rejected on how it looks — candidates are ranked, not
filtered — and says which evidence does the ranking. In the flow that copy is
collapsed behind a `<details>`, so the countdown and the candidate list still fit
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
| `events` | 2 s | **Activity** visible — the Dashboard no longer polls it at all |
| `feedclock` | 1 s | Activity visible — relabels feed ages in place, no re-render |
| `monitor` | 1 s | Dashboard visible **and** at least one Monitor node exists |
| `raw` | 1 s | only while a listening sheet is open; torn down on close |
| `diag` | 5 s | Diagnostics visible |
| *(uptime tick)* | 1 s | always — **not a poll**, and deliberately outside the `timers` map |

The uptime tick is a bare `setInterval` because the header chips are on every
tab and `stopTabPolls()` must not reach it. It puts nothing on the wire:
`/api/system` still arrives every 10 s and uptime is interpolated locally
between samples, so the label has to be rewritten far more often than it is
fetched. Its whole body is one comparison and at most one `nodeValue`
assignment. `dashclock` is gone — its only job was re-rendering the status
chips, which are in the header now.

`/api/raw` has no loader of its own beyond a single boot probe: it is polled by
`openListenFlow` for exactly as long as its sheet is on screen. `openSheet`
therefore takes an `onClose` callback that fires however the sheet goes away —
the ✕, Escape, or a tap on the scrim — which is what lets the flow stop its timer
*and* send `POST /api/raw/stop` when someone backs out of a recording box. The
boot probe exists so that a firmware without the feature removes the button
rather than offering one that 404s.

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

## Language

**English and German.** The toggle sits beside the theme toggle in the header,
wears the same 44 px square, and shows **the language you would get** — `DE`
while you are reading English — because a button showing the language you
already have makes the tap a guess.

### The mechanism: the English string *is* the key

```js
el("button", "btn primary", t("Add node"))     // -> "Node hinzufügen"
```

`t()` looks the English sentence up in the active dictionary and returns the
English itself when there is no entry. **There are deliberately no
`nav.add_node` identifiers, and please do not "improve" it into them.** The
reasoning, so it does not have to be rediscovered:

* This was retrofitted onto a finished 350 KB `app.js`. Opaque ids would mean a
  second dictionary (id → English) to keep in sync forever; English-as-key means
  there is exactly one dictionary per language and it is the only file a
  translator opens.
* A missing or misspelt key degrades to **correct English**. With opaque ids it
  degrades to `nav.add_node` leaking into the interface — much the worse failure
  on a device whose UI ships in flash and cannot be hot-fixed from a server.
* The source stays readable: `t("Add node")` says what appears on screen.

The cost is real and accepted: editing an English string orphans its German
entry (it visibly falls back to the new English), and two identical English
strings cannot take different German. Neither has hurt yet.

### Interpolation

Never concatenate a translated fragment with a value — German word order
differs. One key, with `{placeholders}` that `t()` substitutes:

```js
t("Signal {id} is still used by {n} nodes", { id: sig.id, n: users.length })
```

### What is never translated

Node type wire names (`logic.switch`), MQTT topics, API paths and field names,
GPIO numbers, shell commands, version strings — and every `{"error": "..."}`
sentence the firmware returns, which is authored in C and is closer to log
output than to interface copy.

Top-level tables (`NODE_TYPES`, `EVENT_KINDS`, `DIAG_META`, `CAPTURE_HELP`,
`NODE_DOC`, `PORT_TEXT`, `HB_RECIPES`, `DIAG_PLAIN`) keep their **English**
literals, because those literals are the keys and the tables are evaluated once
at load. Translation happens at the point of use — `nodeType()` does it for the
palette centrally, which is what makes a language switch redraw the node cards.

### Measured, in German, on the device

At **360 px** nothing overflows: the document is 360 px wide on all five tabs,
the widest element is a 336 px panel, and bottom sheets stay at 360 px. The
header still fits with the second toggle in it — "Klingelbox" is not ellipsised
(82 px needed, 82 px given) and the status chips keep their 121 px.

The header tab strip is where German costs the most: **426 px against English's
403 px**, driven almost entirely by *Einstellungen* (103 px vs *Settings*'
69 px). That strip is a horizontal scroller by design and it already scrolled in
English between 720 and 900 px; German widens that window to 720–1100 px. Below
720 px the strip is hidden and the bottom bar takes over, so the phone layout
never sees it.

### Choosing, and switching

`localStorage["klingelbox-lang"]` holds the choice. With nothing stored the
primary subtag of `navigator.language` decides, so a browser set to German opens
in German. Switching sets `<html lang>` (screen-reader voice, hyphenation),
re-labels the static chrome, drops `S.built` and re-enters the current tab —
**no page reload**. Open sheets are left alone on purpose (a listening session
has the radio armed; tearing it down to relabel it would be the worse bug),
polling timers go through the same stop/restart a tab switch performs, and the
graph redraws from the data already in `S` with no refetch.

### Adding a language

1. Copy `lang-de.js` to `lang-<code>.js` and translate the values. Keys stay
   English, byte-for-byte, `{placeholders}` included.
2. Add the global to `LANG_DICTS` and the endonym to `LANG_NAMES` in `app.js`.
3. Add a `<script src="/lang-<code>.js">` to `index.html`, before `app.js`.
4. Past two languages the header control has to stop being a toggle — make it a
   picker; `apply()` already takes an arbitrary code.

Every language costs its own gzipped file on flash. German is 188 KB raw and
**63 KB compressed** — a fifth of the gzipped image, and comfortably inside the
budget the compression bought back (see **Size**).

---

## Size

**607 KB raw** across the four flashed files — `app.js` 362 KB, `lang-de.js`
184 KB, `style.css` 54 KB, `index.html` 7.4 KB. The Handbook is a large share of
both `app.js` and the dictionary, and is the price of a manual that works with
no internet, which is the situation this box is most often in.

**Which is why the image is gzipped.** The 4 MB board's `storage` partition is
`0x80000` = 512 KB, of which about 470 KB is usable after SPIFFS' own overhead.
Measured with `spiffsgen.py`, the *uncompressed* UI needed **94.5 %** of that
partition before German existed — there was no room for a second language, and
CI builds the 4 MB variant, so it would simply have gone red.

Text compresses about 3.2:1:

| File | raw | gzip |
|---|---|---|
| `app.js` | 370162 | 117129 (32 %) |
| `lang-de.js` | 187941 | 64784 (34 %) |
| `style.css` | 55788 | 16398 (29 %) |
| `index.html` | 7576 | 2764 (36 %) |

Measured with `spiffsgen.py`, English-only and uncompressed needed **94.5 %** of
the partition. English **and** German, gzipped, need **43.0 %** — the second
language is affordable only because of the compression, which is why the two
changes landed together.

`firmware/tools/gzip_webui.py` mirrors this directory into
`build/webui-gz/` as `<name>.gz` and *that* is what
`spiffs_create_partition_image` images. This directory is never touched: it must
stay plain, editable, reviewable text. `README.md` is skipped — 35 KB of design
notes that no browser ever fetches has no business on the flash chip. Output is
deterministic (mtime 0), so an unchanged UI produces a byte-identical
`storage.bin`.

**Only the `.gz` is shipped.** Shipping an uncompressed twin would undo the
whole point, so `open_asset()` in `http_api.c` falls back to the `.gz` even for
a client that never sent `Accept-Encoding: gzip`, and labels it honestly with
`Content-Encoding: gzip`. Every browser has understood gzip for two decades and
this is a LAN appliance; a plain `curl` therefore gets correct, correctly
labelled bytes rather than a 404 — read them with `curl --compressed` or a pipe
through `gunzip`.

Nothing is minified, on purpose: the comments explain *why* each non-obvious
decision was made, and a future maintainer reading this off a microcontroller
has no source map. Gzip makes minification unnecessary — comments compress
extremely well.

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

Edit the files and reflash the storage partition. The gzip staging runs as part
of the build, on every build, so there is nothing to remember:

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
node --check app.js && node --check lang-de.js
```
