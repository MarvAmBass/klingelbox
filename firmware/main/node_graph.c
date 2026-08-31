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
 * That guard carries the weight now that DB_NODE_SIGNAL has both ports and a
 * cycle is something a user can draw without meaning to. A -> A is refused
 * outright by db_graph_add_link(); A -> B -> A is walked once and stopped by the
 * mark. What the mark cannot see is a cycle closed OVER THE AIR — signal node A
 * transmits, a receiver echoes it, signal node A hears it and starts a fresh
 * traversal. That one is rf_service's TX echo window's job, not this file's.
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
 *
 * A blob whose version is OLDER than this is migrated on load and written back
 * in the current layout. A blob NEWER than this is refused, because guessing at
 * a layout from the future is how a downgrade eats a user's graph.
 */
#define DB_GRAPH_VERSION 2u

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

/* ---- persisted layout ---------------------------------------------------- */

typedef struct {
    uint32_t version;
    uint32_t item_size;
    uint32_t count;
} grf_hdr_t;

/* ---- trigger queue ------------------------------------------------------- */

typedef enum {
    TRIG_RF = 0,   /* an RF burst: the matching signal node + source.any_rf     */
    TRIG_NODE,     /* arg = node id (UI/REST/MQTT fire, or a test-fire)          */
    TRIG_WIRED,    /* arg = node id, already debounced                           */
    TRIG_GPIO,     /* arg = GPIO slot index, straight from the ISR               */
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
    case DB_NODE_SIGNAL:         return "signal";
    case DB_NODE_SOURCE_GPIO:    return "source.gpio";
    case DB_NODE_SOURCE_VIRTUAL: return "source.virtual";
    case DB_NODE_SOURCE_ANY_RF:  return "source.any_rf";
    case DB_NODE_LOGIC_GROUP:    return "logic.group";
    case DB_NODE_LOGIC_THROTTLE: return "logic.throttle";
    case DB_NODE_LOGIC_REPEAT:   return "logic.repeat";
    case DB_NODE_SINK_MQTT:      return "sink.mqtt";
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
 * same in the item count when the graph is simply empty. */
static int load_blob(const char *key, void *items, size_t item_size, int max,
                     uint32_t *ver_out)
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
    /* Older layouts are migrated by the caller; a NEWER one is refused. The item
     * size must match either way — every shipped version of db_node_t and
     * db_link_t has had the same layout, so a mismatch is corruption or a
     * struct change that would need its own frozen typedef here. */
    if (hdr.version == 0 || hdr.version > DB_GRAPH_VERSION ||
        hdr.item_size != item_size) {
        ESP_LOGW(TAG, "%s blob is layout v%u/%u, this build reads up to v%u/%u — ignored",
                 key, (unsigned)hdr.version, (unsigned)hdr.item_size,
                 (unsigned)DB_GRAPH_VERSION, (unsigned)item_size);
        return 0;
    }
    if (ver_out)
        *ver_out = hdr.version;

    int n = (hdr.count > (uint32_t)max) ? max : (int)hdr.count;
    if (len < sizeof(hdr) + (size_t)n * item_size) {
        ESP_LOGE(TAG, "%s blob is truncated — ignored", key);
        return 0;
    }
    memcpy(items, s_blob + sizeof(hdr), (size_t)n * item_size);
    return n;
}

/* Caller holds the lock. */
static esp_err_t save_nodes(void)
{
    return save_blob(DB_GRAPH_NODES, s_nodes, sizeof(db_node_t), s_node_count);
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
 * Slot 0 was source.button and is now DB_NODE_SIGNAL, which is exactly why
 * every stored button node needs no work at all — the consolidation was
 * designed around keeping that slot. Everything the migrated node needs is
 * already in the struct: signal_id names the same stored signal, and repeats /
 * gap_us still describe how its frame goes out. The node keeps its id, its
 * name, its position and every link, so a graph drawn under v1 comes back
 * looking the same, with one extra input port on the nodes that used to be
 * transmit sinks.
 *
 * ADDING v3: add a `case 2:` below, and let `case 1:` fall INTO it, so a v1
 * blob is carried forward through every step in turn rather than needing its
 * own v1->v3 shortcut. (`case 1:` breaks today only because it is the last one.)
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
            s_nodes[i].type = DB_NODE_SIGNAL;
            changed++;
            ESP_LOGI(TAG, "migrated node %u '%s': sink.transmit -> signal",
                     (unsigned)s_nodes[i].id, s_nodes[i].name);
        }
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
 * Perform whatever a node DOES on being reached. Only sinks act — and the input
 * side of a signal node, which is a sink in all but name; sources and logic
 * nodes exist to route. Caller holds the lock and is the graph task.
 *
 * `is_start` says the node is where this traversal BEGAN rather than somewhere
 * it arrived over a link. That distinction is what makes DB_NODE_SIGNAL's two
 * ports mean different things:
 *
 *   started here  -> the code was heard on air (or the node was test-fired).
 *                    The node's OUTPUT is what fired; the walk continues into
 *                    its children and nothing is transmitted.
 *   reached by a  -> something upstream is asking for this signal to go out.
 *   link             That is the INPUT, and it transmits.
 *
 * Acting on the start too would turn every signal node into an unrequested
 * repeater: hear the code, immediately say it again. Do not "simplify" this by
 * dropping the flag.
 */
static void node_act(const db_node_t *n, const db_trigger_t *trig, bool is_start)
{
    switch (n->type) {
    case DB_NODE_SIGNAL:
        if (is_start)
            break;   /* the output side: heard, not asked to send */
        if (!s_transmit.fn) {
            ESP_LOGW(TAG, "node %u '%s' wants to transmit but no handler is "
                          "registered", (unsigned)n->id, n->name);
            return;
        }
        ESP_LOGI(TAG, "signal transmit: node %u '%s' -> signal %u x%u (from '%s')",
                 (unsigned)n->id, n->name, (unsigned)n->signal_id, n->repeats,
                 trig->label);
        db_events_push(DB_EV_NODE_FIRED, n->signal_id, n->id, trig->rssi_dbm,
                       n->repeats, "%s", n->name[0] ? n->name : "signal");
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
 * The start node is ALSO the one thing node_act() treats differently, for a
 * related reason: a signal node that a burst just fired is acting as an input,
 * not being asked to send. See node_act().
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

        /* `id == start_id` is exactly "this is the start": a node is entered at
         * most once per traversal, so no later hop can wear the start's id. */
        node_act(n, trig, id == start_id);

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
 * has been true of this engine from the start and stays true. Only when a repeat
 * sequence is running does the wait become finite, and then it is exactly the
 * time to the next emission: the task wakes for that ring and for nothing else.
 * No polling interval, no timer task, no second queue.
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
            repeat_service(esp_timer_get_time());
            unlock();
            continue;
        }

        /* A trigger from a source starts its own chain: whatever it arms is one
         * repeat node away from the press, not however many the emission that
         * happened to precede it had crossed. */
        s_repeat_hops = 0;

        switch (q.kind) {
        case TRIG_RF:
            lock();
            for (int i = 0; i < s_node_count; i++) {
                const db_node_t *n = &s_nodes[i];
                if (!n->enabled)
                    continue;

                bool fires = false;
                if (n->type == DB_NODE_SOURCE_ANY_RF) {
                    /* The wildcard: every burst, recognized or not. Opt-in — it
                     * does nothing unless the user placed one. */
                    fires = true;
                } else if (n->type == DB_NODE_SIGNAL &&
                           q.trig.signal_id != 0 &&
                           n->signal_id == q.trig.signal_id) {
                    /* The signal node's OUTPUT side. traverse() starts here, so
                     * node_act() sees is_start and does not send it back out. */
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

        default:
            break;
        }

        /* Handling that trigger may have taken a while (a transmit sink keys the
         * radio for a couple of hundred milliseconds), so an emission can have
         * fallen due meanwhile. Serviced here rather than only on the timeout
         * path, so a busy box still rings on time. */
        lock();
        repeat_service(esp_timer_get_time());
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
                             DB_NODE_MAX, &nodes_ver);
    s_link_count = load_blob(DB_GRAPH_LINKS, s_links, sizeof(db_link_t),
                             DB_LINK_MAX, &links_ver);

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

    /* Transmit policy mirrors db_config's defaults: real receivers integrate
     * several copies of a frame before they act, so one replay is routinely
     * ignored (PLAN.md §13, M4). */
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
