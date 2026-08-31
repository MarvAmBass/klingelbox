/*
 * node_graph.h - The routing engine: sources -> logic -> sinks.
 *
 * EVERY NODE IS STRICTLY LEFT-TO-RIGHT. There was briefly one exception — a
 * single DB_NODE_SIGNAL with a port at each end — and it is gone: a node whose
 * behaviour depended on how the walk entered it is a node nobody can read off
 * the screen. A 433 MHz signal can be received or sent, so it gets TWO node
 * types, DB_NODE_SIGNAL_RX and DB_NODE_SIGNAL_TX, sharing one signal pool.
 *
 * This is what turns "a button was pressed" into "these chimes ring". Users
 * think in terms of wiring things together — this button rings that chime, these
 * three buttons all ring the upstairs chime, don't ring at night — so the
 * firmware models exactly that rather than a fixed set of options.
 *
 * EVENT-DRIVEN, NOT POLLED. Evaluation starts at a source and walks forward
 * along links. Nothing scans the graph on a timer, so an idle box costs nothing.
 * Traversal is depth-limited and visit-marked, so a user who accidentally wires
 * a cycle gets a stopped traversal and a logged warning rather than a stack
 * overflow.
 *
 * FAN-OUT IS THE POINT. A node may have any number of outgoing links, and ALL of
 * them are followed — one press can ring two chimes, publish to MQTT and start a
 * throttle in the same traversal. Equally, a node may have several inbound links
 * (that is what makes logic.group useful). The engine must never stop at the
 * first matching link.
 *
 * NOTHING IS IMPLICIT. There is no global "publish everything" behaviour: MQTT is
 * emitted only where the user has actually placed a sink.mqtt node, so which
 * presses reach the broker is a wiring decision, not a setting. The same is true
 * of transmitting. A source with no links does nothing at all — which is the
 * correct, quiet default for a signal you have learned but not yet wired up.
 *
 * FLAT NODE STRUCT, ON PURPOSE. Every node carries the union of all type-specific
 * fields instead of a tagged union. It wastes a few dozen bytes per node and buys
 * three things that matter more: the whole graph serializes to NVS as one plain
 * array, the REST/JSON mapping is mechanical, and adding a field to one node type
 * never breaks the on-flash layout of the others.
 *
 * SINKS ARE INJECTED. The graph must transmit RF and publish MQTT, but must not
 * depend on those modules — that would make it untestable and circular. Instead
 * the app registers handlers at startup.
 */
#ifndef DB_NODE_GRAPH_H
#define DB_NODE_GRAPH_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DB_NODE_MAX      24
#define DB_LINK_MAX      48
#define DB_NODE_NAME_MAX 32
#define DB_NODE_TOPIC_MAX 48

/* Bounds the blast radius of a mis-wired graph. */
#define DB_GRAPH_MAX_DEPTH 8

/*
 * THE ENUM VALUES ARE THE ON-FLASH FORMAT. A node blob stores `type` as a raw
 * uint8_t, so a slot number is a persisted fact, not an implementation detail.
 * Never renumber an existing entry; append, or retire a slot and leave the hole.
 * node_graph.c's load path migrates old blobs by slot number, and the version in
 * the blob header is what tells it which numbering it is looking at.
 */
typedef enum {
    /*
     * DB_NODE_SIGNAL_RX — "Signal receiver". One stored 433 MHz signal, heard.
     *
     *                    [ Front door / EV1527 0xA685A ]-o out
     *
     * OUTPUT ONLY. It fires when its signal_id is recognized on air, and that
     * is the whole of it. It transmits nothing, ever, on any path — which is
     * what makes rx(A) -> ... -> tx(A) a loop the user can SEE rather than a
     * loop the engine has to defend against from the inside.
     *
     * It sits at slot 0 — where source.button sat, and then the two-ported
     * DB_NODE_SIGNAL — so the common stored node migrates by doing nothing at
     * all. See migrate_nodes() in node_graph.c for how a v2 `signal` node is
     * sorted into rx or tx by the links already attached to it.
     */
    DB_NODE_SIGNAL_RX = 0,

    /* ---- sources: things that start a chain ---- */
    DB_NODE_SOURCE_GPIO = 1,   /* a wired button on a GPIO (see below)       */
    DB_NODE_SOURCE_VIRTUAL,    /* fired from the UI / REST / an MQTT topic   */
    DB_NODE_SOURCE_ANY_RF,     /* WILDCARD: every received burst, incl. unknown */

    /* ---- logic: things that shape a chain ---- */
    DB_NODE_LOGIC_GROUP,       /* any-of / all-of within a time window       */
    DB_NODE_LOGIC_THROTTLE,    /* debounce / rate limit                      */
    /* Emit now, then again N-1 times, `window_ms` apart. This is the node the
     * user asked for as a "loop"; it is called REPEAT because "loop" already
     * means something else and worse in this engine — a cycle in the wiring,
     * which traverse() warns about and refuses to walk. */
    DB_NODE_LOGIC_REPEAT,      /* auto-repeat: ring again, N times, spaced    */

    /* ---- sinks: things that act ---- */
    /* RETIRED, NEVER REUSED. Slot 7 was sink.transmit; its behaviour is now
     * DB_NODE_SIGNAL_TX (slot 11). The hole is deliberate: sink.mqtt keeps
     * slot 8 so that only ONE slot has to be remapped when a v1 blob is loaded,
     * and a stray 7 arriving from anywhere else must never silently become a
     * different node type. There is no wire name for it, so the REST API cannot
     * produce one. */
    DB_NODE__RETIRED_TRANSMIT = 7,
    DB_NODE_SINK_MQTT = 8,     /* publish an event                           */
    /*
     * DB_NODE_SINK_MONITOR — a sink that ACTS ON NOTHING.
     *
     * It records that it was reached and that is the whole of it: no radio, no
     * broker, no GPIO. Drop one anywhere in a chain and the UI can show a lamp
     * that lights when it fires plus a rolling ten-minute timeline of when it
     * did — which is what makes a graph debuggable without ringing anything.
     *
     * Appended at the end of the enum, never inserted, so no stored blob moves
     * and no migration is owed. `window_ms` is borrowed for how long the
     * indicator stays lit per hit (see below); the hit ring itself lives in RAM
     * in node_graph.c and is never persisted.
     */
    DB_NODE_SINK_MONITOR = 9,

    /*
     * DB_NODE_LOGIC_SWITCH — a switch in the wire.
     *
     *          in o-[ Outside bell        ON ]-o out
     *
     * Every other type in this enum is a source, a transform or a sink. This one
     * sits IN a wire and decides whether it conducts: while it is ON an event
     * passes straight through untouched, while it is OFF nothing gets past it.
     * Until it existed the only way to stop a path was to delete the link, which
     * is destructive and cannot be automated.
     *
     * ITS POSITION IS `enabled`. Not a second flag beside it — the SAME one. A
     * disabled node is already skipped by traverse(), which is precisely
     * "blocks", so a switch that is off and a node that is disabled are one idea
     * and not two overlapping ones. That also means no new struct field, and
     * therefore no change to the on-flash layout of db_node_t and no migration.
     *
     * The catch that comes with it is flash wear: the nodes blob is rewritten in
     * full on every mutation, so a Home Assistant automation flapping a switch
     * would write NVS on every toggle. The switch path therefore does NOT save
     * synchronously — see db_graph_switch_set() and switch_save_service() in
     * node_graph.c. RAM is authoritative, flash catches up when things go quiet.
     *
     * `topic` is reused as the MQTT suffix, exactly as SOURCE_VIRTUAL reuses it:
     * <base>/switch/<topic>/set is subscribed and <base>/switch/<topic>/state is
     * published retained. SEVERAL SWITCH NODES MAY SHARE ONE TOPIC — that is the
     * feature, not an accident: one Home Assistant toggle then gates several
     * paths at once. Nothing here or in mqtt_bridge.c may assume topic
     * uniqueness.
     *
     * Appended at the end of the enum (slot 7 stays the retired hole), so no
     * stored blob moves and no migration is owed.
     */
    DB_NODE_LOGIC_SWITCH = 10,

    /*
     * DB_NODE_SIGNAL_TX — "Signal sender". One stored 433 MHz signal, sent.
     *
     *          in o-[ Upstairs chime / EV1527 0x1D3F0 ]
     *
     * INPUT ONLY. Reaching it — over a link, or by firing it directly — puts
     * its signal_id on the air, `repeats` copies `gap_us` apart. There is no
     * second meaning and no context to check: a tx node asked to act sends.
     *
     * THE PAIR IS THE POINT. The old unified DB_NODE_SIGNAL did both jobs and
     * chose between them by whether the traversal STARTED at it, because
     * without that test every reception would instantly re-transmit what it had
     * just heard. That flag is deleted, not moved: with one job per type the
     * ports say which way events travel, and a signal that needs both
     * directions is two nodes, wired however the user means them.
     *
     * Appended at the next free slot — 7 is the retired sink.transmit hole, 9
     * is the monitor and 10 the switch — so no stored blob moves.
     */
    DB_NODE_SIGNAL_TX = 11,

    DB_NODE__COUNT
} db_node_type_t;

/* ---- monitor (visualizer) sink ----
 *
 * DEBUG TELEMETRY, RAM ONLY. A monitor's hits are never written to flash: a
 * doorbell that logged every press to NVS would wear the part out for data
 * nobody reads a minute later, which is the same reasoning event_log.h gives.
 *
 * Two independent bounds, both enforced on insert. The retention window is what
 * the UI draws; the per-node cap is what stops a chatty band from turning a
 * debug node into a memory cost. Whichever bites first, bites.
 */
#define DB_MONITOR_RETENTION_S 600   /* the ten minutes the timeline shows     */
#define DB_MONITOR_HITS        64    /* hits kept per monitor node             */

/* How long the indicator stays lit after a hit, in SECONDS on the wire. Stored
 * in the shared `window_ms` field — no new struct field, so the on-flash layout
 * of db_node_t is untouched. */
#define DB_MONITOR_HOLD_MIN_S  1u
#define DB_MONITOR_HOLD_MAX_S  60u
#define DB_MONITOR_HOLD_DEF_S  3u

typedef enum { DB_GROUP_ANY = 0, DB_GROUP_ALL } db_group_mode_t;

typedef struct {
    uint16_t id;                        /* 1..N; 0 = invalid                  */
    uint8_t  type;                      /* db_node_type_t                     */
    bool     enabled;
    char     name[DB_NODE_NAME_MAX];

    /* SIGNAL_RX: the stored signal this node listens for.
     * SIGNAL_TX: the stored signal this node sends.
     * ONE POOL, TWO TYPES — the same signal_id may legitimately appear on an rx
     * node, on a tx node, or on several of each. */
    uint16_t signal_id;

    /* SOURCE_GPIO — an OPTIONAL wired button, fully configured from the web UI.
     * The pin is stored HERE, per node, not compiled in, so a user can wire a
     * door button to whatever GPIO is convenient and set it in the interface.
     * board_pins.h only supplies suggested defaults for the picker.
     *   gpio_pin    - the GPIO number (-1 = unset/disabled)
     *   active_low  - true (default) for a button wired to GND with the internal
     *                 pull-up enabled: the simplest wiring, and an unconnected
     *                 pin then reads as "not pressed" instead of floating.
     *   debounce_ms - mechanical buttons bounce for milliseconds; without this a
     *                 single press fires the chain several times. */
    int8_t   gpio_pin;
    bool     gpio_active_low;
    uint16_t gpio_debounce_ms;

    /* DB_NODE_SIGNAL_TX: how the frame goes out. Real receivers usually require
     * several consistent copies before acting, so repeats is not cosmetic.
     *
     * LOGIC_REPEAT borrows `repeats` for a different but honestly named job: how
     * many times in TOTAL it emits, counting the immediate one. Reusing the
     * field rather than adding a second count keeps the flat struct (and with it
     * the NVS layout and the REST mapping) exactly as it was — the two types are
     * never both interpreting it at once. */
    uint8_t  repeats;
    uint32_t gap_us;

    /* LOGIC_GROUP / LOGIC_THROTTLE / LOGIC_REPEAT: the time window, in
     * MILLISECONDS internally.
     * The REST API and the UI both speak SECONDS (`window_s`), because that is
     * the unit people actually reason in for a doorbell cooldown — "ring at most
     * once every 10 seconds". Milliseconds are kept in the struct only so a
     * group window can be sub-second if anyone ever needs it.
     *
     * LOGIC_THROTTLE is a LEADING-EDGE throttle (a "cooldown" / "lockout", what
     * Node-RED calls a rate limit): the first event passes through immediately
     * and everything within the window after it is dropped. That ordering is the
     * point — a doorbell must ring on the FIRST press, then go quiet, rather
     * than making a visitor wait out a window before anything happens.
     *
     * It is source-agnostic: an RF remote, a wired GPIO button and an MQTT
     * trigger are all limited identically, because it limits whatever is linked
     * into it rather than inspecting where the event came from.
     *
     * LOGIC_REPEAT uses the window as the INTERVAL between emissions. It passes
     * the event on immediately and then releases it again `repeats - 1` more
     * times, one window apart, each one UNCHANGED — same trigger, same label,
     * same signal — down its own outgoing links. It drops nothing and it never
     * blocks; it places the same action repeatedly in time.
     *
     *     button --> repeat (3 times, 5 s) --> transmit
     *
     * rings at 0 s, 5 s and 10 s from a single press. `repeats` of 1 is a legal
     * pass-through that does nothing at all.
     *
     * A NEW event arriving while a repeat is still running RESTARTS it rather
     * than stacking a second run on top: five impatient presses must not leave
     * fifteen queued rings. */
    uint32_t window_ms;
    uint8_t  group_mode;                /* db_group_mode_t */

    /* Topic suffix, used by three node types:
     *   SINK_MQTT     - published to as <base>/<topic> when the node fires.
     *   SOURCE_VIRTUAL- SUBSCRIBED to as <base>/trigger/<topic>; any message
     *                   arriving there fires the node. This is what makes a
     *                   virtual input reachable from Home Assistant, Node-RED or
     *                   a shell one-liner, with no RF involved at all.
     *   LOGIC_SWITCH  - SUBSCRIBED to as <base>/switch/<topic>/set, and its
     *                   position published retained on <base>/switch/<topic>/state.
     *                   Deliberately NOT unique: several switch nodes may carry
     *                   the same topic so that one HA toggle gates all of them.
     * Empty on a SOURCE_VIRTUAL or a LOGIC_SWITCH means "no MQTT" (UI/REST only). */
    char     topic[DB_NODE_TOPIC_MAX];

    /* UI canvas position. Stored so a layout survives a reboot; ignored by the
     * engine, and irrelevant to the mobile list view. */
    int16_t  ui_x, ui_y;
} db_node_t;

typedef struct {
    uint16_t from;
    uint16_t to;
} db_link_t;

/*
 * What caused a traversal — carried from the source all the way to every sink.
 *
 * Without this a sink knows only *that* it fired, which is useless for MQTT: a
 * payload saying "something happened" tells Home Assistant nothing. Passing the
 * trigger through means an MQTT sink can publish WHICH signal arrived, how
 * strong it was, and how it decoded — including for bursts that match no stored
 * signal at all (signal_id == 0), which is exactly the "proxy every press to
 * MQTT" case.
 */
typedef struct {
    uint16_t signal_id;                 /* stored signal, or 0 if unrecognized  */
    uint32_t fingerprint;               /* rf_fingerprint_t; identity of unknowns*/
    int16_t  rssi_dbm;
    uint8_t  repeats;
    bool     decoded_valid;
    char     protocol[16];              /* "ev1527", or "" when undecoded       */
    uint32_t decoded_id;
    uint8_t  decoded_button;
    char     label[DB_NODE_NAME_MAX];   /* signal name, node name, or "unknown" */
} db_trigger_t;

/* Sink handler. Receives the node that fired AND what triggered it; the app
 * supplies the behaviour so the graph stays free of RF/MQTT dependencies. */
typedef void (*db_sink_fn)(const db_node_t *node, const db_trigger_t *trig, void *ctx);

esp_err_t db_graph_init(void);

int                db_graph_node_count(void);
const db_node_t   *db_graph_nodes(void);
const db_node_t   *db_graph_node(uint16_t id);
int                db_graph_link_count(void);
const db_link_t   *db_graph_links(void);

/* Mutation. All of these persist immediately — a doorbell that forgets its
 * wiring on power loss is worse than useless. */
esp_err_t db_graph_add_node(const db_node_t *node, uint16_t *id_out);
esp_err_t db_graph_update_node(const db_node_t *node);   /* matched by node->id */
esp_err_t db_graph_delete_node(uint16_t id);             /* also drops its links */
esp_err_t db_graph_add_link(uint16_t from, uint16_t to);
esp_err_t db_graph_delete_link(uint16_t from, uint16_t to);

/* Fill *node with type-appropriate defaults (repeats, windows, debounce...). */
void db_graph_node_defaults(db_node_t *node, db_node_type_t type);

void db_graph_set_transmit_handler(db_sink_fn fn, void *ctx);
void db_graph_set_mqtt_handler(db_sink_fn fn, void *ctx);

/* ---- monitor readout ----
 *
 * Copy up to `max` hit timestamps for one DB_NODE_SINK_MONITOR node, NEWEST
 * FIRST, in esp_timer microseconds — the same clock esp_timer_get_time()
 * returns, so a caller reports ages by subtraction and needs no wall clock. The
 * box may have no time source at all, which is precisely why this is not epoch
 * seconds. Anything older than DB_MONITOR_RETENTION_S is already gone. Returns
 * how many were written; 0 for any other node type. */
int db_graph_monitor_hits(uint16_t node_id, int64_t *out_us, int max);

/* The indicator hold of a monitor node in seconds, defaulted and clamped to
 * DB_MONITOR_HOLD_MIN_S..MAX_S. One place decides it, so the value the API
 * reports is the value the UI lights by. */
uint16_t db_graph_monitor_hold_s(const db_node_t *n);

/* ---- logic.switch --------------------------------------------------------
 *
 * The position of a switch node is its `enabled` flag (see DB_NODE_LOGIC_SWITCH
 * above). These are the ONLY writers that a Home Assistant automation can reach,
 * so they are also the only ones that must not put the flash under it.
 *
 * RAM IS AUTHORITATIVE, FLASH CATCHES UP. A change takes effect on the very next
 * traversal, and the blob is written back once the position has been stable for
 * a while — never once per toggle. A reboot therefore comes up in the last
 * settled position rather than a position nobody chose, and the box re-publishes
 * that position retained on connect, so Home Assistant is never left showing a
 * switch the box is not actually in.
 */

/* Set one switch node by id. ESP_ERR_NOT_FOUND if there is no such node,
 * ESP_ERR_INVALID_ARG if it is not a DB_NODE_LOGIC_SWITCH. */
esp_err_t db_graph_switch_set(uint16_t node_id, bool on);

/* Set EVERY switch node carrying `topic` — the "one HA toggle, N switches" case.
 * Returns how many nodes it moved (0 = no switch node has that topic). */
int db_graph_switch_set_topic(const char *topic, bool on);

/*
 * The position to report for `topic`, and whether any switch node carries it.
 *
 * ANY-OF, deliberately. Nodes sharing a topic can only diverge by someone moving
 * one of them from the UI, and the question Home Assistant is really asking is
 * "can anything get through?". Reporting ON while one path conducts is honest;
 * an ALL-of rule would show OFF while the inside bell still rang.
 */
bool db_graph_switch_topic_state(const char *topic, bool *found_out);

/* ---- entry points that start a traversal ---- */

/*
 * An RF burst arrived. Fires:
 *   - every enabled DB_NODE_SIGNAL_RX node bound to trig->signal_id (when
 *     non-zero), and
 *   - every enabled SOURCE_ANY_RF node, ALWAYS — including for bursts that match
 *     no stored signal.
 *
 * DB_NODE_SIGNAL_TX nodes are never started by a burst, whatever they are bound
 * to. That is the split doing its job: a heard code cannot re-send itself
 * because the node that heard it has no transmit side to reach.
 *
 * SOURCE_ANY_RF is the "proxy everything to MQTT" primitive: wire one to a
 * sink.mqtt and Home Assistant sees every press on the band, registered or not.
 * Because it also fires for recognized signals, a burst can legitimately drive
 * both a specific signal chain and the wildcard chain in one traversal — that is
 * intended, not double-firing.
 *
 * Safe to call from the RF event path. */
void db_graph_on_rf(const db_trigger_t *trig);

/* A wired GPIO input fired (signal_id 0, label = the node's name). Exposed so
 * the app can report it uniformly; normally driven internally by the GPIO ISR
 * path. */
void db_graph_on_wired(uint16_t node_id);

/* Fire one node directly (a SOURCE_VIRTUAL from the UI, or a test-fire).
 *
 * A traversal simply STARTS at the node; nothing about it is special-cased any
 * more, so what firing does is just what that node does. On a SIGNAL_RX that is
 * "pretend this code was heard" — its output fires and nothing goes on air. On
 * a SIGNAL_TX it is a transmit, exactly as an inbound link would have caused. */
esp_err_t db_graph_fire_node(uint16_t node_id);

/* ---- wired inputs ----
 * Applies the GPIO configuration of every enabled SOURCE_GPIO node: installs
 * interrupts, pull-ups and debounce. Call after init and after any graph change
 * that touches a GPIO node. Idempotent — it reconciles against what is already
 * configured, and releases pins no longer referenced. Wired inputs are entirely
 * OPTIONAL: with no SOURCE_GPIO nodes this configures nothing and no pin is
 * touched. */
esp_err_t db_graph_apply_gpio_inputs(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_NODE_GRAPH_H */
