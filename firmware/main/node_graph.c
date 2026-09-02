/*
 * node_graph.c - The routing engine (see node_graph.h for the model itself).
 *
 * NVS LAYOUT ("dbgraph" namespace)
 *
 *     "nodes" -> [ grf_hdr_t { version, item_size, count } ][ db_node_t x count ]
 *     "links" -> [ grf_hdr_t { version, item_size, count } ][ db_link_t x count ]
 *
 * Two blobs, rewritten in full on every mutation. That is deliberate: the whole
 * graph is under 3 KB, a user edits it a handful of times in the life of the
 * box, and the alternative — per-node keys with an index — buys nothing but
 * partial-write failure modes. A doorbell that loses its wiring on power loss is
 * worse than useless, so a mutation is not "done" until nvs_commit() returns.
 *
 * ONE TASK OWNS EVERY TRAVERSAL. Triggers arrive from three very different
 * places: the RF capture task (a burst was received), an HTTP handler or the
 * MQTT bridge (fire a virtual node), and a GPIO interrupt (a wired button). All
 * three do nothing but post a small item to a queue; the graph task drains it
 * and does the walking. This buys four things at once:
 *
 *   - The capture task is never blocked by a sink. A transmit sink keys the
 *     radio for a couple of hundred milliseconds, and rf_service holds the radio
 *     mutex while capturing — running a transmit sink directly on the capture
 *     task would deadlock the box on its own radio lock.
 *   - Traversal state (the work queue, the visit bitmap) can be static rather
 *     than half a kilobyte of somebody else's stack, because it is never
 *     re-entered.
 *   - GPIO debounce happens in a task, where it can consult a clock and read the
 *     pin back, instead of in an ISR.
 *   - Two triggers that land at the same instant are serialized, so an ALL-group
 *     sees a coherent view of which of its inputs have fired.
 *
 * THE TRAVERSAL. Breadth-first over an explicit array — never recursion, whose
 * depth would be at the mercy of whatever graph a user drew. From every node
 * that is reached, ALL outgoing links are followed: fan-out is the whole point
 * (one press ringing two chimes, publishing to MQTT and arming a throttle), so
 * the link loop never breaks out early.
 *
 * THE CYCLE GUARD. A node is marked on ENTRY and is entered at most once per
 * traversal, which is what makes the walk terminate: at most DB_NODE_MAX nodes
 * are ever processed, no matter how the links are wired. A user who accidentally
 * wires A -> B -> A therefore gets one warning and a stopped branch instead of a
 * stack overflow. DB_GRAPH_MAX_DEPTH bounds a long acyclic chain on top of that.
 *
 * A -> A is refused outright by db_graph_add_link(); A -> B -> A is walked once
 * and stopped by the mark. What the mark cannot see is a cycle closed OVER THE
 * AIR — a signal.tx sends code A, and a signal.rx bound to code A hears it and
 * starts a fresh traversal. That one is rf_service's TX echo window's job, not
 * this file's. The split makes such a loop legible rather than accidental: it
 * is now two visibly different nodes with a wire between them, not one node
 * quietly feeding itself.
 *
 * Note what the mark does NOT do. It never stops a node from fanning out to all
 * of its children, and it never stops two different parents from reaching a
 * shared child — the crossing of every link is recorded before the mark is
 * consulted, which is precisely what an ALL-group needs to become satisfied by
 * its second inbound link within a single traversal. The mark is only consulted
 * to decide whether the child is entered a SECOND time, which is never wanted.
 *
 * A REPEAT IS A PARKED TRAVERSAL, NOT A SLEEP. logic.repeat never blocks the
 * graph task — one blocked task would stall every other press on the box. It
 * works with the grain of the walk instead: the event passes through
 * immediately, and the emissions still owed are parked as {node, trigger, due}
 * in a small fixed table. When an entry comes due the task starts a NEW
 * traversal whose START is the repeat node itself, and traverse() never calls
 * node_passes() on its own start — so the node does not re-arm itself from the
 * inside, and the walk simply runs into its children again. No resume flag, no
 * second task, no esp_timer: the one thing that changes is how long the task is
 * willing to wait on its queue.
 *
 * THE TRIGGER TRAVELS. Every traversal carries a db_trigger_t from its source to
 * every sink it reaches. A sink that knows only *that* it fired cannot produce a
 * useful MQTT payload, so the cause — signal, fingerprint, RSSI, decode, label —
 * is threaded through the logic nodes untouched. Sources that have no RF behind
 * them (wired, virtual, test-fire) synthesize one with signal_id 0.
 *
 * NOTHING IS IMPLICIT. This file transmits nothing and publishes nothing on its
 * own: it calls the handlers the app injected, and only for sink nodes the user
 * actually placed. A source with no outgoing links does nothing at all, and
 * SOURCE_ANY_RF has no effect whatsoever unless such a node exists.
 *
 * SINKS STAY INJECTED. This file includes no radio and no MQTT header, and never
 * will: it holds two function pointers registered by the app. That is the only
 * reason the engine can be reasoned about (and host-tested) on its own.
 */
#include "node_graph.h"

#include <stdio.h>
#include <string.h>

#include "board_pins.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "event_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_topic.h"
#include "node_migrate.h"
#include "nvs.h"

static const char *TAG = "db_graph";

#define DB_GRAPH_NS      "dbgraph"
#define DB_GRAPH_NODES   "nodes"
#define DB_GRAPH_LINKS   "links"
/*
 * Layout version of the two blobs.
 *
 *   v1 - the original nine types, 0 source.button ... 7 sink.transmit, 8 sink.mqtt.
 *   v2 - source.button and sink.transmit consolidated into one two-ported
 *        DB_NODE_SIGNAL. Slot 0 is unchanged (see node_graph.h), so the whole
 *        migration is "type 7 becomes type 0"; nothing else moves, and
 *        db_node_t / db_link_t are byte-identical to v1.
 *   v3 - that consolidation split again, into DB_NODE_SIGNAL_RX (slot 0, where
 *        the unified type sat) and DB_NODE_SIGNAL_TX (slot 11, appended). Slot 0
 *        is unchanged AGAIN, so a v2 node is already an rx node; the migration
 *        only has to spot the ones that were being used as senders, and it reads
 *        that off the links. db_node_t / db_link_t are still byte-identical.
 *   v4 - db_node_t gained `mqtt_enabled` (node_graph.h). THE FIRST VERSION THAT
 *        CHANGED THE RECORD ITSELF rather than the meaning of a type value, so
 *        it is also the first that needs a frozen copy of the old layout to read
 *        the stored bytes through — db_node_v3_t in node_migrate.h.
 *
 *        THE TRAP HERE IS THAT sizeof() DID NOT CHANGE. The new bool landed in
 *        a padding byte v3 was already wasting, so a v3 blob's item_size still
 *        equals sizeof(db_node_t) and load_blob's size check waves it through.
 *        The byte it now reads as `mqtt_enabled` is v3 PADDING — indeterminate,
 *        never written deliberately — so trusting it would silently drop a
 *        user's nodes off their broker on the first boot after an update. The
 *        version in the header, not the record size, is therefore what decides
 *        which layout the bytes are; see load_blob() and db_node_widen_v3().
 *
 * A blob whose version is OLDER than this is migrated on load and written back
 * in the current layout. A blob NEWER than this is refused, because guessing at
 * a layout from the future is how a downgrade eats a user's graph.
 */
#define DB_GRAPH_VERSION 4u

/* At most this many wired inputs. Fixed slots, never compacted: an ISR argument
 * is a slot index, and compacting the table under a live interrupt would point
 * an ISR at the wrong button. */
#define DB_GPIO_SLOTS 8

/* The highest GPIO this firmware will offer for a wired button. Everything above
 * it is either absent, spoken for by the flash/PSRAM on the ESP32-S3-N16R8 dev
 * board, or not brought out on the ESP32-S3 Zero the project must also run on.
 * This is the same band docs/API.md publishes as GET /api/gpio/available. */
#define DB_GPIO_USER_MAX 21

#define GRAPH_TASK_STACK 4096
#define GRAPH_TASK_PRIO  5
#define GRAPH_QUEUE_LEN  12

/* How many repeat sequences may be running at once. Fixed slots, statically
 * allocated, like everything else in this file — a doorbell must not be able to
 * malloc itself to death because a neighbour's remote is chattering.
 *
 * ONE SLOT PER RUNNING SEQUENCE, not one per queued emission: a slot carries how
 * many emissions are still owed and re-arms itself for the next one. That is why
 * a 20-times repeat costs a single slot, and why sixteen of them is a ceiling on
 * concurrently running repeat nodes rather than on rings. A graph may hold only
 * DB_NODE_MAX nodes in the first place, so the table can never be swamped by
 * distinct nodes — only by a graph that keeps re-triggering, and that restarts
 * its own slot instead of taking another. Overflow is reported rather than
 * hidden (see repeat_arm): a ring the user asked for that silently never happens
 * is a far worse bug than a full table. */
#define DB_REPEAT_SLOTS 16

/* How many times one event may cross INTO a repeat node before the engine calls
 * it a runaway. See repeat_arm() for why a hop count, and not a duplicate check,
 * is the right guard. This bounds cycles, never a legitimate sequence: the
 * emissions of one repeat run do not increment it. */
#define DB_REPEAT_MAX_HOPS DB_GRAPH_MAX_DEPTH

/* Bounds on logic.repeat's two parameters, enforced here as well as in the API
 * so a hand-written REST call or an older stored graph cannot arm a thousand
 * rings. `repeats` counts the immediate emission, so 1 means "no repeat". */
#define DB_REPEAT_MIN_TIMES 1u
#define DB_REPEAT_MAX_TIMES 20u
#define DB_REPEAT_DEF_TIMES 3u
#define DB_REPEAT_DEF_MS    5000u

/* How many monitor nodes may hold a hit ring at once.
 *
 * A ring is claimed on a node's FIRST hit, not when the node is created, so a
 * graph full of idle monitors costs nothing. Eight is well past what anyone
 * places while debugging one chain, and it bounds the table at 8 * (64 * 8 + 8)
 * bytes — about 4 KB of BSS, statically allocated like everything else here.
 * Sizing it to DB_NODE_MAX instead would triple that for slots that would never
 * be used. Overflow is logged once per node rather than hidden: a monitor whose
 * lamp never lights is a confusing bug, and the log says why. */
#define DB_MONITOR_SLOTS 8

/*
 * How the position of a logic.switch reaches flash.
 *
 * The problem this solves is stated in node_graph.h: a switch's position IS the
 * node's `enabled` flag, the nodes blob is rewritten wholesale on every
 * mutation, and a Home Assistant automation may flap that flag as fast as it
 * likes. Saving synchronously the way every other mutation does would put an
 * NVS write behind every toggle, which is a flash-wear bug waiting for the first
 * user who wires a switch to a motion sensor.
 *
 * So the switch path writes RAM and marks the blob dirty. The graph task flushes
 * it later, subject to two independent bounds:
 *
 *   DEBOUNCE  the position must have been STABLE this long. A run of toggles
 *             collapses to one write of the final value, and a switch that is
 *             flapping continuously is never written at all — there is no
 *             settled position to record.
 *   MIN_GAP   at least this long since the last switch-driven write, whatever
 *             the debounce says. This is the hard ceiling: no pattern of
 *             toggling, however adversarial, can make this path write NVS more
 *             often than once per interval.
 *
 * The cost of both is bounded staleness — a power cut within a couple of minutes
 * of a toggle comes back in the previous position. That is the right trade: the
 * box publishes its ACTUAL position retained on every connect, so Home Assistant
 * is never lied to, and the alternative is a doorbell that wears its flash out.
 */
#define DB_SWITCH_SAVE_DEBOUNCE_MS   10000u    /* stable for 10 s              */
#define DB_SWITCH_SAVE_MIN_GAP_MS   120000u    /* at most one write per 2 min  */

/* ---- persisted layout ---------------------------------------------------- */

typedef struct {
    uint32_t version;
    uint32_t item_size;
    uint32_t count;
} grf_hdr_t;

/*
 * How to read records written under an older layout than the current struct.
 *
 * A blob at or below `upto_version` holds records of `item_size` bytes, and
 * `widen` copies `n` of them out of the raw buffer into the current struct,
 * field by field. NULL means "this blob has only ever had one layout" — which is
 * still true of the links blob, and was true of the nodes blob until v4.
 */
typedef struct {
    uint32_t upto_version;
    size_t   item_size;
    void   (*widen)(void *dst, const void *src, int n);
} blob_legacy_t;

static void widen_nodes_v3(void *dst, const void *src, int n)
{
    db_node_widen_v3((db_node_t *)dst, src, n);
}

static const blob_legacy_t NODES_LEGACY = {
    .upto_version = 3u,
    .item_size    = sizeof(db_node_v3_t),
    .widen        = widen_nodes_v3,
};

/*
 * The two sizes coinciding is exactly why the version, not the size, decides
 * which layout a blob holds — see the DB_GRAPH_VERSION comment. This assertion
 * is not a requirement, it is a TRIPWIRE: if a later field makes them differ
 * this fires, and whoever is reading it needs to know only that the version
 * check above already handles both cases and this line can be deleted.
 */
_Static_assert(sizeof(db_node_v3_t) == sizeof(db_node_t),
               "v4 added mqtt_enabled into v3's padding; if that is no longer "
               "true, drop this assert — load_blob() keys off the version, not "
               "the record size, and is correct either way.");

/* ---- trigger queue ------------------------------------------------------- */

typedef enum {
    TRIG_RF = 0,   /* an RF burst: the matching signal node + source.any_rf     */
    TRIG_NODE,     /* arg = node id (UI/REST/MQTT fire, or a test-fire)          */
    TRIG_WIRED,    /* arg = node id, already debounced                           */
    TRIG_GPIO,     /* arg = GPIO slot index, straight from the ISR               */
    /*
     * Nothing to traverse — "look at your clock again".
     *
     * The graph task waits on this queue with portMAX_DELAY whenever nothing is
     * owed, and graph_wait_ticks() is evaluated BEFORE it blocks. So a deadline
     * that appears while it is already blocked (a switch moving, which arms a
     * deferred write) would never be noticed: the task would sleep until the
     * next press, and a position toggled on a quiet box would sit unwritten for
     * hours. This wakes it so it recomputes the wait. It is not a poll — it is
     * posted once, by the thing that created the deadline.
     */
    TRIG_WAKE,
} trig_kind_t;

typedef struct {
    uint8_t      kind;
    uint16_t     arg;
    db_trigger_t trig;
} queued_t;

/* ---- resident state ------------------------------------------------------ */

static db_node_t s_nodes[DB_NODE_MAX];
static int       s_node_count;
static db_link_t s_links[DB_LINK_MAX];
static int       s_link_count;

/* Runtime state, index-aligned with the arrays above and shifted with them:
 *   s_pass_us[i]  - when node i last let an event through (throttle)
 *   s_fired_us[i] - when link i last carried an event (group ALL) */
static int64_t s_pass_us[DB_NODE_MAX];
static int64_t s_fired_us[DB_LINK_MAX];

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static QueueHandle_t     s_queue;
static bool              s_ready;

static struct { db_sink_fn fn; void *ctx; } s_transmit, s_mqtt;

/* Told when an RF control signal moved a switch, so the retained MQTT position
 * can follow. NULL until the app registers one, and NULL forever on a box with
 * MQTT compiled out — the toggle itself does not depend on it. */
static db_graph_notify_fn s_switch_notify;

/* One staging buffer for both blobs; the node blob is the larger. */
static uint8_t s_blob[sizeof(grf_hdr_t) + DB_NODE_MAX * sizeof(db_node_t)];

/* Traversal scratch — static because exactly one task ever walks the graph. */
static struct { uint16_t id; uint8_t depth; } s_work[DB_NODE_MAX + 1];
static uint8_t s_seen[DB_NODE_MAX];

/*
 * Repeat sequences in flight: emissions a logic.repeat node still owes.
 *
 * node_id 0 marks a free slot, which is why the table needs no separate count
 * and no compaction. Entries are keyed by NODE ID rather than by array index
 * precisely because delete_node() shifts s_nodes[] under us — an index would
 * quietly come to mean a different node.
 *
 * The trigger is stored BY VALUE, not by reference. It is the whole reason the
 * feature is worth having: a repeated sink.mqtt must still publish which signal
 * caused it, and the burst it describes is long gone by the time the second ring
 * falls due. ~70 bytes per slot is a cheap price for a payload that means
 * something.
 *
 * A node owns at most ONE entry at a time. That is not an optimisation, it is
 * the "restart, don't stack" rule made structural: arming a repeat clears the
 * node's old entry first, so a leaned-on button can never accumulate runs.
 */
typedef struct {
    uint16_t     node_id;   /* the repeat node to resume from; 0 = free slot  */
    uint8_t      hops;      /* how many repeat nodes this event has crossed   */
    uint8_t      left;      /* emissions still owed, INCLUDING this one       */
    int64_t      due_us;    /* esp_timer_get_time() at which the next is due  */
    db_trigger_t trig;      /* what caused it, delivered unchanged on resume  */
} pending_repeat_t;

static pending_repeat_t s_repeat[DB_REPEAT_SLOTS];

/* Hop count of the traversal currently in flight: 0 for anything a source
 * started, and the parked entry's own count while a resume is running. Static
 * and unguarded is safe here for the same reason s_work and s_seen are — exactly
 * one task ever walks the graph, and a walk is never re-entered. */
static uint8_t s_repeat_hops;

/*
 * Monitor sinks: when each one was last reached. See node_graph.h for why this
 * never touches flash.
 *
 * Keyed by NODE ID for the same reason the repeat table is: delete_node()
 * shifts s_nodes[] under us, and an array index would quietly come to mean a
 * different node. node_id 0 marks a free slot, so no separate count is needed.
 *
 * `us` is kept ASCENDING — [0] oldest, [n-1] newest. An ordinary array rather
 * than a head/tail ring, because every operation this needs is cheaper on a
 * sorted array: pruning the expired ones is one memmove of at most 512 bytes,
 * on a path that only runs when a node actually fires, and the readout walks
 * backwards to produce newest-first without any modular arithmetic.
 */
typedef struct {
    uint16_t node_id;                 /* the monitor node; 0 = free slot     */
    uint8_t  n;                       /* hits held, <= DB_MONITOR_HITS       */
    int64_t  us[DB_MONITOR_HITS];     /* esp_timer_get_time(), ascending     */
} monitor_ring_t;

static monitor_ring_t s_monitor[DB_MONITOR_SLOTS];

/* Wired inputs. pin < 0 means the slot is free. */
typedef struct {
    int8_t   pin;
    uint16_t node_id;
    bool     active_low;
    uint16_t debounce_ms;
    int64_t  last_us;      /* last ACCEPTED edge, for the debounce lockout */
    uint32_t bounces;      /* edges swallowed by the lockout, for the log  */
} gpio_slot_t;

static gpio_slot_t s_gpio[DB_GPIO_SLOTS];
static bool        s_isr_service_installed;

/* Deferred persistence of switch positions. See DB_SWITCH_SAVE_* above.
 *   s_switch_dirty     a switch moved and flash does not know it yet
 *   s_switch_change_us when it last moved (restarts the debounce)
 *   s_switch_saved_us  when this path last wrote the blob (0 = not yet) */
static bool    s_switch_dirty;
static int64_t s_switch_change_us;
static int64_t s_switch_saved_us;

/* ---- helpers ------------------------------------------------------------- */

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static int node_index(uint16_t id)
{
    for (int i = 0; i < s_node_count; i++)
        if (s_nodes[i].id == id)
            return i;
    return -1;
}

static uint16_t next_free_node_id(void)
{
    for (uint16_t id = 1; id <= DB_NODE_MAX * 2; id++)
        if (node_index(id) < 0)
            return id;
    return 0;
}

/* The wire names from docs/API.md, so logs and JSON cannot drift apart. */
static const char *type_name(uint8_t t)
{
    switch (t) {
    case DB_NODE_SIGNAL_RX:      return "signal.rx";
    case DB_NODE_SIGNAL_TX:      return "signal.tx";
    case DB_NODE_SOURCE_GPIO:    return "source.gpio";
    case DB_NODE_SOURCE_VIRTUAL: return "source.virtual";
    case DB_NODE_SOURCE_ANY_RF:  return "source.any_rf";
    case DB_NODE_LOGIC_GROUP:    return "logic.group";
    case DB_NODE_LOGIC_THROTTLE: return "logic.throttle";
    case DB_NODE_LOGIC_REPEAT:   return "logic.repeat";
    case DB_NODE_LOGIC_SWITCH:   return "logic.switch";
    case DB_NODE_SINK_MQTT:      return "sink.mqtt";
    case DB_NODE_SINK_MONITOR:   return "sink.monitor";
    default:                     return "?";   /* incl. the retired slot 7 */
    }
}

/* A trigger for something with no RF behind it: a wired button, a virtual node
 * fired from the UI or MQTT, a test-fire. signal_id 0 is the honest answer, and
 * the label is what a sink has to work with. */
static void trigger_from_node(db_trigger_t *t, const db_node_t *n)
{
    memset(t, 0, sizeof(*t));
    snprintf(t->label, sizeof(t->label), "%s",
             (n && n->name[0]) ? n->name : "trigger");
}

/* ---- persistence --------------------------------------------------------- */

/* Caller holds the lock. */
static esp_err_t save_blob(const char *key, const void *items, size_t item_size,
                           int count)
{
    grf_hdr_t hdr = { .version   = DB_GRAPH_VERSION,
                      .item_size = (uint32_t)item_size,
                      .count     = (uint32_t)count };
    size_t len = sizeof(hdr) + (size_t)count * item_size;
    if (len > sizeof(s_blob))
        return ESP_ERR_INVALID_SIZE;

    memcpy(s_blob, &hdr, sizeof(hdr));
    if (count > 0)
        memcpy(s_blob + sizeof(hdr), items, (size_t)count * item_size);

    nvs_handle_t h;
    esp_err_t err = nvs_open(DB_GRAPH_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save %s: nvs_open: %s", key, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, key, s_blob, len);
    if (err == ESP_OK)
        err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
        ESP_LOGE(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    return err;
}

/* Caller holds the lock. Returns the number of items read (0 on any problem —
 * an unreadable graph is an empty graph, never a half-parsed one).
 *
 * *ver_out receives the layout version the blob was written in, or 0 when there
 * was no readable blob at all. The caller needs that to know whether a migration
 * is owed AND whether it may write back: "no blob" and "an old blob" look the
 * same in the item count when the graph is simply empty.
 *
 * `legacy` (may be NULL) describes the layout older blobs were written in. THE
 * VERSION IN THE HEADER DECIDES which layout applies, never the record size:
 * v4 added a bool into a padding byte, so a v3 record is the same NUMBER of
 * bytes as a v4 one while meaning something different in the byte that matters.
 * Sizing off item_size would read v3 padding as a live flag. */
static int load_blob(const char *key, void *items, size_t item_size, int max,
                     uint32_t *ver_out, const blob_legacy_t *legacy)
{
    if (ver_out)
        *ver_out = 0;

    nvs_handle_t h;
    if (nvs_open(DB_GRAPH_NS, NVS_READONLY, &h) != ESP_OK)
        return 0;

    size_t len = sizeof(s_blob);
    esp_err_t err = nvs_get_blob(h, key, s_blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len < sizeof(grf_hdr_t))
        return 0;

    grf_hdr_t hdr;
    memcpy(&hdr, s_blob, sizeof(hdr));

    /* A NEWER layout is refused outright: guessing at a layout from the future
     * is how a downgrade eats a user's graph. */
    if (hdr.version == 0 || hdr.version > DB_GRAPH_VERSION) {
        ESP_LOGW(TAG, "%s blob is layout v%u, this build reads up to v%u — ignored",
                 key, (unsigned)hdr.version, (unsigned)DB_GRAPH_VERSION);
        return 0;
    }

    /* Which layout the records are in, decided by the VERSION. */
    const blob_legacy_t *lg = NULL;
    size_t stored_size = item_size;
    if (legacy && hdr.version <= legacy->upto_version) {
        lg = legacy;
        stored_size = legacy->item_size;
    }

    if (hdr.item_size != stored_size) {
        ESP_LOGW(TAG, "%s blob is layout v%u with %u-byte records, expected %u — ignored",
                 key, (unsigned)hdr.version, (unsigned)hdr.item_size,
                 (unsigned)stored_size);
        return 0;
    }
    if (ver_out)
        *ver_out = hdr.version;

    int n = (hdr.count > (uint32_t)max) ? max : (int)hdr.count;
    if (len < sizeof(hdr) + (size_t)n * stored_size) {
        ESP_LOGE(TAG, "%s blob is truncated — ignored", key);
        return 0;
    }
    if (lg)
        lg->widen(items, s_blob + sizeof(hdr), n);
    else
        memcpy(items, s_blob + sizeof(hdr), (size_t)n * item_size);
    return n;
}

/* Caller holds the lock.
 *
 * Every path that writes the node array comes through here, which is why the
 * deferred switch write is cleared here too: an ordinary mutation (a node added,
 * renamed, deleted) has just persisted the whole array, switch positions
 * included, so there is nothing left owing. Without this the graph task would
 * wake later and write an identical blob for nothing. */
static esp_err_t save_nodes(void)
{
    esp_err_t err = save_blob(DB_GRAPH_NODES, s_nodes, sizeof(db_node_t), s_node_count);
    if (err == ESP_OK)
        s_switch_dirty = false;
    return err;
}

/* Caller holds the lock. */
static esp_err_t save_links(void)
{
    return save_blob(DB_GRAPH_LINKS, s_links, sizeof(db_link_t), s_link_count);
}

/* ---- migration chain -----------------------------------------------------
 *
 * Same shape as db_config.c and signal_store.c: the blob header carries the
 * layout it was written in, an older one is converted IN PLACE onto today's
 * structs, and the caller writes it back so the next boot reads it directly.
 *
 * v1 -> v2 is a single relabelling. db_node_t did not change, so there is no
 * frozen db_node_v1_t to copy field by field; only the meaning of one `type`
 * value did:
 *
 *     v1 slot 7 "sink.transmit"  ->  v2 slot 0 DB_NODE_SIGNAL
 *
 * Slot 0 was source.button and became DB_NODE_SIGNAL, which is exactly why
 * every stored button node needs no work at all — the consolidation was
 * designed around keeping that slot. Everything the migrated node needs is
 * already in the struct: signal_id names the same stored signal, and repeats /
 * gap_us still describe how its frame goes out. The node keeps its id, its
 * name, its position and every link, so a graph drawn under v1 comes back
 * looking the same, with one extra input port on the nodes that used to be
 * transmit sinks.
 *
 * v2 -> v3 splits that unified type back apart, into signal.rx (output only,
 * still slot 0) and signal.tx (input only, slot 11). Slot 0 not moving is again
 * what makes the common case free: a stored `signal` node IS an rx node
 * already, and the only question is which of them were being used the other way
 * round. The links answer it, because a two-ported node's wiring is the only
 * record of which port the user actually meant:
 *
 *     has an outgoing link   -> signal.rx   (something downstream of it)
 *     only incoming links    -> signal.tx   (it was the far end of a chain)
 *     no links at all        -> signal.rx   (slot 0 keeps it, nothing to do)
 *     BOTH directions        -> signal.rx, and SAY SO
 *
 * NOTHING IS INVENTED HERE. The both-directions node is the one case that
 * genuinely needs two nodes now, and this migration deliberately does not
 * create the second one or move any link onto it. A migration that rewires a
 * user's graph is far harder to trust than one that reports: the node is kept
 * as the receiver it most likely was, and the user is told — in the log for
 * whoever is watching a console, and as a DB_EV_SYSTEM event naming the node
 * for the user who is not — that it needs a sender adding.
 *
 * v3 -> v4 adds `mqtt_enabled`, and is the first step whose work does NOT
 * happen here. The two before it only ever changed what a `type` VALUE meant,
 * which is something you can do to an already-loaded array. This one changed the
 * RECORD, so it has to happen while the bytes are still being read: load_blob()
 * routes any blob at v3 or below through db_node_widen_v3(), which copies every
 * old field across and sets mqtt_enabled = true. By the time this function runs
 * the array is already in the v4 layout, which is why `case 3:` below has
 * nothing to do — see the DB_GRAPH_VERSION comment for why trusting the record
 * size instead would have quietly dropped nodes off the user's broker.
 *
 * ADDING v5: add a `case 4:` below and let `case 3:` fall INTO it, so a v1 blob
 * is carried forward through every step in turn rather than needing its own
 * shortcut. (`case 3:` breaks today only because it is the last one.) If v5
 * changes the record rather than the meaning of a value, freeze a db_node_v4_t
 * in node_migrate.h and extend NODES_LEGACY instead.
 *
 * Caller holds the lock. Returns how many nodes were changed.
 */
static int migrate_nodes(uint32_t from_version)
{
    int changed = 0;

    switch (from_version) {
    case 1:
        for (int i = 0; i < s_node_count; i++) {
            if (s_nodes[i].type != DB_NODE__RETIRED_TRANSMIT)
                continue;
            /* v1's sink.transmit became the two-ported v2 type at slot 0, which
             * v2 -> v3 below then sorts into rx or tx like any other. */
            s_nodes[i].type = DB_NODE_SIGNAL_RX;
            changed++;
            ESP_LOGI(TAG, "migrated node %u '%s': sink.transmit -> signal",
                     (unsigned)s_nodes[i].id, s_nodes[i].name);
        }
        /* Falls into the v2 -> v3 step, which then sorts these freshly retyped
         * nodes by their links exactly like any other stored signal node — a v1
         * sink.transmit only ever had incoming links, so it lands on tx. */
        __attribute__((fallthrough));
    case 2:
        for (int i = 0; i < s_node_count; i++) {
            /* Slot 0 held the unified `signal` type in v2 and holds signal.rx
             * now, so this is exactly "every stored signal node". */
            if (s_nodes[i].type != DB_NODE_SIGNAL_RX)
                continue;

            bool has_out = false, has_in = false;
            for (int k = 0; k < s_link_count; k++) {
                /* A link whose far end no longer exists says nothing about
                 * which way this node was used — the dangling-link sweep in
                 * db_graph_init() is about to drop it anyway. */
                if (s_links[k].from == s_nodes[i].id &&
                    node_index(s_links[k].to) >= 0)
                    has_out = true;
                if (s_links[k].to == s_nodes[i].id &&
                    node_index(s_links[k].from) >= 0)
                    has_in = true;
            }

            if (has_in && has_out) {
                ESP_LOGW(TAG, "node %u '%s' was wired BOTH ways as a signal node "
                              "and is now a receiver (signal.rx) only. Its "
                              "incoming link(s) no longer transmit — add a "
                              "Signal sender node for that code and wire them "
                              "into it instead.",
                         (unsigned)s_nodes[i].id, s_nodes[i].name);
                db_events_push(DB_EV_SYSTEM, s_nodes[i].signal_id, s_nodes[i].id,
                               0, 0, "\"%s\" needs a Signal sender",
                               s_nodes[i].name[0] ? s_nodes[i].name : "signal");
                continue;   /* stays rx: slot 0, nothing to write */
            }

            if (has_in) {
                s_nodes[i].type = DB_NODE_SIGNAL_TX;
                changed++;
                ESP_LOGI(TAG, "migrated node %u '%s': signal -> signal.tx "
                              "(only ever fed by a link)",
                         (unsigned)s_nodes[i].id, s_nodes[i].name);
            } else {
                ESP_LOGI(TAG, "node %u '%s': signal -> signal.rx",
                         (unsigned)s_nodes[i].id, s_nodes[i].name);
            }
        }
        __attribute__((fallthrough));
    case 3:
        /* v3 -> v4 is a LAYOUT change, and load_blob() has already applied it
         * via db_node_widen_v3(): every node in the array arrived here with
         * mqtt_enabled already set true. Nothing is left to do, and the case is
         * spelled out rather than omitted so the chain still reads as one step
         * per version and the next person adding `case 4:` has somewhere
         * obvious to hang it. */
        break;
    default:
        break;
    }

    return changed;
}

/* ---- repeat sequences ---------------------------------------------------- */

/* Interval between the emissions of a repeat node, in milliseconds. Same unit
 * convention as logic.throttle: milliseconds live in the struct, and a zero
 * window means "unset", not "instantly". Zero would also let repeat_service()
 * spin, because the next emission would be due the moment it was armed. */
static uint32_t repeat_interval_ms(const db_node_t *n)
{
    return n->window_ms ? n->window_ms : DB_REPEAT_DEF_MS;
}

/* How many emissions a repeat node makes in total, the immediate one included.
 * Clamped here as well as in the API: a graph written by an older firmware, or
 * by a hand-rolled curl, must not be able to arm a thousand rings. */
static uint8_t repeat_total(const db_node_t *n)
{
    uint32_t t = n->repeats ? n->repeats : DB_REPEAT_DEF_TIMES;
    if (t < DB_REPEAT_MIN_TIMES) t = DB_REPEAT_MIN_TIMES;
    if (t > DB_REPEAT_MAX_TIMES) t = DB_REPEAT_MAX_TIMES;
    return (uint8_t)t;
}

/* Forget the sequence a node still owes. Caller holds the lock.
 *
 * This is not housekeeping, it is correctness: an entry outlives the traversal
 * that created it, so a node deleted, disabled or re-typed mid-run would
 * otherwise keep ringing a chain the user has already taken apart. Returns how
 * many entries were dropped, for the log. */
static int repeat_cancel_node(uint16_t node_id)
{
    int dropped = 0;
    for (int i = 0; i < DB_REPEAT_SLOTS; i++) {
        if (s_repeat[i].node_id != node_id)
            continue;
        memset(&s_repeat[i], 0, sizeof(s_repeat[i]));
        dropped++;
    }
    return dropped;
}

/*
 * Start (or restart) the run of extra emissions a logic.repeat node owes after
 * the one that is passing through right now. Caller holds the lock and is the
 * graph task.
 *
 * RESTART, NEVER STACK. A new event arriving mid-run cancels what was left of
 * the previous run and begins again from this press. Anything else turns an
 * impatient visitor into a runaway: five presses of a "3 times" repeat would
 * otherwise leave fifteen rings queued, which is precisely the behaviour this
 * node exists to make safe.
 *
 * THE RUNAWAY GUARD, and why it is a hop count.
 *
 * Every resumed emission is a BRAND NEW traversal with a fresh s_seen, so the
 * per-traversal cycle mark that protects the rest of this engine buys nothing
 * across them: a user who wires repeat -> ... -> repeat (or, through the REST
 * API, anything -> back into the same repeat) would get a chain of traversals
 * that re-arms itself for ever, at whatever rate the interval sets. Nothing else
 * in the box would notice; it would just ring at 3 a.m. until someone pulled the
 * power.
 *
 * So each entry carries how many repeat nodes the event has already crossed, and
 * a resumed traversal passes that count on to anything it arms. A fresh press
 * always starts at zero, a cycle increments on every lap, and the run is refused
 * once it reaches DB_REPEAT_MAX_HOPS. The emissions WITHIN one run never touch
 * the count, so a legitimate 20-times repeat is never cut short.
 *
 * The tempting alternative - "refuse if this node already has an entry for this
 * trigger" - was rejected because it cannot tell a cycle from an ordinary second
 * press. Two presses of the same wired button produce byte-identical triggers
 * (trigger_from_node zeroes everything but the label), so that rule would drop
 * the user's real second ring, while a cycle built from two DIFFERENT repeat
 * nodes would sail straight through it. A hop count gets both cases right.
 *
 * The price is paid by NESTED repeats, and it is the right price. In a chain of
 * repeat A -> repeat B, each of A's emissions restarts B one hop further along,
 * so a very long A eventually spends the budget and B stops repeating. Resetting
 * the count per node would remove that ceiling — and with it the only thing
 * bounding a mutual A -> B -> A cycle, which is a graph that rings for ever. A
 * bounded surprise beats an unbounded one at three in the morning.
 */
static void repeat_arm(const db_node_t *n, int64_t now, const db_trigger_t *trig)
{
    uint8_t  total  = repeat_total(n);
    uint32_t int_ms = repeat_interval_ms(n);

    /* Whatever this node still owed belongs to a press the user has superseded.
     * Done before the hop check too, so even a refused run leaves no stale
     * sequence ticking away behind it. */
    repeat_cancel_node(n->id);

    if (total <= 1)
        return;   /* "1 time" is the immediate emission and nothing more */

    if (s_repeat_hops >= DB_REPEAT_MAX_HOPS) {
        ESP_LOGW(TAG, "repeat '%s' not re-armed: this event has already crossed "
                      "%u repeat node(s) - the wiring feeds a repeat back into "
                      "itself, which stops here",
                 n->name, (unsigned)s_repeat_hops);
        db_events_push(DB_EV_SYSTEM, 0, n->id, 0, 0,
                       "Repeat loop stopped at \"%s\"",
                       n->name[0] ? n->name : "repeat");
        return;
    }

    int slot = -1;
    for (int i = 0; i < DB_REPEAT_SLOTS; i++)
        if (s_repeat[i].node_id == 0) { slot = i; break; }

    if (slot < 0) {
        /* Drop the NEWEST run rather than evicting one already promised: the
         * sequences ahead of it are closer to their next ring, and stealing a
         * slot would turn one lost repeat into several. Say so both ways round -
         * the log for whoever is watching a console, an event for the user who
         * is not. The immediate emission has already happened, so the press is
         * not lost; only its repeats are. */
        ESP_LOGW(TAG, "repeat '%s' dropped its repeats: all %d slots are running",
                 n->name, DB_REPEAT_SLOTS);
        db_events_push(DB_EV_SYSTEM, trig->signal_id, n->id, trig->rssi_dbm, 0,
                       "Repeat \"%s\" full - repeats dropped",
                       n->name[0] ? n->name : "repeat");
        return;
    }

    s_repeat[slot].node_id = n->id;
    s_repeat[slot].hops    = (uint8_t)(s_repeat_hops + 1);
    s_repeat[slot].left    = (uint8_t)(total - 1);   /* the immediate one is done */
    s_repeat[slot].due_us  = now + (int64_t)int_ms * 1000;
    s_repeat[slot].trig    = *trig;

    ESP_LOGI(TAG, "repeat '%s': '%s' fired, %u more %lums apart",
             n->name, trig->label, (unsigned)(total - 1), (unsigned long)int_ms);
}

/* Earliest due time of anything owed, or 0 when nothing is running. Caller holds
 * the lock. */
static int64_t repeat_next_due_us(void)
{
    int64_t best = 0;
    for (int i = 0; i < DB_REPEAT_SLOTS; i++) {
        if (s_repeat[i].node_id == 0)
            continue;
        if (best == 0 || s_repeat[i].due_us < best)
            best = s_repeat[i].due_us;
    }
    return best;
}

/* ---- monitor sinks ------------------------------------------------------- */

uint16_t db_graph_monitor_hold_s(const db_node_t *n)
{
    if (!n)
        return (uint16_t)DB_MONITOR_HOLD_DEF_S;
    uint32_t s = n->window_ms / 1000u;
    if (s == 0)                     s = DB_MONITOR_HOLD_DEF_S;  /* unset */
    if (s < DB_MONITOR_HOLD_MIN_S)  s = DB_MONITOR_HOLD_MIN_S;
    if (s > DB_MONITOR_HOLD_MAX_S)  s = DB_MONITOR_HOLD_MAX_S;
    return (uint16_t)s;
}

/* Forget everything a monitor node has seen and give its slot back. Caller
 * holds the lock. Same contract as repeat_cancel_node(), and needed for the
 * same reason: node ids are reused, so a ring left behind by a deleted node
 * would surface as somebody else's history. Returns 1 if a slot was freed. */
static int monitor_clear_node(uint16_t node_id)
{
    for (int i = 0; i < DB_MONITOR_SLOTS; i++) {
        if (s_monitor[i].node_id != node_id)
            continue;
        memset(&s_monitor[i], 0, sizeof(s_monitor[i]));
        return 1;
    }
    return 0;
}

/*
 * Record that a monitor node was reached. Caller holds the lock and is the
 * graph task.
 *
 * PRUNE FIRST, ALWAYS. Anything past the retention window is dropped before the
 * new hit is stored, so a busy band cannot push ten minutes of history out of
 * the ring in a few seconds — the oldest thing in the buffer is never older
 * than the window the UI claims to be showing. The per-node cap is the second
 * bound and only bites inside the window: past DB_MONITOR_HITS hits in ten
 * minutes the oldest is dropped, because a lamp and a timeline do not get more
 * useful past sixty-odd marks.
 */
static void monitor_record(const db_node_t *n, int64_t now)
{
    int slot = -1, free_slot = -1;
    for (int i = 0; i < DB_MONITOR_SLOTS; i++) {
        if (s_monitor[i].node_id == n->id) { slot = i; break; }
        if (s_monitor[i].node_id == 0 && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        if (free_slot < 0) {
            /* Rate-limited by nature: this only speaks when a monitor fires
             * that has no ring, and there are at most DB_NODE_MAX of those. */
            ESP_LOGW(TAG, "monitor '%s': no free slot (max %d monitor nodes "
                          "record hits at once) - its timeline stays empty",
                     n->name, DB_MONITOR_SLOTS);
            return;
        }
        slot = free_slot;
        s_monitor[slot].node_id = n->id;
        s_monitor[slot].n       = 0;
    }

    monitor_ring_t *m = &s_monitor[slot];
    int64_t cutoff = now - (int64_t)DB_MONITOR_RETENTION_S * 1000000;

    int drop = 0;
    while (drop < m->n && m->us[drop] < cutoff)
        drop++;
    if (drop > 0) {
        memmove(m->us, m->us + drop, (size_t)(m->n - drop) * sizeof(m->us[0]));
        m->n = (uint8_t)(m->n - drop);
    }
    if (m->n >= DB_MONITOR_HITS) {
        memmove(m->us, m->us + 1, (size_t)(DB_MONITOR_HITS - 1) * sizeof(m->us[0]));
        m->n = DB_MONITOR_HITS - 1;
    }
    m->us[m->n++] = now;

    ESP_LOGD(TAG, "monitor '%s' hit (%u in the last %ds)",
             n->name, (unsigned)m->n, DB_MONITOR_RETENTION_S);
}

int db_graph_monitor_hits(uint16_t node_id, int64_t *out_us, int max)
{
    if (!out_us || max <= 0)
        return 0;

    lock();
    int written = 0;
    for (int i = 0; i < DB_MONITOR_SLOTS; i++) {
        if (s_monitor[i].node_id != node_id)
            continue;
        const monitor_ring_t *m = &s_monitor[i];
        /* Expired hits are pruned on insert, so a monitor that stopped firing
         * would keep reporting stale marks until it fired again. Filter here
         * too — the readout must never claim something happened inside a window
         * it did not. */
        int64_t cutoff = esp_timer_get_time() -
                         (int64_t)DB_MONITOR_RETENTION_S * 1000000;
        for (int k = m->n - 1; k >= 0 && written < max; k--) {
            if (m->us[k] < cutoff)
                break;              /* ascending, so everything below is older */
            out_us[written++] = m->us[k];
        }
        break;
    }
    unlock();
    return written;
}

/* ---- logic.switch -------------------------------------------------------- */

/*
 * Note what is NOT here: any code in node_passes() or traverse() for the switch
 * type. There does not need to be. traverse() already refuses to enter a node
 * whose `enabled` is false — and refuses before it even records the link
 * crossing, so a blocked branch leaves no trace for an ALL-group downstream to
 * mistake for an arrival. A switch's position being that same flag is exactly
 * what makes "a switch in the wire" free: OFF is a node the walk does not enter,
 * which is what "does not conduct" means.
 */

/* A switch moved. Caller holds the lock. Arms the deferred write; never touches
 * flash itself.
 *
 * The wake-up is not optional. This runs on an HTTP worker or the MQTT bridge
 * task, while the graph task is asleep on its queue with a timeout it worked out
 * BEFORE this deadline existed. Without the nudge a box that nobody rings would
 * keep the new position in RAM only — and lose it to the next power cut, which
 * is exactly the failure this whole deferred scheme exists to avoid. A full
 * queue is harmless: the task is plainly awake and will recompute its wait when
 * it drains. */
static void switch_mark_dirty(void)
{
    s_switch_dirty     = true;
    s_switch_change_us = esp_timer_get_time();

    if (s_queue) {
        queued_t q;
        memset(&q, 0, sizeof(q));
        q.kind = TRIG_WAKE;
        xQueueSend(s_queue, &q, 0);
    }
}

/* When the pending switch write becomes due, or 0 when nothing is pending.
 * Caller holds the lock. */
static int64_t switch_save_due_us(void)
{
    if (!s_switch_dirty)
        return 0;
    int64_t debounce = s_switch_change_us + (int64_t)DB_SWITCH_SAVE_DEBOUNCE_MS * 1000;
    int64_t floor_us = s_switch_saved_us
                           ? s_switch_saved_us + (int64_t)DB_SWITCH_SAVE_MIN_GAP_MS * 1000
                           : 0;
    return debounce > floor_us ? debounce : floor_us;
}

/* Write the graph back if a switch position is owed and both bounds are
 * satisfied. Caller holds the lock and is the graph task.
 *
 * A failed save leaves the dirty flag set on purpose: RAM is already correct, so
 * the only thing lost is durability, and the next attempt is one interval away
 * rather than never. */
static void switch_save_service(int64_t now)
{
    int64_t due = switch_save_due_us();
    if (due == 0 || now < due)
        return;

    if (save_nodes() == ESP_OK) {
        s_switch_dirty    = false;
        s_switch_saved_us = now;
        ESP_LOGI(TAG, "switch positions written to flash");
    } else {
        /* Do not retry in a tight loop against a failing NVS. */
        s_switch_saved_us = now;
    }
}

/*
 * Move one switch, by array index. Caller holds the lock. Returns whether the
 * position actually changed.
 *
 * Split out of db_graph_switch_set() so the RF control path below can reuse it
 * from INSIDE the lock it already holds — s_lock is a plain mutex, not a
 * recursive one, so calling the public function there would deadlock the graph
 * task on itself. Reusing this rather than writing `enabled` directly is what
 * keeps the deferred-write logic on every path that moves a switch: a fob
 * hammered by a child must be no worse for the flash than a Home Assistant
 * automation is.
 */
static bool switch_set_locked(int i, bool on)
{
    if (s_nodes[i].enabled == on)
        return false;
    s_nodes[i].enabled = on;
    switch_mark_dirty();
    ESP_LOGI(TAG, "switch '%s' (node %u) is now %s",
             s_nodes[i].name, (unsigned)s_nodes[i].id, on ? "ON" : "OFF");
    return true;
}

esp_err_t db_graph_switch_set(uint16_t node_id, bool on)
{
    lock();
    int i = node_index(node_id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (s_nodes[i].type != DB_NODE_LOGIC_SWITCH) {
        unlock();
        return ESP_ERR_INVALID_ARG;
    }
    switch_set_locked(i, on);
    unlock();
    return ESP_OK;
}

void db_graph_set_switch_notify_handler(db_graph_notify_fn fn)
{
    s_switch_notify = fn;
    ESP_LOGI(TAG, "switch notify handler %s", fn ? "registered" : "cleared");
}

/*
 * A stored code was heard (or simulated). TOGGLE every switch node that names it
 * as its control signal. Caller holds the lock; returns how many moved.
 *
 * A CONTROL ACTION, NOT A TRAVERSAL. Nothing is walked and nothing is injected
 * into the switch's outputs: the point of the feature is to change whether the
 * wire conducts, not to push an event down it. A remote that both flipped the
 * switch and rang through it would be indistinguishable from a broken switch.
 *
 * BEFORE THE TRAVERSALS, on purpose. Within one burst the control action lands
 * first, so a press never walks a switch it is in the middle of moving and the
 * position the log reports is the position the same burst's chains saw.
 */
static int switch_control_on_signal(uint16_t heard_id)
{
    int moved = 0;
    if (heard_id == 0)
        return 0;
    for (int i = 0; i < s_node_count; i++) {
        if (!db_switch_reacts_to(&s_nodes[i], heard_id))
            continue;
        /* Toggle: one button, both directions. See db_switch_reacts_to() in
         * node_graph.h for why the switch's own position is not a gate here. */
        if (switch_set_locked(i, !s_nodes[i].enabled)) {
            moved++;
            db_events_push(DB_EV_SYSTEM, heard_id, s_nodes[i].id, 0, 0,
                           "signal toggled switch \"%s\" %s", s_nodes[i].name,
                           s_nodes[i].enabled ? "ON" : "OFF");
        }
    }
    return moved;
}

/* Both addressable types resolve their suffix here — see node_graph.h. */
void db_graph_node_suffix(const db_node_t *n, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    if (!n) { out[0] = '\0'; return; }
    db_mqtt_node_suffix(n->topic, n->name, out, outsz);
}

/*
 * MATCHED BY RESOLVED SUFFIX, NOT BY THE RAW `topic` FIELD.
 *
 * This is the bug that started the whole feature's trouble. The bridge
 * subscribes on db_graph_node_suffix() — explicit topic, else a slug of the
 * name — but this function used to compare `s_nodes[i].topic` directly. A switch
 * called "All Bells Switch" with no topic typed therefore subscribed on
 * "all_bells_switch" and got a Home Assistant entity, while every command that
 * arrived looked for a node whose topic field equalled "all_bells_switch" and
 * found none. The entity existed, could not be commanded, and its retained state
 * was never published, because db_graph_switch_topic_state() below had the same
 * flaw and reported "no such topic".
 *
 * Both now ask db_graph_node_suffix(), which is the same function the bridge
 * asks. There is one rule and one implementation of it.
 */
int db_graph_switch_set_topic(const char *topic, bool on)
{
    if (!topic || !topic[0])
        return 0;

    lock();
    int moved = 0, matched = 0;
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].type != DB_NODE_LOGIC_SWITCH)
            continue;
        /* A node the user has taken off MQTT is not commandable FROM MQTT, even
         * when it shares a topic with one that is still exposed. This is the
         * only caller's whole purpose, so the check belongs here rather than
         * being owed by every caller. */
        if (!s_nodes[i].mqtt_enabled)
            continue;
        char sfx[DB_NODE_TOPIC_MAX];
        db_graph_node_suffix(&s_nodes[i], sfx, sizeof(sfx));
        if (strcmp(sfx, topic) != 0)
            continue;
        matched++;
        if (s_nodes[i].enabled == on)
            continue;
        s_nodes[i].enabled = on;
        moved++;
    }
    if (moved)
        switch_mark_dirty();
    unlock();

    if (matched)
        ESP_LOGI(TAG, "switch topic '%s' -> %s (%d node(s), %d moved)",
                 topic, on ? "ON" : "OFF", matched, moved);
    return matched;
}

bool db_graph_switch_topic_state(const char *topic, bool *found_out)
{
    if (found_out)
        *found_out = false;
    if (!topic || !topic[0])
        return false;

    lock();
    bool found = false, any_on = false;
    for (int i = 0; i < s_node_count; i++) {
        if (s_nodes[i].type != DB_NODE_LOGIC_SWITCH)
            continue;
        /* Same rule as db_graph_switch_set_topic(): the position reported to the
         * broker is the position of the nodes the broker can actually reach,
         * matched on the RESOLVED suffix so a name-derived topic reports a state
         * instead of looking like a topic no node carries. */
        if (!s_nodes[i].mqtt_enabled)
            continue;
        char sfx[DB_NODE_TOPIC_MAX];
        db_graph_node_suffix(&s_nodes[i], sfx, sizeof(sfx));
        if (strcmp(sfx, topic) != 0)
            continue;
        found = true;
        if (s_nodes[i].enabled)
            any_on = true;
    }
    unlock();

    if (found_out)
        *found_out = found;
    return any_on;
}

/* ---- gating logic -------------------------------------------------------- */

/*
 * Decide whether an event reaching `n` continues past it. Sources and sinks
 * always pass; the logic types are the whole reason this function exists.
 *
 * `trig` is needed because passing is not always the end of it: a logic.repeat
 * lets the event through AND owes it again later, so it has to keep a copy of
 * what caused it.
 *
 * Caller holds the lock and is the graph task.
 */
static bool node_passes(db_node_t *n, int idx, int64_t now, const db_trigger_t *trig)
{
    switch (n->type) {
    case DB_NODE_LOGIC_GROUP: {
        if (n->group_mode != DB_GROUP_ALL)
            return true;   /* ANY: the first input through fires it */

        /* ALL: every inbound link must have carried an event inside the window.
         * The window is what makes "all of them" a usable idea at all — nobody
         * presses three buttons at the same instant. */
        uint32_t win_ms = n->window_ms ? n->window_ms : 1000u;
        int inbound = 0;
        for (int i = 0; i < s_link_count; i++) {
            if (s_links[i].to != n->id)
                continue;
            inbound++;
            if (s_fired_us[i] == 0 ||
                (now - s_fired_us[i]) > (int64_t)win_ms * 1000)
                return false;
        }
        if (inbound == 0)
            return false;   /* an ALL group with no inputs can never be satisfied */

        /* Satisfied: consume the inputs so the group re-arms rather than firing
         * again on the next event from any single one of them. */
        for (int i = 0; i < s_link_count; i++)
            if (s_links[i].to == n->id)
                s_fired_us[i] = 0;
        ESP_LOGI(TAG, "group '%s' satisfied (%d inputs within %lums)",
                 n->name, inbound, (unsigned long)win_ms);
        return true;
    }

    case DB_NODE_LOGIC_THROTTLE: {
        uint32_t win_ms = n->window_ms ? n->window_ms : 10000u;   /* 10 s default */
        if (s_pass_us[idx] != 0 &&
            (now - s_pass_us[idx]) < (int64_t)win_ms * 1000) {
            ESP_LOGI(TAG, "throttle '%s' dropped an event (%lums window)",
                     n->name, (unsigned long)win_ms);
            return false;
        }
        s_pass_us[idx] = now;
        return true;
    }

    case DB_NODE_LOGIC_REPEAT:
        /* "Yes, and again later." The first ring must be immediate — a doorbell
         * that makes a visitor wait out an interval before anything happens is
         * broken — so this returns true and the traversal walks straight on into
         * the children. What is scheduled is only what is still OWED.
         *
         * Those emissions are picked up by repeat_service(), which starts a
         * fresh traversal FROM this node. traverse() does not gate its own
         * start, so we are not consulted again for them: the node cannot re-arm
         * itself from the inside, and each resume simply rings the children
         * once more. */
        repeat_arm(n, now, trig);
        return true;

    default:
        return true;
    }
}

/*
 * Perform whatever a node DOES on being reached. Only sinks act — signal.tx
 * among them, since transmitting is exactly what a sink does; sources and logic
 * nodes exist to route. Caller holds the lock and is the graph task.
 *
 * NO CONTEXT IS CONSULTED. This function is a pure function of the NODE, which
 * it was not while one DB_NODE_SIGNAL type had to serve both directions: it
 * took an `is_start` flag and transmitted only when the node had been reached
 * over a link, because otherwise every reception would have re-sent what it had
 * just heard. That flag is gone with the type it existed for. signal.rx has no
 * case here at all — being heard is not an action, it is the start of one — and
 * signal.tx has one meaning on every path that reaches it.
 *
 * If a future type ever wants to know how it was entered, give it two types
 * instead. That is the lesson this file already paid for once.
 */
static void node_act(const db_node_t *n, const db_trigger_t *trig)
{
    switch (n->type) {
    case DB_NODE_SIGNAL_TX:
        if (!s_transmit.fn) {
            ESP_LOGW(TAG, "node %u '%s' wants to transmit but no handler is "
                          "registered", (unsigned)n->id, n->name);
            return;
        }
        ESP_LOGI(TAG, "signal.tx: node %u '%s' -> signal %u x%u (from '%s')",
                 (unsigned)n->id, n->name, (unsigned)n->signal_id, n->repeats,
                 trig->label);
        db_events_push(DB_EV_NODE_FIRED, n->signal_id, n->id, trig->rssi_dbm,
                       n->repeats, "%s", n->name[0] ? n->name : "signal sender");
        s_transmit.fn(n, trig, s_transmit.ctx);
        break;

    case DB_NODE_SINK_MQTT:
        if (!s_mqtt.fn) {
            ESP_LOGD(TAG, "node %u '%s': no MQTT handler registered",
                     (unsigned)n->id, n->name);
            return;
        }
        ESP_LOGI(TAG, "sink mqtt: node %u '%s' -> '%s' (from '%s')",
                 (unsigned)n->id, n->name, n->topic, trig->label);
        db_events_push(DB_EV_NODE_FIRED, trig->signal_id, n->id, trig->rssi_dbm,
                       trig->repeats, "%s", n->name[0] ? n->name : "mqtt");
        s_mqtt.fn(n, trig, s_mqtt.ctx);
        break;

    case DB_NODE_SINK_MONITOR:
        /* The one sink with no handler and no side effect. It notes the time
         * and stops — no radio, no broker, nothing injected, which is exactly
         * what makes it safe to drop into a live chain to see whether that
         * chain fires.
         *
         * Deliberately NOT pushed to the event log either. The event ring holds
         * 48 entries shared by the whole box, and a monitor left wired to
         * source.any_rf on a busy band would evict every real event with copies
         * of "the monitor fired". Its history has a home of its own that the
         * user asked for: GET /api/monitor. */
        monitor_record(n, esp_timer_get_time());
        break;

    default:
        break;
    }
}

/*
 * Walk forward from one node, carrying `trig` to everything reached. See the
 * file header for the fan-out and cycle-guard rules.
 *
 * THE START NODE IS NOT GATED. node_passes() is asked about the TARGET of a
 * link, never about start_id — a source has nothing to gate, and a test-fire
 * must fire. That was always true; logic.repeat now leans on it, because each
 * owed emission is a traversal starting AT the repeat node, and the node must
 * step aside rather than restart its own run every time it rings. Do not "tidy"
 * this by gating the start.
 *
 * The start node is otherwise ORDINARY. node_act() used to treat it specially,
 * because the old two-ported signal type needed to know which of its jobs it
 * was doing; signal.rx and signal.tx each do one, so nothing downstream of here
 * cares where the walk began any more.
 *
 * Caller holds the lock and is the graph task.
 */
static void traverse(uint16_t start_id, const db_trigger_t *trig)
{
    int start = node_index(start_id);
    if (start < 0 || !s_nodes[start].enabled)
        return;

    memset(s_seen, 0, sizeof(s_seen));
    s_seen[start] = 1;

    int head = 0, tail = 0;
    s_work[tail].id    = start_id;
    s_work[tail].depth = 0;
    tail++;

    bool loop_reported = false;
    int64_t now = esp_timer_get_time();

    while (head < tail) {
        uint16_t id    = s_work[head].id;
        uint8_t  depth = s_work[head].depth;
        head++;

        int idx = node_index(id);
        if (idx < 0)
            continue;
        db_node_t *n = &s_nodes[idx];
        if (!n->enabled)
            continue;

        node_act(n, trig);

        if (depth >= DB_GRAPH_MAX_DEPTH) {
            ESP_LOGW(TAG, "traversal stopped at node %u '%s': depth limit %d "
                          "reached — the chain is longer than this engine walks",
                     (unsigned)id, n->name, DB_GRAPH_MAX_DEPTH);
            continue;
        }

        /* Follow EVERY outgoing link. No early exit: fan-out is a primary use
         * case, not an edge case. */
        for (int i = 0; i < s_link_count; i++) {
            if (s_links[i].from != id)
                continue;

            int t = node_index(s_links[i].to);
            if (t < 0 || !s_nodes[t].enabled)
                continue;

            /* Record the crossing BEFORE anything can skip this link: an
             * ALL-group needs to know its other inputs arrived, including on the
             * passes where it does not fire. */
            s_fired_us[i] = now;

            if (s_seen[t]) {
                /* Already entered in this traversal. Either the user wired a
                 * loop, or two paths converge here — both mean "do not enter it
                 * a second time", and the loop case is why this guard exists at
                 * all. */
                if (!loop_reported) {
                    loop_reported = true;
                    ESP_LOGW(TAG, "node %u '%s' was already reached in this "
                                  "traversal (link %u -> %u): not re-entering — "
                                  "either two paths converge here, or the wiring "
                                  "contains a loop, which stops here.",
                             (unsigned)s_nodes[t].id, s_nodes[t].name,
                             (unsigned)s_links[i].from, (unsigned)s_links[i].to);
                    if (s_nodes[t].id == start_id)
                        db_events_push(DB_EV_SYSTEM, 0, start_id, 0, 0,
                                       "Graph loop back to node %u",
                                       (unsigned)start_id);
                }
                continue;
            }

            if (!node_passes(&s_nodes[t], t, now, trig))
                continue;

            if (tail >= (int)(sizeof(s_work) / sizeof(s_work[0]))) {
                ESP_LOGW(TAG, "traversal work queue full — graph too dense");
                continue;
            }
            s_seen[t] = 1;
            s_work[tail].id    = s_nodes[t].id;
            s_work[tail].depth = (uint8_t)(depth + 1);
            tail++;
        }
    }
}

/*
 * Ring whatever is due, oldest first, and re-arm the sequences that owe more.
 * Caller holds the lock and is the graph task.
 *
 * Order matters twice over. The entry is copied and its slot settled BEFORE the
 * traversal runs, because that traversal may arm repeats of its own and must see
 * a coherent table. And entries are taken in due order, so two repeat nodes that
 * both came due while the task was busy still ring in the sequence the user
 * wired them.
 *
 * DRIFT IS CORRECTED, NOT ACCUMULATED. The next emission is due one interval
 * after the one just made was DUE, not after it actually ran, so a run of ten
 * rings does not slowly slide later because the task was busy. If the box was so
 * busy that the corrected time is already past, it falls back to now + interval
 * rather than firing the rest of the sequence back to back.
 *
 * This loop terminates. Every entry it writes is due strictly later than the
 * `now` it was called with, so nothing it creates can satisfy the same pass -
 * the hop guard in repeat_arm() is about laps across time, this is about not
 * spinning inside one wake-up.
 */
static void repeat_service(int64_t now)
{
    for (;;) {
        int best = -1;
        for (int i = 0; i < DB_REPEAT_SLOTS; i++) {
            if (s_repeat[i].node_id == 0 || s_repeat[i].due_us > now)
                continue;
            if (best < 0 || s_repeat[i].due_us < s_repeat[best].due_us)
                best = i;
        }
        if (best < 0)
            return;

        pending_repeat_t e = s_repeat[best];

        /* The node may have gone away or been switched off in the seconds this
         * run spent waiting. The mutation paths cancel their own entries, so
         * this is the belt to their braces rather than the only check. */
        int idx = node_index(e.node_id);
        if (idx < 0 || !s_nodes[idx].enabled) {
            memset(&s_repeat[best], 0, sizeof(s_repeat[best]));
            continue;
        }

        if (e.left > 1) {
            int64_t next = e.due_us + (int64_t)repeat_interval_ms(&s_nodes[idx]) * 1000;
            if (next <= now)
                next = now + (int64_t)repeat_interval_ms(&s_nodes[idx]) * 1000;
            s_repeat[best].left   = (uint8_t)(e.left - 1);
            s_repeat[best].due_us = next;
        } else {
            memset(&s_repeat[best], 0, sizeof(s_repeat[best]));
        }

        ESP_LOGI(TAG, "repeat '%s': emission for '%s' (%u left after this)",
                 s_nodes[idx].name, e.trig.label, (unsigned)(e.left - 1));

        /* Carry the hop count into the resumed traversal so anything IT arms
         * counts as one repeat node further from the press that started all
         * this. The emissions of this run do not increment it. */
        s_repeat_hops = e.hops;
        traverse(e.node_id, &e.trig);
        s_repeat_hops = 0;
    }
}

/* A wired button press, already debounced. Runs on the graph task. */
static void fire_wired(uint16_t node_id)
{
    int idx = node_index(node_id);
    if (idx < 0)
        return;

    db_trigger_t trig;
    trigger_from_node(&trig, &s_nodes[idx]);
    db_events_push(DB_EV_WIRED_PRESS, 0, node_id, 0, 0, "%s", trig.label);
    traverse(node_id, &trig);
}

/* ---- wired inputs -------------------------------------------------------- */

/*
 * The ISR does one thing: name the slot that fired. No timing, no filtering, no
 * logging — the debounce needs a clock and a pin read-back, and neither belongs
 * in an interrupt.
 */
static void IRAM_ATTR gpio_edge_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    queued_t q;
    memset(&q, 0, sizeof(q));
    q.kind = TRIG_GPIO;
    q.arg  = (uint16_t)(uintptr_t)arg;
    xQueueSendFromISR(s_queue, &q, &hp);
    if (hp == pdTRUE)
        portYIELD_FROM_ISR();
}

/* Why a pin may not be used for a wired button. Returns NULL when it may. */
static const char *pin_rejection(int pin)
{
    if (pin < 0)
        return "unset";
    if (!GPIO_IS_VALID_GPIO(pin))
        return "not a GPIO on this chip";
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin))
        return "input-only: no internal pull-up, so a button would float";

    if (pin == (int)DB_PIN_CC1101_GDO0 || pin == (int)DB_PIN_CC1101_GDO2 ||
        pin == (int)DB_PIN_CC1101_CS   || pin == (int)DB_PIN_CC1101_SCK  ||
        pin == (int)DB_PIN_CC1101_MOSI || pin == (int)DB_PIN_CC1101_MISO)
        return "wired to the CC1101";

    if (pin == 0)
        return "a strapping pin (holding it low forces download mode at reset)";
    if (pin == 19 || pin == 20)
        return "the USB-Serial/JTAG pair";
    if (pin > DB_GPIO_USER_MAX)
        return "not available on both target boards (flash/PSRAM/JTAG)";

    return NULL;
}

/* Caller holds the lock. */
static void gpio_slot_release(gpio_slot_t *s)
{
    if (s->pin < 0)
        return;
    gpio_num_t pin = (gpio_num_t)s->pin;
    gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
    gpio_isr_handler_remove(pin);
    gpio_reset_pin(pin);
    ESP_LOGI(TAG, "wired input released: GPIO %d", (int)pin);
    memset(s, 0, sizeof(*s));
    s->pin = -1;
}

/* Caller holds the lock. */
static esp_err_t gpio_slot_configure(gpio_slot_t *s, int slot, const db_node_t *n)
{
    gpio_num_t pin = (gpio_num_t)n->gpio_pin;

    if (!s_isr_service_installed) {
        /* Installed lazily, so a box with no wired buttons never allocates the
         * shared ISR service at all. INVALID_STATE means someone else already
         * installed it, which is a success for our purposes. */
        esp_err_t err = gpio_install_isr_service(0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service: %s", esp_err_to_name(err));
            return err;
        }
        s_isr_service_installed = true;
    }

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = n->gpio_active_low ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = n->gpio_active_low ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
        /* Only the pressing edge interrupts. The release is uninteresting, and
         * ignoring it halves the bounce traffic the task has to filter. */
        .intr_type    = n->gpio_active_low ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(GPIO %d): %s", (int)pin, esp_err_to_name(err));
        return err;
    }

    err = gpio_isr_handler_add(pin, gpio_edge_isr, (void *)(uintptr_t)slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_isr_handler_add(GPIO %d): %s",
                 (int)pin, esp_err_to_name(err));
        gpio_reset_pin(pin);
        return err;
    }

    s->pin         = (int8_t)pin;
    s->node_id     = n->id;
    s->active_low  = n->gpio_active_low;
    s->debounce_ms = n->gpio_debounce_ms ? n->gpio_debounce_ms : 50;
    s->last_us     = 0;
    s->bounces     = 0;

    ESP_LOGI(TAG, "wired input: GPIO %d -> node %u '%s' (%s, %ums debounce)",
             (int)pin, (unsigned)n->id, n->name,
             n->gpio_active_low ? "active-low, pull-up" : "active-high, pull-down",
             s->debounce_ms);
    return ESP_OK;
}

esp_err_t db_graph_apply_gpio_inputs(void)
{
    lock();

    /* Pass 1: work out what the graph wants. Nothing is touched yet, so a graph
     * with no source.gpio nodes leaves every pin exactly as it found it. */
    struct { int pin; int node_idx; } want[DB_GPIO_SLOTS];
    int want_count = 0;
    esp_err_t first_err = ESP_OK;

    for (int i = 0; i < s_node_count; i++) {
        const db_node_t *n = &s_nodes[i];
        if (n->type != DB_NODE_SOURCE_GPIO || !n->enabled)
            continue;
        if (n->gpio_pin < 0)
            continue;   /* configured but no pin chosen yet: not an error */

        const char *why = pin_rejection(n->gpio_pin);
        if (why) {
            ESP_LOGE(TAG, "node %u '%s': GPIO %d refused — %s",
                     (unsigned)n->id, n->name, (int)n->gpio_pin, why);
            if (first_err == ESP_OK)
                first_err = ESP_ERR_INVALID_ARG;
            continue;
        }

        bool dup = false;
        for (int j = 0; j < want_count; j++)
            if (want[j].pin == n->gpio_pin) dup = true;
        if (dup) {
            ESP_LOGE(TAG, "node %u '%s': GPIO %d is already claimed by another "
                          "node — one pin, one wired button",
                     (unsigned)n->id, n->name, (int)n->gpio_pin);
            if (first_err == ESP_OK)
                first_err = ESP_ERR_INVALID_STATE;
            continue;
        }

        if (want_count >= DB_GPIO_SLOTS) {
            ESP_LOGE(TAG, "node %u '%s': no free wired-input slot (max %d)",
                     (unsigned)n->id, n->name, DB_GPIO_SLOTS);
            if (first_err == ESP_OK)
                first_err = ESP_ERR_NO_MEM;
            continue;
        }
        want[want_count].pin      = n->gpio_pin;
        want[want_count].node_idx = i;
        want_count++;
    }

    /* Pass 2: release what is no longer wanted, or whose configuration changed.
     * This is what makes the function idempotent — calling it twice in a row
     * does nothing the second time, and deleting a GPIO node genuinely gives the
     * pin back. */
    for (int s = 0; s < DB_GPIO_SLOTS; s++) {
        if (s_gpio[s].pin < 0)
            continue;
        bool keep = false;
        for (int j = 0; j < want_count; j++) {
            if (want[j].pin != s_gpio[s].pin)
                continue;
            const db_node_t *n = &s_nodes[want[j].node_idx];
            keep = (n->id == s_gpio[s].node_id &&
                    n->gpio_active_low == s_gpio[s].active_low);
            break;
        }
        if (!keep)
            gpio_slot_release(&s_gpio[s]);
    }

    /* Pass 3: configure what is wanted and not already live. */
    int live = 0;
    for (int j = 0; j < want_count; j++) {
        int slot = -1;
        for (int s = 0; s < DB_GPIO_SLOTS; s++) {
            if (s_gpio[s].pin == want[j].pin) { slot = s; break; }
        }
        const db_node_t *n = &s_nodes[want[j].node_idx];

        if (slot >= 0) {
            /* Already interrupt-armed on this pin for this node: only the cheap
             * fields can have changed. */
            s_gpio[slot].debounce_ms = n->gpio_debounce_ms ? n->gpio_debounce_ms : 50;
            live++;
            continue;
        }
        for (int s = 0; s < DB_GPIO_SLOTS && slot < 0; s++)
            if (s_gpio[s].pin < 0) slot = s;
        if (slot < 0) {
            if (first_err == ESP_OK) first_err = ESP_ERR_NO_MEM;
            continue;
        }

        esp_err_t err = gpio_slot_configure(&s_gpio[slot], slot, n);
        if (err != ESP_OK) {
            if (first_err == ESP_OK) first_err = err;
            continue;
        }
        live++;
    }

    unlock();

    ESP_LOGI(TAG, "wired inputs reconciled: %d active", live);
    return first_err;
}

/* Debounce, in the task where it belongs. A mechanical button bounces for
 * milliseconds; the first edge is the real one, so we act on it immediately and
 * then ignore everything until the contacts have settled (a lockout, not a
 * settle-then-sample delay — the chime should ring on the press, not 50 ms
 * after it). The pin is read back as a last sanity check: an EMI spike on a long
 * doorbell run can produce an edge with no press behind it. */
static void handle_gpio_edge(uint16_t slot_idx)
{
    if (slot_idx >= DB_GPIO_SLOTS)
        return;

    lock();
    gpio_slot_t *s = &s_gpio[slot_idx];
    if (s->pin < 0) {
        unlock();
        return;   /* the pin was released while this edge was in flight */
    }

    int64_t now = esp_timer_get_time();
    if (s->last_us != 0 &&
        (now - s->last_us) < (int64_t)s->debounce_ms * 1000) {
        s->bounces++;
        unlock();
        return;
    }

    int level = gpio_get_level((gpio_num_t)s->pin);
    bool pressed = s->active_low ? (level == 0) : (level != 0);
    if (!pressed) {
        ESP_LOGD(TAG, "GPIO %d: edge with no press behind it — ignored", s->pin);
        unlock();
        return;
    }

    s->last_us = now;
    uint16_t node_id = s->node_id;
    int pin = s->pin;
    uint32_t bounces = s->bounces;
    s->bounces = 0;

    ESP_LOGI(TAG, "wired press: GPIO %d -> node %u%s", pin, (unsigned)node_id,
             bounces ? " (after bounce)" : "");
    fire_wired(node_id);
    unlock();
}

/* ---- the graph task ------------------------------------------------------ */

/*
 * How long to wait on the trigger queue.
 *
 * portMAX_DELAY whenever nothing is owed — an idle box must cost nothing, which
 * has been true of this engine from the start and stays true. The wait only
 * becomes finite when something is actually due: the next emission of a running
 * repeat sequence, or a switch position waiting to be written back. The task
 * wakes for those and for nothing else. No polling interval, no timer task, no
 * second queue.
 *
 * The floor of one tick is what keeps it that way. pdMS_TO_TICKS() truncates, so
 * a remainder shorter than a tick would otherwise become a zero-timeout receive
 * — a spin, at full task priority, until the millisecond arrived. Sleeping one
 * tick too long is invisible in a doorbell; busy-waiting is not.
 */
static TickType_t graph_wait_ticks(void)
{
    lock();
    int64_t due = repeat_next_due_us();
    /* Whichever is sooner. A pending switch write must not sit unwritten until
     * the next press happens to wake the task. */
    int64_t sw = switch_save_due_us();
    if (sw != 0 && (due == 0 || sw < due))
        due = sw;
    unlock();

    if (due == 0)
        return portMAX_DELAY;

    int64_t rem_ms = (due - esp_timer_get_time()) / 1000;
    if (rem_ms <= 0)
        return 0;                      /* already due: drain the queue, then ring */

    TickType_t t = pdMS_TO_TICKS(rem_ms);
    return t ? t : 1;
}

static void graph_task(void *arg)
{
    (void)arg;
    static queued_t q;   /* ~70 bytes, kept off the task stack */

    for (;;) {
        bool got = (xQueueReceive(s_queue, &q, graph_wait_ticks()) == pdTRUE);

        /* Waking on the timeout with nothing queued is the normal case for a
         * repeat: there is no trigger to handle, only emissions to make. */
        if (!got) {
            lock();
            int64_t now = esp_timer_get_time();
            repeat_service(now);
            switch_save_service(now);
            unlock();
            continue;
        }

        /* A trigger from a source starts its own chain: whatever it arms is one
         * repeat node away from the press, not however many the emission that
         * happened to precede it had crossed. */
        s_repeat_hops = 0;

        /* Set by the two paths that can move a switch without an API caller
         * behind them; the broker is told once, after the lock is dropped. */
        bool switch_moved = false;

        switch (q.kind) {
        case TRIG_RF:
            lock();
            /* Control before data — see switch_control_on_signal(). */
            switch_moved = (switch_control_on_signal(q.trig.signal_id) > 0);
            for (int i = 0; i < s_node_count; i++) {
                const db_node_t *n = &s_nodes[i];
                if (!n->enabled)
                    continue;

                bool fires = false;
                if (n->type == DB_NODE_SOURCE_ANY_RF) {
                    /* The wildcard: every burst, recognized or not. Opt-in — it
                     * does nothing unless the user placed one. */
                    fires = true;
                } else if (n->type == DB_NODE_SIGNAL_RX &&
                           q.trig.signal_id != 0 &&
                           n->signal_id == q.trig.signal_id) {
                    /* RECEIVERS ONLY. A signal.tx bound to the same code is not
                     * started by hearing it — it has no output to start from —
                     * so a heard code cannot re-send itself, structurally,
                     * rather than because a flag was checked. */
                    fires = true;
                }
                if (!fires)
                    continue;

                ESP_LOGI(TAG, "RF '%s' fires node %u '%s' (%s)",
                         q.trig.label, (unsigned)n->id, n->name,
                         type_name(n->type));
                /* A separate traversal per source node, so a burst driving both
                 * a signal chain and the wildcard chain reaches everything on
                 * both — that is intended, not double-firing. */
                traverse(n->id, &q.trig);
            }
            unlock();
            break;

        case TRIG_NODE: {
            lock();
            int idx = node_index(q.arg);
            if (idx >= 0) {
                /* Firing a SIGNAL_RX means "pretend this code was just heard",
                 * so it has to mean the whole of it — including the switches
                 * that react to that code. Without this the ▶ on a receiver
                 * would quietly simulate less than a real press does, and the
                 * feature would be untestable without a transmitter. No other
                 * type moves a switch: firing a tx node puts a code on the air
                 * and our own echo is suppressed, and firing anything else has
                 * no code behind it at all. */
                if (s_nodes[idx].type == DB_NODE_SIGNAL_RX)
                    switch_moved = (switch_control_on_signal(s_nodes[idx].signal_id) > 0);
                db_trigger_t trig;
                trigger_from_node(&trig, &s_nodes[idx]);
                traverse(q.arg, &trig);
            }
            unlock();
            break;
        }

        case TRIG_WIRED:
            lock();
            fire_wired(q.arg);
            unlock();
            break;

        case TRIG_GPIO:
            handle_gpio_edge(q.arg);
            break;

        case TRIG_WAKE:
            /* Deliberately nothing. Its whole job was to end the blocking
             * receive; the service calls below and the next graph_wait_ticks()
             * do the rest. */
            break;

        default:
            break;
        }

        /* Outside the lock, because the handler hands work to another task and
         * must never be run holding this one's mutex. */
        if (switch_moved && s_switch_notify)
            s_switch_notify();

        /* Handling that trigger may have taken a while (a transmit sink keys the
         * radio for a couple of hundred milliseconds), so an emission can have
         * fallen due meanwhile. Serviced here rather than only on the timeout
         * path, so a busy box still rings on time. */
        lock();
        int64_t now = esp_timer_get_time();
        repeat_service(now);
        switch_save_service(now);
        unlock();
    }
}

/* ---- init ---------------------------------------------------------------- */

esp_err_t db_graph_init(void)
{
    if (!s_lock)
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    if (!s_lock)
        return ESP_ERR_NO_MEM;
    if (s_ready)
        return ESP_OK;

    for (int i = 0; i < DB_GPIO_SLOTS; i++)
        s_gpio[i].pin = -1;

    lock();
    uint32_t nodes_ver = 0, links_ver = 0;
    s_node_count = load_blob(DB_GRAPH_NODES, s_nodes, sizeof(db_node_t),
                             DB_NODE_MAX, &nodes_ver, &NODES_LEGACY);
    /* The link record has never changed shape, so it has no legacy layout. */
    s_link_count = load_blob(DB_GRAPH_LINKS, s_links, sizeof(db_link_t),
                             DB_LINK_MAX, &links_ver, NULL);

    /* Bring an older layout forward BEFORE anything else looks at the types —
     * in particular before the unknown-type sanitiser below, which would
     * otherwise see a retired slot and disable the user's node. */
    if (nodes_ver != 0 && nodes_ver < DB_GRAPH_VERSION) {
        int changed = migrate_nodes(nodes_ver);
        ESP_LOGI(TAG, "graph nodes migrated v%u -> v%u (%d node(s) retyped)",
                 (unsigned)nodes_ver, (unsigned)DB_GRAPH_VERSION, changed);
    }

    /* Sanitise what came off flash: bounded strings and no link that points at a
     * node which no longer exists (a graph edited by an older firmware, or a
     * blob written before a failed delete). */
    for (int i = 0; i < s_node_count; i++) {
        s_nodes[i].name[DB_NODE_NAME_MAX - 1]   = '\0';
        s_nodes[i].topic[DB_NODE_TOPIC_MAX - 1] = '\0';
        /* The retired slot counts as unknown: after migrate_nodes() nothing may
         * still be one, and a node that somehow is must not be left holding a
         * type nothing in this file knows how to act on. */
        if (s_nodes[i].type >= DB_NODE__COUNT ||
            s_nodes[i].type == DB_NODE__RETIRED_TRANSMIT) {
            ESP_LOGW(TAG, "node %u has unknown type %u — disabling it",
                     (unsigned)s_nodes[i].id, s_nodes[i].type);
            s_nodes[i].type    = DB_NODE_SOURCE_VIRTUAL;
            s_nodes[i].enabled = false;
        }
    }
    int kept = 0;
    for (int i = 0; i < s_link_count; i++) {
        if (node_index(s_links[i].from) < 0 || node_index(s_links[i].to) < 0) {
            ESP_LOGW(TAG, "dropping dangling link %u -> %u",
                     (unsigned)s_links[i].from, (unsigned)s_links[i].to);
            continue;
        }
        s_links[kept++] = s_links[i];
    }
    s_link_count = kept;

    memset(s_pass_us, 0, sizeof(s_pass_us));
    memset(s_fired_us, 0, sizeof(s_fired_us));
    /* Nothing may survive a (re)load: an entry names a node id, and the graph
     * that has just come off flash is not necessarily the one those ids belonged
     * to. Zero at boot too, so this holds however init comes to be called. */
    memset(s_repeat, 0, sizeof(s_repeat));
    s_repeat_hops = 0;
    /* The graph that has just come off flash IS what flash holds, by definition,
     * so nothing is owed. Switch nodes come up in the position that was last
     * written — which the MQTT bridge then publishes retained on connect, so no
     * subscriber is left believing in a position the box is not in. */
    s_switch_dirty     = false;
    s_switch_change_us = 0;
    s_switch_saved_us  = 0;
    /* Monitor history goes the same way, and for the same reason: a ring names
     * a node id, and the graph that has just come off flash is not necessarily
     * the one that id belonged to. It is RAM-only debug telemetry anyway — a
     * reload is exactly the moment it stops meaning anything. */
    memset(s_monitor, 0, sizeof(s_monitor));

    /* Write the graph back in the current layout so the next boot reads it
     * directly and the migration is a one-off. Only when a blob was actually
     * READ and was older: an absent blob must stay absent (a box with no graph
     * yet should not acquire an empty one), and a failed save is logged by
     * save_blob() and simply left for the next mutation to retry — the in-RAM
     * graph is already correct either way. */
    if (nodes_ver != 0 && nodes_ver < DB_GRAPH_VERSION)
        save_nodes();
    if (links_ver != 0 && links_ver < DB_GRAPH_VERSION)
        save_links();
    unlock();

    if (!s_queue)
        s_queue = xQueueCreate(GRAPH_QUEUE_LEN, sizeof(queued_t));
    if (!s_queue)
        return ESP_ERR_NO_MEM;

    if (xTaskCreate(graph_task, "db_graph", GRAPH_TASK_STACK, NULL,
                    GRAPH_TASK_PRIO, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;

    s_ready = true;
    ESP_LOGI(TAG, "graph loaded: %d node(s), %d link(s)", s_node_count, s_link_count);
    return ESP_OK;
}

/* ---- read-only accessors ------------------------------------------------- */

int              db_graph_node_count(void) { return s_node_count; }
const db_node_t *db_graph_nodes(void)      { return s_nodes; }
int              db_graph_link_count(void) { return s_link_count; }
const db_link_t *db_graph_links(void)      { return s_links; }

const db_node_t *db_graph_node(uint16_t id)
{
    int i = node_index(id);
    return (i >= 0) ? &s_nodes[i] : NULL;
}

/* ---- defaults ------------------------------------------------------------ */

void db_graph_node_defaults(db_node_t *node, db_node_type_t type)
{
    if (!node)
        return;

    memset(node, 0, sizeof(*node));
    node->id       = 0;                  /* assigned by add_node */
    node->type     = (uint8_t)type;
    node->enabled  = true;
    node->gpio_pin = -1;                 /* wired input: opt-in, never assumed */
    node->gpio_active_low  = true;       /* button to GND + internal pull-up */
    node->gpio_debounce_ms = 50;         /* mechanical contacts settle in ~10-30ms */

    /* EVERY type, not only the three the bridge looks at. A user turning this
     * off on a node it does not apply to should see it stay off rather than
     * silently reset, and a future type that does reach MQTT must arrive
     * exposed by default like everything else — the flag is opt-OUT. */
    node->mqtt_enabled = true;

    /* Transmit policy mirrors db_config's defaults: real receivers integrate
     * several copies of a frame before they act, so one replay is routinely
     * ignored. */
    node->repeats = 6;
    node->gap_us  = 8000;

    node->group_mode = DB_GROUP_ANY;

    switch (type) {
    case DB_NODE_LOGIC_GROUP:
        /* A second is about as long as "at the same time" means to a person
         * pressing two buttons. */
        node->window_ms = 1000;
        break;
    case DB_NODE_LOGIC_THROTTLE:
        /* Long enough to swallow a leaned-on doorbell button, short enough that
         * a genuine second visitor still gets a chime. */
        node->window_ms = 10000;
        break;
    case DB_NODE_LOGIC_REPEAT:
        /* Three rings five seconds apart: the "I did not hear it the first
         * time" default. `repeats` here is a count of emissions, NOT the number
         * of copies of a frame a transmit sink sends, so it overrides the
         * transmit default set above. */
        node->window_ms = DB_REPEAT_DEF_MS;
        node->repeats   = (uint8_t)DB_REPEAT_DEF_TIMES;
        break;
    case DB_NODE_SINK_MONITOR:
        /* Not a window in the logic sense: how long the indicator stays lit
         * after a hit. Three seconds is long enough to catch out of the corner
         * of an eye and short enough that two presses read as two. */
        node->window_ms = DB_MONITOR_HOLD_DEF_S * 1000u;
        break;
    default:
        /* Sources (including the parameterless SOURCE_ANY_RF) and sinks have no
         * window of their own. */
        node->window_ms = 0;
        break;
    }

    snprintf(node->name, sizeof(node->name), "%s", type_name((uint8_t)type));
}

/* ---- mutation ------------------------------------------------------------ */

esp_err_t db_graph_add_node(const db_node_t *node, uint16_t *id_out)
{
    if (!node || node->type >= DB_NODE__COUNT)
        return ESP_ERR_INVALID_ARG;

    lock();
    if (s_node_count >= DB_NODE_MAX) {
        unlock();
        ESP_LOGE(TAG, "cannot add a node: the graph is full (%d)", DB_NODE_MAX);
        return ESP_ERR_NO_MEM;
    }
    uint16_t id = next_free_node_id();
    if (id == 0) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    db_node_t *n = &s_nodes[s_node_count];
    *n = *node;
    n->id = id;
    n->name[DB_NODE_NAME_MAX - 1]   = '\0';
    n->topic[DB_NODE_TOPIC_MAX - 1] = '\0';
    s_pass_us[s_node_count] = 0;
    /* Node ids are reused: next_free_node_id() hands back the lowest one free,
     * so a brand-new node can inherit the id of one deleted moments ago. Delete
     * already clears its own entries; this makes the invariant hold even if some
     * future path forgets to. */
    repeat_cancel_node(id);
    monitor_clear_node(id);
    s_node_count++;

    esp_err_t err = save_nodes();
    if (err != ESP_OK)
        s_node_count--;   /* keep RAM and flash in step */
    char name[DB_NODE_NAME_MAX];
    snprintf(name, sizeof(name), "%s", n->name);
    unlock();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "node %u added: %s '%s'", (unsigned)id,
                 type_name(node->type), name);
        if (id_out)
            *id_out = id;
    }
    return err;
}

esp_err_t db_graph_update_node(const db_node_t *node)
{
    if (!node || node->type >= DB_NODE__COUNT)
        return ESP_ERR_INVALID_ARG;

    lock();
    int i = node_index(node->id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    db_node_t prev = s_nodes[i];
    s_nodes[i] = *node;
    s_nodes[i].id = prev.id;                       /* the id is not editable */
    s_nodes[i].name[DB_NODE_NAME_MAX - 1]   = '\0';
    s_nodes[i].topic[DB_NODE_TOPIC_MAX - 1] = '\0';
    /* A retyped or rewindowed logic node starts from a clean slate rather than
     * inheriting a timestamp that means something different now. */
    if (prev.type != node->type || prev.window_ms != node->window_ms)
        s_pass_us[i] = 0;

    /* The same reasoning, but with teeth: a repeat sequence in flight belongs to
     * the node as it was CONFIGURED when it started. Retype it, retime it,
     * change how many rings it owes, or switch it off, and those rings are no
     * longer the ones the user is asking for — a disabled node in particular
     * must go quiet at once, not finish its run. Editing the name or the canvas
     * position is deliberately not on this list; nobody expects dragging a node
     * to cancel a chime. */
    if (prev.type != node->type || prev.window_ms != node->window_ms ||
        prev.repeats != node->repeats || (prev.enabled && !node->enabled)) {
        int cancelled = repeat_cancel_node(prev.id);
        if (cancelled)
            ESP_LOGI(TAG, "node %u edited: %d pending repeat run(s) cancelled",
                     (unsigned)prev.id, cancelled);
    }

    /* A monitor's history is a record of what THIS node saw. Retype it and the
     * marks describe something that no longer exists; switch it off and it must
     * go dark at once rather than showing a timeline it is no longer adding to.
     * Changing the hold is deliberately not on this list — that only says how
     * long the lamp stays lit, and throwing away ten minutes of observations
     * because someone nudged it from 3 s to 5 s would be gratuitous. */
    if (prev.type != node->type || (prev.enabled && !node->enabled))
        monitor_clear_node(prev.id);

    esp_err_t err = save_nodes();
    if (err != ESP_OK)
        s_nodes[i] = prev;
    char name[DB_NODE_NAME_MAX];
    snprintf(name, sizeof(name), "%s", s_nodes[i].name);
    unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "node %u updated: %s '%s'%s", (unsigned)node->id,
                 type_name(node->type), name,
                 node->enabled ? "" : " (disabled)");
    return err;
}

esp_err_t db_graph_delete_node(uint16_t id)
{
    lock();
    int i = node_index(id);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    /* Before the shift, and unconditionally: an emission still owed by this node
     * would otherwise come due against a node that no longer exists — at best a
     * silent no-op, at worst a ring from a chain the user has just deleted. The
     * table is keyed by node id, so the shift below cannot touch it either way. */
    int stopped = repeat_cancel_node(id);
    /* Likewise the monitor ring: ids are handed back out to new nodes, so a
     * ring left behind would surface as somebody else's history. */
    monitor_clear_node(id);

    for (int k = i; k < s_node_count - 1; k++) {
        s_nodes[k]   = s_nodes[k + 1];
        s_pass_us[k] = s_pass_us[k + 1];
    }
    s_node_count--;
    memset(&s_nodes[s_node_count], 0, sizeof(s_nodes[s_node_count]));
    s_pass_us[s_node_count] = 0;

    /* Its links go with it — a link to a node that no longer exists is exactly
     * the kind of debris that later reads as a mysterious dead branch. */
    int kept = 0, dropped = 0;
    for (int k = 0; k < s_link_count; k++) {
        if (s_links[k].from == id || s_links[k].to == id) { dropped++; continue; }
        s_links[kept]    = s_links[k];
        s_fired_us[kept] = s_fired_us[k];
        kept++;
    }
    s_link_count = kept;

    esp_err_t err = save_nodes();
    if (err == ESP_OK && dropped)
        err = save_links();

    /* A wired input pointing at the deleted node keeps interrupting until the
     * caller reconciles; the edge handler simply finds no node and does nothing.
     * db_graph_apply_gpio_inputs() releases the pin for good. */
    unlock();

    ESP_LOGI(TAG, "node %u deleted (%d link(s) with it%s)", (unsigned)id, dropped,
             stopped ? ", repeat run stopped" : "");
    return err;
}

esp_err_t db_graph_add_link(uint16_t from, uint16_t to)
{
    if (from == 0 || to == 0)
        return ESP_ERR_INVALID_ARG;
    if (from == to) {
        ESP_LOGE(TAG, "refusing self-link on node %u — that is a cycle of one",
                 (unsigned)from);
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (node_index(from) < 0 || node_index(to) < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    for (int i = 0; i < s_link_count; i++) {
        if (s_links[i].from == from && s_links[i].to == to) {
            unlock();
            return ESP_OK;   /* already wired: idempotent, not an error */
        }
    }
    if (s_link_count >= DB_LINK_MAX) {
        unlock();
        ESP_LOGE(TAG, "cannot add a link: the graph is full (%d)", DB_LINK_MAX);
        return ESP_ERR_NO_MEM;
    }

    s_links[s_link_count].from = from;
    s_links[s_link_count].to   = to;
    s_fired_us[s_link_count]   = 0;
    s_link_count++;

    esp_err_t err = save_links();
    if (err != ESP_OK)
        s_link_count--;
    unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "link %u -> %u added", (unsigned)from, (unsigned)to);
    return err;
}

esp_err_t db_graph_delete_link(uint16_t from, uint16_t to)
{
    lock();
    int found = -1;
    for (int i = 0; i < s_link_count; i++)
        if (s_links[i].from == from && s_links[i].to == to) { found = i; break; }
    if (found < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = found; i < s_link_count - 1; i++) {
        s_links[i]    = s_links[i + 1];
        s_fired_us[i] = s_fired_us[i + 1];
    }
    s_link_count--;
    s_fired_us[s_link_count] = 0;

    esp_err_t err = save_links();
    unlock();

    if (err == ESP_OK)
        ESP_LOGI(TAG, "link %u -> %u deleted", (unsigned)from, (unsigned)to);
    return err;
}

/* ---- sink registration --------------------------------------------------- */

void db_graph_set_transmit_handler(db_sink_fn fn, void *ctx)
{
    s_transmit.fn  = fn;
    s_transmit.ctx = ctx;
    ESP_LOGI(TAG, "transmit sink handler %s", fn ? "registered" : "cleared");
}

void db_graph_set_mqtt_handler(db_sink_fn fn, void *ctx)
{
    s_mqtt.fn  = fn;
    s_mqtt.ctx = ctx;
    ESP_LOGI(TAG, "mqtt sink handler %s", fn ? "registered" : "cleared");
}

/* ---- entry points -------------------------------------------------------- */

void db_graph_on_rf(const db_trigger_t *trig)
{
    if (!s_queue || !trig)
        return;

    /* Called from the RF capture task, which is holding the radio mutex. It must
     * not block and it must not run a sink here — hence the queue. */
    queued_t q;
    memset(&q, 0, sizeof(q));
    q.kind = TRIG_RF;
    q.arg  = trig->signal_id;
    q.trig = *trig;
    q.trig.protocol[sizeof(q.trig.protocol) - 1] = '\0';
    q.trig.label[sizeof(q.trig.label) - 1]       = '\0';
    if (q.trig.label[0] == '\0')
        snprintf(q.trig.label, sizeof(q.trig.label), "%s",
                 trig->signal_id ? "signal" : "unknown");

    if (xQueueSend(s_queue, &q, 0) != pdTRUE)
        ESP_LOGW(TAG, "trigger queue full — dropped an RF burst (signal %u)",
                 (unsigned)trig->signal_id);
}

void db_graph_on_wired(uint16_t node_id)
{
    if (!s_queue || node_id == 0)
        return;

    queued_t q;
    memset(&q, 0, sizeof(q));
    q.kind = TRIG_WIRED;
    q.arg  = node_id;
    if (xQueueSend(s_queue, &q, pdMS_TO_TICKS(50)) != pdTRUE)
        ESP_LOGW(TAG, "trigger queue full — dropped wired press on node %u",
                 (unsigned)node_id);
}

esp_err_t db_graph_fire_node(uint16_t node_id)
{
    if (!s_queue)
        return ESP_ERR_INVALID_STATE;

    lock();
    int i = node_index(node_id);
    bool enabled = (i >= 0) && s_nodes[i].enabled;
    unlock();

    if (i < 0)
        return ESP_ERR_NOT_FOUND;
    if (!enabled)
        return ESP_ERR_INVALID_STATE;   /* the API maps this to 409 */

    queued_t q;
    memset(&q, 0, sizeof(q));
    q.kind = TRIG_NODE;
    q.arg  = node_id;
    if (xQueueSend(s_queue, &q, pdMS_TO_TICKS(50)) != pdTRUE) {
        ESP_LOGW(TAG, "trigger queue full — node %u not fired", (unsigned)node_id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

/*
 * MATCHED BY RESOLVED SUFFIX, exactly as db_graph_switch_set_topic() is — see
 * node_graph.h for why every node on the topic fires rather than the first.
 *
 * The ids are collected under the lock and fired outside it: db_graph_fire_node()
 * takes the same non-recursive mutex, and holding it across a queue send would
 * be the graph task deadlocking on itself the moment the queue filled.
 */
int db_graph_fire_topic(const char *topic)
{
    if (!topic || !topic[0])
        return 0;

    uint16_t ids[DB_NODE_MAX];
    int n = 0;

    lock();
    for (int i = 0; i < s_node_count && n < DB_NODE_MAX; i++) {
        if (s_nodes[i].type != DB_NODE_SOURCE_VIRTUAL)
            continue;
        if (!s_nodes[i].enabled || !s_nodes[i].mqtt_enabled)
            continue;
        char sfx[DB_NODE_TOPIC_MAX];
        db_graph_node_suffix(&s_nodes[i], sfx, sizeof(sfx));
        if (strcmp(sfx, topic) != 0)
            continue;
        ids[n++] = s_nodes[i].id;
    }
    unlock();

    int fired = 0;
    for (int i = 0; i < n; i++)
        if (db_graph_fire_node(ids[i]) == ESP_OK)
            fired++;

    if (n)
        ESP_LOGI(TAG, "trigger topic '%s' -> %d node(s), %d fired", topic, n, fired);
    return fired;
}
