/*
 * mqtt_bridge.c - see db_mqtt.h. MQTT + Home Assistant discovery for Klingelbox.
 *
 * TOPIC MAP (base topic from config, default "klingelbox")
 *
 *   <base>/status                      "online"/"offline", RETAINED, and the LWT
 *   <base>/button/<slug>/state         one recognized press, JSON, NOT retained
 *   <base>/button/<slug>/press         SUBSCRIBED: any message transmits <slug>
 *   <base>/trigger/<suffix>            SUBSCRIBED: fires the SOURCE_VIRTUAL node
 *                                      whose topic suffix this is
 *   <base>/unknown/state               an UNREGISTERED burst, JSON, NOT retained
 *   <base>/unknown                     last unregistered burst, JSON, RETAINED
 *   <base>/event                       every node firing + system events, JSON
 *   <base>/<suffix>                    a SINK_MQTT node's own topic, when set
 *   <base>/radio                       radio telemetry, JSON, RETAINED
 *
 * WHY PRESSES ARE NOT RETAINED AND TELEMETRY IS. Retention answers "what should a
 * subscriber that just connected be told?". For radio telemetry the honest answer
 * is the last reading; for a doorbell press it is *nothing*, because a press is a
 * moment, not a condition. A retained press payload would ring every chime in the
 * house every time Home Assistant restarts.
 *
 * WHY DEVICE TRIGGERS. HA's MQTT integration offers two plausible shapes for a
 * button: a binary_sensor that goes on and must be reset, or a device trigger.
 * The binary_sensor is a trap — the reset can be lost (a dropped connection, a
 * reboot mid-press) and the entity then sits "on" forever, and every automation
 * has to be written against a state edge instead of an event. A device trigger
 * (`device_automation`, automation_type "trigger") is HA's native momentary
 * event: it needs no reset, cannot get stuck, and shows up directly in the
 * automation editor as "<subtype> <type>" under this device. So each stored
 * signal is announced as a trigger for *receiving*, and additionally as a
 * `button` entity for *transmitting* — the two directions are genuinely
 * different things and deserve different entity types.
 *
 * THE PROXY CASE. A `source.any_rf -> sink.mqtt` chain forwards EVERY burst on
 * the band, including ones matching no stored signal (`trig->signal_id == 0`).
 * Those have no per-signal entity to fire, so they get a catch-all pair: one
 * device trigger on <base>/unknown/state, so "any unregistered remote was
 * pressed" is automatable, and one retained diagnostic sensor showing the last
 * unknown fingerprint, so a user can SEE the code of the remote they are about
 * to learn. Nothing about an unknown burst is assumed: signal lookups may return
 * NULL and `decoded` is legitimately null.
 *
 * ONE HA DEVICE. Every discovery payload carries the same `device` block, keyed
 * on identifiers derived from the MAC. That is the whole mechanism by which HA
 * groups entities: get it wrong and a box with six buttons scatters into a dozen
 * unrelated entities with no shared page and no shared availability. The MAC is
 * used rather than the hostname because the hostname is user-editable — renaming
 * the box must move the device, not fork it.
 *
 * SLUGS. Topics are addressed by a slug of the signal name, not by its numeric
 * id, because `klingelbox/button/front_door/press` is a topic a human can type into
 * mosquitto_pub and an automation someone can read six months later. Names are
 * not unique though, and slugging collapses more of them together ("Front door"
 * and "front-door!"), so the slug table resolves collisions deterministically —
 * see slug_table_build(). unique_ids, by contrast, are keyed on the numeric
 * signal id so a rename never orphans an HA entity.
 *
 * NOTHING BLOCKS THE ESP-MQTT EVENT TASK. The event handler only ever copies a
 * topic/payload into a queue message. Everything with a cost — loading a 1 KB
 * frame from NVS, taking the radio mutex, keying the carrier for a few hundred
 * milliseconds, building discovery JSON — happens on this module's own task. A
 * transmit run inside the event handler would stall esp-mqtt's keepalive and
 * drop the very connection the command arrived on. The same queue carries press
 * notifications and graph events, so the RF path never blocks on the network
 * either.
 */
#include "db_mqtt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"        /* esp-mqtt: esp_mqtt_client_* (NOT our header) */

#include "event_log.h"
#include "node_graph.h"
#include "rf_service.h"
#include "signal_store.h"

static const char *TAG = "db_mqtt";

/* Slugs are topic segments and HA object_id fragments, so they stay short and
 * ASCII. 40 covers DB_SIGNAL_NAME_MAX (32) plus a "_<id>" de-dupe suffix. */
#define DB_MQTT_SLUG_MAX   40
#define DB_MQTT_TOPIC_MAX  176   /* <base>/button/<slug>/state and friends      */
#define DB_MQTT_DISC_MAX   240   /* <prefix>/<component>/<node>/<object>/config */
#define DB_MQTT_ARG_MAX    DB_NODE_TOPIC_MAX  /* slug | node topic suffix       */
#define DB_MQTT_TEXT_MAX   64

#define DB_MQTT_QUEUE_LEN  10
#define DB_MQTT_TICK_MS    10000  /* periodic radio-telemetry refresh */

#define DB_MQTT_DEFAULT_BASE      "klingelbox"
#define DB_MQTT_DEFAULT_DISCOVERY "homeassistant"

/* ---- module state --------------------------------------------------------- */

static db_config_t             *s_cfg;
static esp_mqtt_client_handle_t s_client;
static volatile bool            s_connected;
static volatile bool            s_resub_all;   /* broker forgot our subscriptions */
static TaskHandle_t             s_task;
static QueueHandle_t            s_queue;

/* "<base>/status". Also the Last-Will topic, so it must outlive
 * esp_mqtt_client_init() — hence static, not a stack buffer. */
static char s_status_topic[DB_STR_TOPIC + 16];

/* Effective base / discovery prefix (config value, or the default when empty). */
static const char *s_base;
static const char *s_disc;

/* HA device identity. Derived once at start: the MAC gives a stable identifier
 * that survives a hostname change, the hostname gives the friendly name. */
static char s_dev_uid[32];        /* "klingelbox_a1b2c3d4e5f6" */
static char s_dev_mac[18];        /* "a1:b2:c3:d4:e5:f6" */
static char s_dev_slug[40];       /* slug of the hostname, for object_ids */
static char s_sw_version[32];

/* Last recognized press, for the radio telemetry payload. */
static uint16_t s_last_press_id;
static int      s_last_press_rssi;
static int64_t  s_last_press_us;

/* ---- slug table -----------------------------------------------------------
 * Rebuilt from the live signal store whenever it is needed. Only the bridge task
 * touches it, so it needs no locking. Everything that maps between a signal and
 * a topic goes through here, which is what guarantees the topic we announce is
 * the topic we publish on and the topic we route commands from. */
static uint16_t s_slug_id[DB_SIGNAL_MAX];
static char     s_slug[DB_SIGNAL_MAX][DB_MQTT_SLUG_MAX];
static int      s_slug_count;

/* Discovery snapshot: what we last announced, so a rename or a delete can clear
 * the retained config of the topic that went away. */
static uint16_t s_ann_id[DB_SIGNAL_MAX];
static char     s_ann_slug[DB_SIGNAL_MAX][DB_MQTT_SLUG_MAX];
static int      s_ann_count;

/* Live <base>/trigger/<suffix> subscriptions, one per enabled SOURCE_VIRTUAL
 * node with a topic. Routing is by suffix; node_id is what actually fires. */
typedef struct {
    uint16_t node_id;
    char     suffix[DB_NODE_TOPIC_MAX];
} db_mqtt_sub_t;
static db_mqtt_sub_t s_subs[DB_NODE_MAX];
static int           s_sub_count;

/* Announced virtual-source buttons, for the same clear-on-delete reason. */
static uint16_t s_ann_virt[DB_NODE_MAX];
static int      s_ann_virt_count;

/* The shared entities have been cleared for a discovery-off config. Latched so a
 * box that never had discovery on does not re-publish the same four empty
 * retained payloads on every single reconnect. */
static bool s_shared_retired;

/* ---- queue messages -------------------------------------------------------
 * One queue for every direction of work, so the bridge task can wait on a single
 * object and still have a periodic tick (the receive timeout). */
typedef enum {
    MSG_PUBLISH = 0,  /* push radio telemetry now                              */
    MSG_ANNOUNCE,     /* re-resolve slugs, re-publish discovery, re-sync subs   */
    MSG_PRESS,        /* a stored signal was recognized                         */
    MSG_TRANSMIT,     /* <base>/button/<slug>/press arrived                     */
    MSG_FIRE,         /* <base>/trigger/<suffix> arrived                        */
    MSG_EVENT,        /* a node fired / a system event                          */
    MSG_STOP,
} db_mqtt_msg_kind_t;

typedef struct {
    uint8_t      kind;                    /* db_mqtt_msg_kind_t */
    uint8_t      ev_kind;                 /* db_event_kind_t, for MSG_EVENT */
    uint16_t     node_id;
    db_trigger_t trig;                    /* what caused this, if anything */
    char         arg[DB_MQTT_ARG_MAX];    /* slug | trigger suffix | node topic */
    char         text[DB_MQTT_TEXT_MAX];  /* command payload | event text */
} db_mqtt_msg_t;

/* Vocabulary of event_log.h / docs/API.md, verbatim. The names on the wire and
 * the names in GET /api/events must never drift apart. */
static const char *const EVENT_KIND_NAMES[] = {
    "rf_unmatched", "button_press", "wired_press", "node_fired",
    "transmit", "learn", "system",
};

/* ---- small helpers -------------------------------------------------------- */

static bool post(const db_mqtt_msg_t *m)
{
    if (!s_queue) return false;
    /* Never block a caller: the RF path, the graph and the esp-mqtt event task
     * all post here. A full queue means we are already saturated; dropping the
     * newest message is strictly better than stalling the radio. */
    if (xQueueSend(s_queue, m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped message kind %u", m->kind);
        return false;
    }
    return true;
}

static void post_simple(db_mqtt_msg_kind_t kind)
{
    db_mqtt_msg_t m = { .kind = (uint8_t)kind };
    post(&m);
}

static void pub(const char *topic, const char *payload, int qos, int retain)
{
    if (s_client)
        esp_mqtt_client_publish(s_client, topic, payload ? payload : "", 0, qos, retain);
}

/* Serialize, publish, free — on every path, including the failed-print one. */
static void pub_json(const char *topic, cJSON *doc, int qos, int retain)
{
    if (!doc) return;
    char *s = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!s) {
        ESP_LOGW(TAG, "out of memory rendering %s", topic);
        return;
    }
    pub(topic, s, qos, retain);
    cJSON_free(s);
}

/* Lowercase alphanumerics survive; every other byte becomes '_', runs collapse,
 * and leading/trailing separators are trimmed. Multi-byte UTF-8 (an umlaut in a
 * signal name) degrades to a single '_' rather than mangling the topic. */
static void slugify(const char *name, char *out, size_t outsz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < outsz; p++) {
        if (isalnum(*p)) {
            out[o++] = (char)tolower(*p);
        } else if (o && out[o - 1] != '_') {
            out[o++] = '_';
        }
    }
    while (o && out[o - 1] == '_') o--;
    out[o] = '\0';
}

static void topic_of(char *out, size_t outsz, const char *suffix)
{
    snprintf(out, outsz, "%s/%s", s_base, suffix);
}

static void button_topic(char *out, size_t outsz, const char *slug, const char *leaf)
{
    snprintf(out, outsz, "%s/button/%s/%s", s_base, slug, leaf);
}

static void disc_topic(char *out, size_t outsz, const char *component, const char *object)
{
    snprintf(out, outsz, "%s/%s/%s/%s/config", s_disc, component, s_dev_uid, object);
}

/* Unix seconds, or 0 while the clock is unset (no SNTP yet, or no internet).
 * Publishing a 1970 timestamp would be worse than publishing none. */
static int64_t unix_now(void)
{
    time_t now = time(NULL);
    return (now > 1600000000) ? (int64_t)now : 0;
}

/* ---- slug resolution ------------------------------------------------------ */

static bool slug_taken(const char *cand, int upto)
{
    for (int i = 0; i < upto; i++)
        if (strcmp(s_slug[i], cand) == 0) return true;
    return false;
}

/*
 * Rebuild the id <-> slug table from the live store.
 *
 * Uniqueness ladder, applied in store order so the result is deterministic and
 * stable for a given set of signals: the bare slug of the name; then
 * "<slug>_<id>"; then "signal_<id>". Ids are unique, so the ladder always
 * terminates, and an EARLIER signal never loses its slug to a later one — which
 * is what keeps an existing automation working when a new button is learned.
 */
static void slug_table_build(void)
{
    const db_signal_meta_t *list = db_signals_list();
    int n = db_signals_count();
    if (n > DB_SIGNAL_MAX) n = DB_SIGNAL_MAX;
    s_slug_count = 0;
    if (!list) return;

    for (int i = 0; i < n; i++) {
        char *dst = s_slug[s_slug_count];
        slugify(list[i].name, dst, DB_MQTT_SLUG_MAX);
        if (!dst[0] || slug_taken(dst, s_slug_count)) {
            if (dst[0]) {
                char base[DB_MQTT_SLUG_MAX];
                strlcpy(base, dst, sizeof(base));
                /* Truncate the base so the "_<id>" suffix always fits. */
                if (strlen(base) > DB_MQTT_SLUG_MAX - 8) base[DB_MQTT_SLUG_MAX - 8] = '\0';
                /* The explicit precision repeats the truncation above in a form
                 * the compiler can verify; without it gcc cannot prove the
                 * "_<id>" suffix fits and -Werror=format-truncation trips. */
                snprintf(dst, DB_MQTT_SLUG_MAX, "%.*s_%u",
                         (int)(DB_MQTT_SLUG_MAX - 8), base, list[i].id);
            }
            if (!dst[0] || slug_taken(dst, s_slug_count))
                snprintf(dst, DB_MQTT_SLUG_MAX, "signal_%u", list[i].id);
        }
        s_slug_id[s_slug_count] = list[i].id;
        s_slug_count++;
    }
}

static const char *slug_for_id(uint16_t id)
{
    for (int i = 0; i < s_slug_count; i++)
        if (s_slug_id[i] == id) return s_slug[i];
    return NULL;
}

static uint16_t id_for_slug(const char *slug)
{
    for (int i = 0; i < s_slug_count; i++)
        if (strcmp(s_slug[i], slug) == 0) return s_slug_id[i];
    return 0;
}

/* ---- Home Assistant discovery --------------------------------------------- */

/* The shared device block. Identical in every payload — that identity is what
 * makes HA fold the entities into one device page with one availability state. */
static void add_device(cJSON *doc)
{
    cJSON *dev = cJSON_AddObjectToObject(doc, "device");
    if (!dev) return;

    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    if (ids) cJSON_AddItemToArray(ids, cJSON_CreateString(s_dev_uid));

    /* connections lets HA merge this with a device it already knows by MAC
     * (e.g. from the DHCP integration) instead of creating a duplicate. */
    cJSON *conns = cJSON_AddArrayToObject(dev, "connections");
    if (conns) {
        cJSON *c = cJSON_CreateArray();
        if (c) {
            cJSON_AddItemToArray(c, cJSON_CreateString("mac"));
            cJSON_AddItemToArray(c, cJSON_CreateString(s_dev_mac));
            cJSON_AddItemToArray(conns, c);
        }
    }

    cJSON_AddStringToObject(dev, "name",
                            s_cfg->hostname[0] ? s_cfg->hostname : "klingelbox");
    cJSON_AddStringToObject(dev, "manufacturer", "MarvAmBass");
    cJSON_AddStringToObject(dev, "model", "Klingelbox");
    cJSON_AddStringToObject(dev, "sw_version", s_sw_version);

    if (s_cfg->hostname[0]) {
        char url[DB_STR_HOSTNAME + 16];
        snprintf(url, sizeof(url), "http://%s.local/", s_cfg->hostname);
        cJSON_AddStringToObject(dev, "configuration_url", url);
    }
}

/* Availability off the retained <base>/status topic, which the LWT flips to
 * "offline" when the box drops off the network. Without this HA would keep
 * showing the last state of a device that is no longer there. */
static void add_availability(cJSON *doc)
{
    cJSON_AddStringToObject(doc, "availability_topic", s_status_topic);
    cJSON_AddStringToObject(doc, "payload_available", "online");
    cJSON_AddStringToObject(doc, "payload_not_available", "offline");
}

/*
 * A device trigger. Deliberately minimal: HA's device_trigger discovery schema
 * accepts only automation_type / topic / type / subtype / payload /
 * value_template / device and drops everything else, so there is no unique_id,
 * no name and no availability here — a trigger is not an entity. With no
 * `payload` or `value_template` set, ANY message on the topic fires it, which is
 * exactly the semantics we want: one topic per button, one message per press.
 */
static void announce_trigger(const char *state_topic, const char *subtype,
                             const char *object)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "automation_type", "trigger");
    cJSON_AddStringToObject(d, "topic", state_topic);
    /* HA renders the picker entry as "<subtype> <type>", e.g. "Front door
     * pressed". button_short_press is HA's standard momentary type. */
    cJSON_AddStringToObject(d, "type", "button_short_press");
    cJSON_AddStringToObject(d, "subtype", subtype);
    add_device(d);

    char dt[DB_MQTT_DISC_MAX];
    disc_topic(dt, sizeof(dt), "device_automation", object);
    pub_json(dt, d, 1, 1);
}

static void announce_signal_trigger(const db_signal_meta_t *m, const char *slug)
{
    char state_t[DB_MQTT_TOPIC_MAX];
    button_topic(state_t, sizeof(state_t), slug, "state");
    char object[DB_MQTT_SLUG_MAX + 8];
    snprintf(object, sizeof(object), "%s_press", slug);
    announce_trigger(state_t, m->name[0] ? m->name : slug, object);
}

/* The other direction: a `button` entity whose press transmits the signal. */
static void announce_tx_button(const db_signal_meta_t *m, const char *slug)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "name", m->name[0] ? m->name : slug);

    /* unique_id is keyed on the numeric id, NOT the slug: renaming a signal must
     * rename the HA entity, not create a second one and orphan the first. */
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_sig%u_tx", s_dev_uid, m->id);
    cJSON_AddStringToObject(d, "unique_id", buf);
    snprintf(buf, sizeof(buf), "%s_%s", s_dev_slug, slug);
    cJSON_AddStringToObject(d, "object_id", buf);

    char t[DB_MQTT_TOPIC_MAX];
    button_topic(t, sizeof(t), slug, "press");
    cJSON_AddStringToObject(d, "command_topic", t);
    cJSON_AddStringToObject(d, "payload_press", "PRESS");
    cJSON_AddStringToObject(d, "icon", "mdi:bell-ring-outline");
    add_availability(d);
    add_device(d);

    char object[DB_MQTT_SLUG_MAX + 8];
    snprintf(object, sizeof(object), "%s_tx", slug);
    char dt[DB_MQTT_DISC_MAX];
    disc_topic(dt, sizeof(dt), "button", object);
    pub_json(dt, d, 1, 1);
}

/* A `button` entity for an MQTT-triggerable SOURCE_VIRTUAL node, so a virtual
 * input is pressable straight from the HA dashboard and not only from an
 * automation that publishes to the raw topic. */
static void announce_virtual_button(const db_node_t *n)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "name", n->name[0] ? n->name : "Virtual input");

    char buf[80];
    snprintf(buf, sizeof(buf), "%s_node%u", s_dev_uid, n->id);
    cJSON_AddStringToObject(d, "unique_id", buf);
    char nslug[DB_MQTT_SLUG_MAX];
    slugify(n->name[0] ? n->name : n->topic, nslug, sizeof(nslug));
    if (!nslug[0]) snprintf(nslug, sizeof(nslug), "node_%u", n->id);
    snprintf(buf, sizeof(buf), "%s_%s", s_dev_slug, nslug);
    cJSON_AddStringToObject(d, "object_id", buf);

    char t[DB_MQTT_TOPIC_MAX];
    snprintf(t, sizeof(t), "%s/trigger/%s", s_base, n->topic);
    cJSON_AddStringToObject(d, "command_topic", t);
    cJSON_AddStringToObject(d, "payload_press", "PRESS");
    cJSON_AddStringToObject(d, "icon", "mdi:gesture-tap-button");
    add_availability(d);
    add_device(d);

    char object[24];
    snprintf(object, sizeof(object), "node%u", n->id);
    char dt[DB_MQTT_DISC_MAX];
    disc_topic(dt, sizeof(dt), "button", object);
    pub_json(dt, d, 1, 1);
}

/*
 * The catch-all for bursts that match no stored signal — the normal case behind
 * a `source.any_rf -> sink.mqtt` proxy. Two complementary entities, because the
 * two questions users ask about an unknown remote are different:
 *
 *   - device trigger on <base>/unknown/state — "run this automation whenever ANY
 *     unregistered remote is pressed" (a momentary event, so a trigger, for the
 *     same reasons as a registered button);
 *   - a retained diagnostic sensor showing the last unknown fingerprint, with
 *     the decode and RSSI as attributes — "what code did that remote send?",
 *     which is what a user reads before deciding to learn it. A trigger alone
 *     cannot answer that: it leaves nothing behind to look at.
 */
static void announce_unknown(void)
{
    char state_t[DB_MQTT_TOPIC_MAX];
    topic_of(state_t, sizeof(state_t), "unknown/state");
    announce_trigger(state_t, "Unregistered remote", "unknown_press");

    char last_t[DB_MQTT_TOPIC_MAX];
    topic_of(last_t, sizeof(last_t), "unknown");

    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddStringToObject(d, "name", "Last unknown code");
    char buf[64];
    snprintf(buf, sizeof(buf), "%s_unknown", s_dev_uid);
    cJSON_AddStringToObject(d, "unique_id", buf);
    snprintf(buf, sizeof(buf), "%s_unknown", s_dev_slug);
    cJSON_AddStringToObject(d, "object_id", buf);
    cJSON_AddStringToObject(d, "state_topic", last_t);
    cJSON_AddStringToObject(d, "value_template", "{{ value_json.fingerprint }}");
    cJSON_AddStringToObject(d, "json_attributes_topic", last_t);
    cJSON_AddStringToObject(d, "icon", "mdi:help-rhombus-outline");
    cJSON_AddStringToObject(d, "entity_category", "diagnostic");
    add_availability(d);
    add_device(d);

    char dt[DB_MQTT_DISC_MAX];
    disc_topic(dt, sizeof(dt), "sensor", "unknown");
    pub_json(dt, d, 1, 1);
}

/* Radio telemetry: one sensor (the live noise floor, with the last press as
 * attributes) and one binary_sensor (is a CC1101 actually there). Both are
 * diagnostics — they answer "why did nothing ring?", which on a 433 MHz box is
 * either "no radio" or "the band is too noisy to hear the remote". */
static void announce_radio(void)
{
    char state_t[DB_MQTT_TOPIC_MAX];
    topic_of(state_t, sizeof(state_t), "radio");
    char dt[DB_MQTT_DISC_MAX];
    char buf[64];

    cJSON *d = cJSON_CreateObject();
    if (d) {
        cJSON_AddStringToObject(d, "name", "Radio RSSI");
        snprintf(buf, sizeof(buf), "%s_rssi", s_dev_uid);
        cJSON_AddStringToObject(d, "unique_id", buf);
        snprintf(buf, sizeof(buf), "%s_rssi", s_dev_slug);
        cJSON_AddStringToObject(d, "object_id", buf);
        cJSON_AddStringToObject(d, "state_topic", state_t);
        cJSON_AddStringToObject(d, "value_template", "{{ value_json.rssi_dbm }}");
        cJSON_AddStringToObject(d, "json_attributes_topic", state_t);
        cJSON_AddStringToObject(d, "unit_of_measurement", "dBm");
        cJSON_AddStringToObject(d, "device_class", "signal_strength");
        cJSON_AddStringToObject(d, "state_class", "measurement");
        cJSON_AddStringToObject(d, "entity_category", "diagnostic");
        add_availability(d);
        add_device(d);
        disc_topic(dt, sizeof(dt), "sensor", "rssi");
        pub_json(dt, d, 1, 1);
    }

    d = cJSON_CreateObject();
    if (d) {
        cJSON_AddStringToObject(d, "name", "Radio");
        snprintf(buf, sizeof(buf), "%s_radio", s_dev_uid);
        cJSON_AddStringToObject(d, "unique_id", buf);
        snprintf(buf, sizeof(buf), "%s_radio", s_dev_slug);
        cJSON_AddStringToObject(d, "object_id", buf);
        cJSON_AddStringToObject(d, "state_topic", state_t);
        cJSON_AddStringToObject(d, "value_template",
                                "{{ 'ON' if value_json.present else 'OFF' }}");
        cJSON_AddStringToObject(d, "payload_on", "ON");
        cJSON_AddStringToObject(d, "payload_off", "OFF");
        cJSON_AddStringToObject(d, "device_class", "connectivity");
        cJSON_AddStringToObject(d, "entity_category", "diagnostic");
        add_availability(d);
        add_device(d);
        disc_topic(dt, sizeof(dt), "binary_sensor", "radio");
        pub_json(dt, d, 1, 1);
    }
}

/* An empty RETAINED payload is how MQTT discovery says "forget this entity".
 * Without it a deleted signal leaves a permanently unavailable entity behind in
 * Home Assistant that only a manual purge removes. */
static void retire_signal(const char *slug)
{
    char object[DB_MQTT_SLUG_MAX + 8];
    char dt[DB_MQTT_DISC_MAX];
    snprintf(object, sizeof(object), "%s_press", slug);
    disc_topic(dt, sizeof(dt), "device_automation", object);
    pub(dt, "", 1, 1);
    snprintf(object, sizeof(object), "%s_tx", slug);
    disc_topic(dt, sizeof(dt), "button", object);
    pub(dt, "", 1, 1);
    ESP_LOGI(TAG, "retired discovery for button '%s'", slug);
}

static void retire_virtual(uint16_t node_id)
{
    char object[24];
    char dt[DB_MQTT_DISC_MAX];
    snprintf(object, sizeof(object), "node%u", node_id);
    disc_topic(dt, sizeof(dt), "button", object);
    pub(dt, "", 1, 1);
    ESP_LOGI(TAG, "retired discovery for virtual node %u", node_id);
}

static void retire_shared(void)
{
    char dt[DB_MQTT_DISC_MAX];
    disc_topic(dt, sizeof(dt), "sensor", "rssi");
    pub(dt, "", 1, 1);
    disc_topic(dt, sizeof(dt), "binary_sensor", "radio");
    pub(dt, "", 1, 1);
    disc_topic(dt, sizeof(dt), "sensor", "unknown");
    pub(dt, "", 1, 1);
    disc_topic(dt, sizeof(dt), "device_automation", "unknown_press");
    pub(dt, "", 1, 1);
}

/* ---- trigger payloads ------------------------------------------------------ */

/* Fill in whatever the caller could not know. A trigger that names a stored
 * signal gains its name, fingerprint and decode from the store; one that names
 * nothing (an unregistered burst) keeps its zeros and gets a usable label. */
static void trigger_enrich(db_trigger_t *t)
{
    const db_signal_meta_t *m = t->signal_id ? db_signals_get(t->signal_id) : NULL;
    if (m) {
        if (!t->label[0]) strlcpy(t->label, m->name, sizeof(t->label));
        if (!t->fingerprint) t->fingerprint = m->fingerprint;
        if (!t->decoded_valid && m->decoded_valid) {
            t->decoded_valid  = true;
            t->decoded_id     = m->decoded_id;
            t->decoded_button = m->decoded_button;
            strlcpy(t->protocol, m->protocol, sizeof(t->protocol));
        }
    }
    if (!t->label[0]) strlcpy(t->label, "unknown", sizeof(t->label));
}

/*
 * The payload shape of docs/API.md "MQTT sink payloads carry the trigger".
 * signal_id 0 and decoded null are ordinary values here, not error cases: they
 * are what a wildcard proxy publishes for every remote in the neighbourhood.
 */
static cJSON *trigger_doc(const db_trigger_t *t, uint16_t node_id)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;

    cJSON_AddNumberToObject(d, "signal_id", t->signal_id);
    cJSON_AddStringToObject(d, "label", t->label[0] ? t->label : "unknown");

    if (t->fingerprint) {
        char fp[12];
        snprintf(fp, sizeof(fp), "%08" PRIx32, t->fingerprint);
        cJSON_AddStringToObject(d, "fingerprint", fp);
    } else {
        cJSON_AddNullToObject(d, "fingerprint");
    }

    cJSON_AddNumberToObject(d, "rssi_dbm", t->rssi_dbm);
    cJSON_AddNumberToObject(d, "repeats", t->repeats);

    if (t->decoded_valid) {
        cJSON *dec = cJSON_AddObjectToObject(d, "decoded");
        if (dec) {
            cJSON_AddStringToObject(dec, "protocol", t->protocol);
            cJSON_AddNumberToObject(dec, "id", (double)t->decoded_id);
            cJSON_AddNumberToObject(dec, "button", t->decoded_button);
        }
    } else {
        cJSON_AddNullToObject(d, "decoded");   /* unknown protocol: normal */
    }

    const db_node_t *n = node_id ? db_graph_node(node_id) : NULL;
    if (node_id) {
        cJSON *nd = cJSON_AddObjectToObject(d, "node");
        if (nd) {
            cJSON_AddNumberToObject(nd, "id", node_id);
            cJSON_AddStringToObject(nd, "name", (n && n->name[0]) ? n->name : "");
        }
    } else {
        cJSON_AddNullToObject(d, "node");
    }

    cJSON_AddNumberToObject(d, "ts_s", (double)unix_now());
    return d;
}

/* ---- state publishing ----------------------------------------------------- */

static void publish_radio_state(void)
{
    cJSON *d = cJSON_CreateObject();
    if (!d) return;

    bool present = rf_service_radio_present();
    cJSON_AddBoolToObject(d, "present", present);

    int rssi = 0;
    if (present && rf_service_rssi(&rssi) == ESP_OK)
        cJSON_AddNumberToObject(d, "rssi_dbm", rssi);
    else
        cJSON_AddNullToObject(d, "rssi_dbm");

    cJSON_AddNumberToObject(d, "signals", db_signals_count());

    if (s_last_press_id) {
        const db_signal_meta_t *m = db_signals_get(s_last_press_id);
        cJSON_AddStringToObject(d, "last_press", (m && m->name[0]) ? m->name : "?");
        cJSON_AddNumberToObject(d, "last_press_id", s_last_press_id);
        cJSON_AddNumberToObject(d, "last_press_rssi_dbm", s_last_press_rssi);
        cJSON_AddNumberToObject(d, "last_press_ago_s",
                                (double)((esp_timer_get_time() - s_last_press_us) / 1000000));
    } else {
        cJSON_AddNullToObject(d, "last_press");
    }

    char t[DB_MQTT_TOPIC_MAX];
    topic_of(t, sizeof(t), "radio");
    pub_json(t, d, 0, 1);
}

/* An unregistered burst: the momentary event that fires the catch-all device
 * trigger, plus a retained copy so the diagnostic sensor has something to show
 * after a Home Assistant restart. */
static void publish_unknown(const db_trigger_t *t, uint16_t node_id)
{
    char topic[DB_MQTT_TOPIC_MAX];
    topic_of(topic, sizeof(topic), "unknown/state");
    pub_json(topic, trigger_doc(t, node_id), 0, 0);
    topic_of(topic, sizeof(topic), "unknown");
    pub_json(topic, trigger_doc(t, node_id), 0, 1);
}

static void publish_press(db_trigger_t *t)
{
    const char *slug = slug_for_id(t->signal_id);
    if (!slug) {
        /* A signal learned since the last announce. Resolve it and make sure HA
         * hears about the new button before the next press. */
        slug_table_build();
        slug = slug_for_id(t->signal_id);
        if (!slug) {
            ESP_LOGW(TAG, "press for unknown signal %u", t->signal_id);
            return;
        }
        post_simple(MSG_ANNOUNCE);
    }

    cJSON *d = cJSON_CreateObject();
    if (!d) return;
    cJSON_AddNumberToObject(d, "id", t->signal_id);
    cJSON_AddStringToObject(d, "name", t->label[0] ? t->label : slug);
    cJSON_AddNumberToObject(d, "rssi_dbm", t->rssi_dbm);
    cJSON_AddNumberToObject(d, "repeats", t->repeats);
    if (t->fingerprint) {
        char fp[12];
        snprintf(fp, sizeof(fp), "%08" PRIx32, t->fingerprint);
        cJSON_AddStringToObject(d, "fingerprint", fp);
    }
    cJSON_AddNumberToObject(d, "ts_s", (double)unix_now());
    cJSON_AddNumberToObject(d, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    /* Same shape as GET /api/signals: an object when a decoder claimed the
     * frame, null when it did not — an ordinary, fully supported state. */
    if (t->decoded_valid) {
        cJSON *dec = cJSON_AddObjectToObject(d, "decoded");
        if (dec) {
            cJSON_AddStringToObject(dec, "protocol", t->protocol);
            cJSON_AddNumberToObject(dec, "id", (double)t->decoded_id);
            cJSON_AddNumberToObject(dec, "button", t->decoded_button);
        }
    } else {
        cJSON_AddNullToObject(d, "decoded");
    }

    char topic[DB_MQTT_TOPIC_MAX];
    button_topic(topic, sizeof(topic), slug, "state");
    pub_json(topic, d, 0, 0);   /* qos 0, NOT retained — a press is an event */

    s_last_press_id   = t->signal_id;
    s_last_press_rssi = t->rssi_dbm;
    s_last_press_us   = esp_timer_get_time();
    publish_radio_state();
}

/*
 * <base>/event carries every firing in one stream: the API.md trigger document
 * plus the event `kind` and free text, so a single subscription is enough to
 * follow the whole box. A SINK_MQTT node additionally gets a copy on its own
 * topic, in exactly the documented shape and nothing more — that topic is a
 * contract the user configured, not a debug feed.
 */
static void publish_event(uint8_t ev_kind, const db_trigger_t *t, uint16_t node_id,
                          const char *node_topic, const char *text)
{
    if (node_topic && node_topic[0]) {
        char topic[DB_MQTT_TOPIC_MAX];
        snprintf(topic, sizeof(topic), "%s/%s", s_base, node_topic);
        pub_json(topic, trigger_doc(t, node_id), 0, 0);
    }

    cJSON *d = trigger_doc(t, node_id);
    if (!d) return;
    const char *kind = (ev_kind < sizeof(EVENT_KIND_NAMES) / sizeof(EVENT_KIND_NAMES[0]))
                           ? EVENT_KIND_NAMES[ev_kind] : "system";
    cJSON_AddStringToObject(d, "kind", kind);
    if (text && text[0]) cJSON_AddStringToObject(d, "text", text);
    else                 cJSON_AddNullToObject(d, "text");

    char topic[DB_MQTT_TOPIC_MAX];
    topic_of(topic, sizeof(topic), "event");
    pub_json(topic, d, 0, 0);
}

/* ---- announce: discovery + subscriptions ---------------------------------- */

/* Reconcile the <base>/trigger/<suffix> subscriptions against the graph. The
 * broker forgets subscriptions across a disconnect, so s_resub_all makes the
 * next pass treat the table as empty and re-subscribe everything. */
static void sync_trigger_subs(void)
{
    char t[DB_MQTT_TOPIC_MAX];
    const db_node_t *nodes = db_graph_nodes();
    int n = db_graph_node_count();

    if (s_resub_all) {
        s_resub_all = false;
        s_sub_count = 0;   /* the broker dropped them; nothing to unsubscribe */
    }

    /* Drop subscriptions whose node is gone, disabled, or no longer virtual. */
    for (int i = 0; i < s_sub_count;) {
        uint16_t owner = 0;
        for (int j = 0; j < n && nodes; j++) {
            if (nodes[j].type != DB_NODE_SOURCE_VIRTUAL || !nodes[j].enabled) continue;
            if (!nodes[j].topic[0]) continue;
            if (strcmp(nodes[j].topic, s_subs[i].suffix) == 0) { owner = nodes[j].id; break; }
        }
        if (owner) {
            s_subs[i].node_id = owner;   /* the suffix may have moved nodes */
            i++;
        } else {
            snprintf(t, sizeof(t), "%s/trigger/%s", s_base, s_subs[i].suffix);
            esp_mqtt_client_unsubscribe(s_client, t);
            ESP_LOGI(TAG, "unsubscribed %s", t);
            s_subs[i] = s_subs[--s_sub_count];
        }
    }

    /* Subscribe to anything new. */
    for (int j = 0; j < n && nodes && s_sub_count < DB_NODE_MAX; j++) {
        if (nodes[j].type != DB_NODE_SOURCE_VIRTUAL || !nodes[j].enabled) continue;
        if (!nodes[j].topic[0]) continue;   /* UI/REST only, by the user's choice */
        bool have = false;
        for (int i = 0; i < s_sub_count; i++)
            if (strcmp(s_subs[i].suffix, nodes[j].topic) == 0) { have = true; break; }
        if (have) continue;
        strlcpy(s_subs[s_sub_count].suffix, nodes[j].topic, DB_NODE_TOPIC_MAX);
        s_subs[s_sub_count].node_id = nodes[j].id;
        s_sub_count++;
        snprintf(t, sizeof(t), "%s/trigger/%s", s_base, nodes[j].topic);
        esp_mqtt_client_subscribe(s_client, t, 0);
        ESP_LOGI(TAG, "subscribed %s -> node %u", t, nodes[j].id);
    }
}

/*
 * Re-resolve every slug, retire what disappeared, publish the current discovery
 * set, and re-sync the trigger subscriptions. Run on every (re)connect (retained
 * discovery must survive a broker restart, which loses it) and after any signal
 * or graph change. Idempotent by construction.
 */
static void announce(void)
{
    if (!s_connected) return;
    slug_table_build();

    bool ha = s_cfg->mqtt_homeassistant;

    /* Anything we announced under a slug that no longer resolves to the same
     * signal — deleted, renamed, or shuffled by collision resolution. */
    for (int i = 0; i < s_ann_count; i++) {
        bool still = false;
        for (int j = 0; j < s_slug_count && !still; j++)
            still = (s_ann_id[i] == s_slug_id[j]) &&
                    strcmp(s_ann_slug[i], s_slug[j]) == 0;
        if (!still || !ha) retire_signal(s_ann_slug[i]);
    }

    if (ha) {
        for (int i = 0; i < s_slug_count; i++) {
            const db_signal_meta_t *m = db_signals_get(s_slug_id[i]);
            if (!m) continue;
            announce_signal_trigger(m, s_slug[i]);
            announce_tx_button(m, s_slug[i]);
        }
        announce_radio();
        announce_unknown();
        s_shared_retired = false;
    } else if (!s_shared_retired) {
        /* Discovery is off (or was just turned off): clear whatever we — or an
         * earlier firmware — published, so HA drops the device instead of
         * keeping a set of permanently unavailable entities. */
        retire_shared();
        s_shared_retired = true;
    }

    /* Snapshot what is now out there. */
    s_ann_count = ha ? s_slug_count : 0;
    for (int i = 0; i < s_ann_count; i++) {
        s_ann_id[i] = s_slug_id[i];
        strlcpy(s_ann_slug[i], s_slug[i], DB_MQTT_SLUG_MAX);
    }

    sync_trigger_subs();

    /* Virtual-source buttons: announce the live set, retire the rest. */
    for (int i = 0; i < s_ann_virt_count; i++) {
        bool still = false;
        for (int j = 0; j < s_sub_count && !still; j++)
            still = (s_subs[j].node_id == s_ann_virt[i]);
        if (!still || !ha) retire_virtual(s_ann_virt[i]);
    }
    s_ann_virt_count = 0;
    if (ha) {
        for (int j = 0; j < s_sub_count; j++) {
            const db_node_t *n = db_graph_node(s_subs[j].node_id);
            if (!n) continue;
            announce_virtual_button(n);
            s_ann_virt[s_ann_virt_count++] = n->id;
        }
    }

    publish_radio_state();
    ESP_LOGI(TAG, "announced %d signal(s), %d virtual trigger(s)%s",
             s_slug_count, s_sub_count, ha ? "" : " (HA discovery off)");
}

/* ---- inbound commands (executed on OUR task, never the event task) --------- */

/* Optional JSON body: {"repeats":6,"gap_us":8000}. Anything else — "", "PRESS",
 * "ON", whatever an automation happens to send — means "transmit with the
 * configured defaults", because requiring a body would make the topic useless
 * from a plain `mosquitto_pub -n`. */
static void parse_tx_args(const char *payload, uint8_t *repeats, uint32_t *gap_us)
{
    *repeats = s_cfg->tx_repeats ? s_cfg->tx_repeats : 6;
    *gap_us  = s_cfg->tx_gap_us  ? s_cfg->tx_gap_us  : 8000;
    if (!payload) return;
    while (*payload == ' ') payload++;
    if (*payload != '{') return;

    cJSON *j = cJSON_Parse(payload);
    if (!j) return;
    cJSON *r = cJSON_GetObjectItem(j, "repeats");
    if (cJSON_IsNumber(r) && r->valueint > 0 && r->valueint <= 64)
        *repeats = (uint8_t)r->valueint;
    cJSON *g = cJSON_GetObjectItem(j, "gap_us");
    if (cJSON_IsNumber(g) && g->valueint >= 0 && g->valueint <= 1000000)
        *gap_us = (uint32_t)g->valueint;
    cJSON_Delete(j);
}

/* 1 KB of pulse durations: static, so it never lands on the task stack. Only the
 * bridge task uses it. */
static rf_frame_t s_tx_frame;

static void handle_transmit(const char *slug, const char *payload)
{
    slug_table_build();
    uint16_t id = id_for_slug(slug);
    if (!id) {
        ESP_LOGW(TAG, "press command for unknown button '%s'", slug);
        return;
    }
    esp_err_t err = db_signals_load_frame(id, &s_tx_frame);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load frame %u: %s", id, esp_err_to_name(err));
        return;
    }
    uint8_t repeats; uint32_t gap_us;
    parse_tx_args(payload, &repeats, &gap_us);

    /* Blocks for the airtime of the burst — which is precisely why this runs
     * here and not in the esp-mqtt event handler. */
    err = rf_service_transmit(&s_tx_frame, repeats, gap_us);
    const db_signal_meta_t *m = db_signals_get(id);
    ESP_LOGI(TAG, "MQTT transmit '%s' x%u: %s", slug, repeats, esp_err_to_name(err));
    db_events_push(DB_EV_TRANSMIT, id, 0, 0, repeats, "MQTT: %s",
                   (m && m->name[0]) ? m->name : slug);

    db_trigger_t t = { .signal_id = id, .repeats = repeats };
    trigger_enrich(&t);
    publish_event(DB_EV_TRANSMIT, &t, 0, NULL,
                  err == ESP_OK ? "transmitted via MQTT" : esp_err_to_name(err));
}

static void handle_fire(const char *suffix)
{
    uint16_t node_id = 0;
    for (int i = 0; i < s_sub_count; i++)
        if (strcmp(s_subs[i].suffix, suffix) == 0) { node_id = s_subs[i].node_id; break; }
    if (!node_id) {
        ESP_LOGW(TAG, "trigger for unknown topic '%s'", suffix);
        return;
    }
    /* Starts a graph traversal, which may end in a transmit — again, work that
     * has no business running on the MQTT event task. */
    esp_err_t err = db_graph_fire_node(node_id);
    ESP_LOGI(TAG, "MQTT trigger '%s' -> node %u: %s", suffix, node_id,
             esp_err_to_name(err));
}

/* ---- the bridge task ------------------------------------------------------ */

static void bridge_task(void *arg)
{
    (void)arg;
    db_mqtt_msg_t m;

    for (;;) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(DB_MQTT_TICK_MS)) != pdTRUE) {
            /* Periodic tick: keep the retained radio telemetry fresh so the noise
             * floor in HA is current rather than whatever it was at boot. */
            if (s_connected) publish_radio_state();
            continue;
        }
        if (m.kind == MSG_STOP) break;
        if (!s_connected) continue;   /* offline: an event has no value late */

        switch ((db_mqtt_msg_kind_t)m.kind) {
        case MSG_PUBLISH:
            publish_radio_state();
            break;
        case MSG_ANNOUNCE:
            announce();
            break;
        case MSG_PRESS:
            trigger_enrich(&m.trig);
            publish_press(&m.trig);
            publish_event(DB_EV_BUTTON_PRESS, &m.trig, 0, NULL, NULL);
            break;
        case MSG_TRANSMIT:
            handle_transmit(m.arg, m.text);
            break;
        case MSG_FIRE:
            handle_fire(m.arg);
            break;
        case MSG_EVENT:
            trigger_enrich(&m.trig);
            /* An unregistered burst reaching an MQTT sink is the whole point of
             * the wildcard proxy: give it the catch-all topics too, so it is
             * visible in HA despite having no entity of its own. */
            if (!m.trig.signal_id && m.trig.fingerprint)
                publish_unknown(&m.trig, m.node_id);
            publish_event(m.ev_kind, &m.trig, m.node_id, m.arg, m.text);
            break;
        default:
            break;
        }
    }

    ESP_LOGI(TAG, "bridge task exiting");
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- esp-mqtt event handling ---------------------------------------------- */

/*
 * Split "<base>/button/<slug>/press" or "<base>/trigger/<suffix>" and hand the
 * result to the task. Nothing here allocates, blocks, touches flash or takes a
 * lock — the topic is copied, sliced, and queued.
 */
static void route_inbound(const char *topic, int tlen, const char *data, int dlen)
{
    char t[DB_MQTT_TOPIC_MAX];
    if (tlen <= 0 || tlen >= (int)sizeof(t)) return;
    memcpy(t, topic, (size_t)tlen);
    t[tlen] = '\0';

    size_t blen = strlen(s_base);
    if (strncmp(t, s_base, blen) != 0 || t[blen] != '/') return;
    char *rest = t + blen + 1;

    db_mqtt_msg_t m = {0};

    if (strncmp(rest, "button/", 7) == 0) {
        char *slug = rest + 7;
        char *leaf = strrchr(slug, '/');
        if (!leaf || strcmp(leaf, "/press") != 0) return;
        *leaf = '\0';
        if (!slug[0] || strlen(slug) >= DB_MQTT_ARG_MAX) return;
        m.kind = MSG_TRANSMIT;
        strlcpy(m.arg, slug, sizeof(m.arg));
        if (dlen > 0) {
            int n = dlen < (int)sizeof(m.text) - 1 ? dlen : (int)sizeof(m.text) - 1;
            memcpy(m.text, data, (size_t)n);
            m.text[n] = '\0';
        }
        post(&m);
        return;
    }

    if (strncmp(rest, "trigger/", 8) == 0) {
        const char *suffix = rest + 8;
        if (!suffix[0] || strlen(suffix) >= DB_MQTT_ARG_MAX) return;
        m.kind = MSG_FIRE;
        strlcpy(m.arg, suffix, sizeof(m.arg));   /* payload deliberately ignored */
        post(&m);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = event_data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        s_connected = true;
        pub(s_status_topic, "online", 1, 1);

        /* One wildcard covers every button, so learning a signal never needs a
         * new subscription. The per-node trigger topics cannot use a wildcard:
         * a user's suffix may itself contain '/'. */
        char sub[DB_MQTT_TOPIC_MAX];
        snprintf(sub, sizeof(sub), "%s/button/+/press", s_base);
        esp_mqtt_client_subscribe(s_client, sub, 0);

        /* The broker dropped our subscriptions with the session, and retained
         * discovery may not have survived a broker restart: re-do both. */
        s_resub_all = true;
        post_simple(MSG_ANNOUNCE);
        ESP_LOGI(TAG, "connected; subscribed %s", sub);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_DATA:
        route_inbound(e->topic, e->topic_len, e->data, e->data_len);
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "mqtt error (type %d)", (int)e->error_handle->error_type);
        break;
    default:
        break;
    }
}

/* ---- public API ----------------------------------------------------------- */

void db_mqtt_start(db_config_t *cfg)
{
    if (!cfg) return;
    s_cfg = cfg;
    if (!cfg->mqtt_enabled) {
        ESP_LOGI(TAG, "MQTT disabled in config");
        return;
    }
    if (!cfg->mqtt_host[0]) {
        ESP_LOGW(TAG, "MQTT enabled but no broker host configured");
        return;
    }
    if (s_client) {
        ESP_LOGW(TAG, "already started");
        return;
    }

    s_base = cfg->mqtt_base_topic[0] ? cfg->mqtt_base_topic : DB_MQTT_DEFAULT_BASE;
    s_disc = cfg->mqtt_discovery_prefix[0] ? cfg->mqtt_discovery_prefix
                                           : DB_MQTT_DEFAULT_DISCOVERY;
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/status", s_base);

    /* Device identity. The MAC is the stable key; the hostname is only cosmetic
     * and may change under the user's hands at any time. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_uid, sizeof(s_dev_uid), "klingelbox_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_dev_mac, sizeof(s_dev_mac), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    slugify(cfg->hostname[0] ? cfg->hostname : "klingelbox", s_dev_slug, sizeof(s_dev_slug));
    if (!s_dev_slug[0]) strlcpy(s_dev_slug, "klingelbox", sizeof(s_dev_slug));
    const esp_app_desc_t *app = esp_app_get_description();
    strlcpy(s_sw_version, app ? app->version : "unknown", sizeof(s_sw_version));

    s_queue = xQueueCreate(DB_MQTT_QUEUE_LEN, sizeof(db_mqtt_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }

    char uri[DB_STR_HOST + 24];
    snprintf(uri, sizeof(uri), "mqtt://%s:%u", cfg->mqtt_host,
             cfg->mqtt_port ? cfg->mqtt_port : 1883);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = uri,
        .credentials.username = cfg->mqtt_user[0] ? cfg->mqtt_user : NULL,
        .credentials.authentication.password = cfg->mqtt_pass[0] ? cfg->mqtt_pass : NULL,
        .credentials.client_id = s_dev_uid,
        .session.keepalive = 30,
        /* THE point of the availability topic: when this box loses power or
         * falls off the network, the broker publishes "offline" on its behalf
         * and every HA entity goes unavailable, instead of a dead doorbell
         * looking exactly like a doorbell nobody has rung. */
        .session.last_will = {
            .topic   = s_status_topic,
            .msg     = "offline",
            .msg_len = 7,
            .qos     = 1,
            .retain  = 1,
        },
        /* Discovery is a retained qos-1 burst of two configs per signal plus the
         * shared diagnostics; give the outbox room so none of it is dropped. */
        .outbox.limit = 32768,
    };

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);

    /* 6 KB: cJSON discovery documents plus the topic buffers. The 1 KB pulse
     * frame is static, not on this stack. */
    if (xTaskCreate(bridge_task, "db_mqtt", 6144, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "MQTT bridge -> %s, base '%s'%s", uri, s_base,
             cfg->mqtt_homeassistant ? ", HA discovery on" : "");
}

void db_mqtt_stop(void)
{
    if (!s_client) return;

    if (s_connected) pub(s_status_topic, "offline", 1, 1);

    /* Ask the task to leave rather than deleting it: it may be inside
     * rf_service_transmit() holding the radio mutex, and killing it there would
     * wedge the radio for the rest of the boot. */
    if (s_task) {
        db_mqtt_msg_t m = { .kind = MSG_STOP };
        xQueueSend(s_queue, &m, pdMS_TO_TICKS(100));
        for (int i = 0; i < 150 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
        if (s_task) ESP_LOGW(TAG, "bridge task did not exit; leaking it");
    }

    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;

    if (!s_task && s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_ann_count = 0;
    s_ann_virt_count = 0;
    s_sub_count = 0;
    s_shared_retired = false;
    ESP_LOGI(TAG, "stopped");
}

bool db_mqtt_connected(void)
{
    return s_connected;
}

void db_mqtt_notify_publish(void)
{
    post_simple(MSG_PUBLISH);
}

void db_mqtt_on_signal_press(uint16_t signal_id, int rssi_dbm, uint8_t repeats)
{
    if (!signal_id) return;
    db_mqtt_msg_t m = { .kind = MSG_PRESS };
    m.trig.signal_id = signal_id;
    m.trig.rssi_dbm  = (int16_t)rssi_dbm;
    m.trig.repeats   = repeats;
    post(&m);
}

void db_mqtt_on_signals_changed(void)
{
    post_simple(MSG_ANNOUNCE);
}

void db_mqtt_on_graph_changed(void)
{
    post_simple(MSG_ANNOUNCE);
}

void db_mqtt_sink(const db_node_t *node, const db_trigger_t *trig, void *ctx)
{
    (void)ctx;
    if (!node) return;
    db_mqtt_msg_t m = {
        .kind    = MSG_EVENT,
        .ev_kind = DB_EV_NODE_FIRED,
        .node_id = node->id,
    };
    if (trig) m.trig = *trig;      /* signal_id 0 = an unregistered burst */
    strlcpy(m.arg, node->topic, sizeof(m.arg));   /* "" = event stream only */
    strlcpy(m.text, node->name, sizeof(m.text));
    post(&m);
}

void db_mqtt_publish_event(db_event_kind_t kind, uint16_t signal_id,
                           uint16_t node_id, const char *text)
{
    db_mqtt_msg_t m = {
        .kind    = MSG_EVENT,
        .ev_kind = (uint8_t)kind,
        .node_id = node_id,
    };
    m.trig.signal_id = signal_id;
    if (text) strlcpy(m.text, text, sizeof(m.text));
    post(&m);
}
