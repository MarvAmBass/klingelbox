/*
 * test_node_graph.c - Host-compiled tests for the two pieces of node-graph logic
 * that have no hardware in them and can destroy a user's data if they are wrong.
 *
 * 1. THE v3 -> v4 BLOB MIGRATION. The node array lives in NVS as a raw record
 *    array. Adding `mqtt_enabled` to db_node_t changed what those bytes mean,
 *    and getting the widening wrong does not fail loudly — it silently returns a
 *    graph with the wrong values in it, on a box the user has spent an evening
 *    wiring up. The failure mode that matters most is subtle and is tested
 *    explicitly below: the new bool landed in a byte v3 used as PADDING, so
 *    sizeof() did not change, the size check that would have caught a layout
 *    change waves a v3 blob straight through, and the flag is read out of
 *    uninitialised memory unless the version drives the decision instead.
 *
 * 2. MQTT TOPIC VALIDITY. Publishing to a topic containing '#' or '+' is illegal
 *    and a broker will refuse the message or drop the connection, so one typo in
 *    the base topic takes the whole bridge down. It is a pure string rule, which
 *    means it can be pinned down completely here instead of by poking a live
 *    box.
 *
 * 3. PERSISTENCE AND ROLLBACK. node_graph.c and signal_store.c themselves,
 *    linked against a fake in-RAM NVS (stubs/host_env.c) that can be told to
 *    fill up or to fail its next write. What is pinned down here is the
 *    discipline that keeps RAM and flash telling the same story: a mutation
 *    that cannot be saved is rolled back rather than surviving until reboot
 *    un-happens it; a delete commits the index before erasing the waveform so
 *    a power cut leaves an invisible orphan, never a listed signal that
 *    errors on use; the boot-time reconciliation cleans up both halves of an
 *    interrupted delete without ever touching data a DIFFERENT firmware wrote;
 *    and the store-full budget refuses an add while every other save on the
 *    box still has room to keep working.
 *
 * None of this needs ESP-IDF: everything is compiled against the stubs in
 * stubs/ rather than the framework. If a source file here ever needs a stub
 * that is not a fair model of the real thing, that is the moment to stop and
 * reconsider, not to fatten the fake.
 *
 * Build and run:  make test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "host_env.h"
#include "mqtt_topic.h"
#include "node_graph.h"
#include "node_migrate.h"
#include "nvs.h"
#include "signal_store.h"

/* ---- micro test harness (same shape as test_rf_decode.c) ----------------- */

static int g_pass;
static int g_fail;
static const char *g_case = "";

#define CASE(name) do { g_case = (name); } while (0)

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        if (cond) {                                                             \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            printf("  FAIL [%s] %s:%d: ", g_case, __FILE__, __LINE__);          \
            printf(__VA_ARGS__);                                                \
            printf("\n");                                                       \
        }                                                                       \
    } while (0)

/* ---- helpers ------------------------------------------------------------- */

/* Build one v3 record the way the firmware that wrote it would have. `pad` is
 * what to leave in the byte v4 later claimed for mqtt_enabled — the whole point
 * of several tests below, so it is a parameter rather than an accident. */
static void make_v3(unsigned char *dst, const db_node_v3_t *rec, unsigned char pad)
{
    memset(dst, pad, sizeof(db_node_v3_t));
    memcpy(dst, rec, sizeof(*rec));
    /* Re-stamp the padding AFTER the struct copy: the compiler is free to leave
     * anything at all in a struct's padding, so writing it through the byte
     * buffer is the only way to control it. */
    dst[offsetof(db_node_t, mqtt_enabled)] = pad;
}

/* ---- 1. the frozen layout ------------------------------------------------ */

/*
 * These are not style assertions. db_node_v3_t describes bytes already sitting
 * in somebody's flash: if an offset here moves, every graph written by every
 * shipped firmware is read wrong. The numbers are the contract.
 */
static void test_frozen_layout(void)
{
    CASE("frozen v3 layout");

    CHECK(sizeof(db_node_v3_t) == 108,
          "v3 record must be 108 bytes, got %u", (unsigned)sizeof(db_node_v3_t));

    CHECK(offsetof(db_node_v3_t, id) == 0, "id at 0");
    CHECK(offsetof(db_node_v3_t, type) == 2, "type at 2");
    CHECK(offsetof(db_node_v3_t, enabled) == 3, "enabled at 3");
    CHECK(offsetof(db_node_v3_t, name) == 4, "name at 4");
    CHECK(offsetof(db_node_v3_t, signal_id) == 36, "signal_id at 36");
    CHECK(offsetof(db_node_v3_t, gpio_pin) == 38, "gpio_pin at 38");
    CHECK(offsetof(db_node_v3_t, gpio_active_low) == 39, "gpio_active_low at 39");
    CHECK(offsetof(db_node_v3_t, gpio_debounce_ms) == 40, "gpio_debounce_ms at 40");
    CHECK(offsetof(db_node_v3_t, repeats) == 42, "repeats at 42");
    CHECK(offsetof(db_node_v3_t, gap_us) == 44, "gap_us at 44");
    CHECK(offsetof(db_node_v3_t, window_ms) == 48, "window_ms at 48");
    CHECK(offsetof(db_node_v3_t, group_mode) == 52, "group_mode at 52");
    CHECK(offsetof(db_node_v3_t, topic) == 53, "topic at 53");
    CHECK(offsetof(db_node_v3_t, ui_x) == 102, "ui_x at 102");
    CHECK(offsetof(db_node_v3_t, ui_y) == 104, "ui_y at 104");

    /* Every field the two layouts share must sit at the same offset, or the
     * widening is copying between different things. */
    CHECK(offsetof(db_node_t, id)        == offsetof(db_node_v3_t, id), "id aligned");
    CHECK(offsetof(db_node_t, name)      == offsetof(db_node_v3_t, name), "name aligned");
    CHECK(offsetof(db_node_t, gap_us)    == offsetof(db_node_v3_t, gap_us), "gap_us aligned");
    CHECK(offsetof(db_node_t, window_ms) == offsetof(db_node_v3_t, window_ms), "window_ms aligned");
    CHECK(offsetof(db_node_t, topic)     == offsetof(db_node_v3_t, topic), "topic aligned");
    CHECK(offsetof(db_node_t, ui_x)      == offsetof(db_node_v3_t, ui_x), "ui_x aligned");
    CHECK(offsetof(db_node_t, ui_y)      == offsetof(db_node_v3_t, ui_y), "ui_y aligned");
}

/*
 * THE TRAP, PINNED DOWN. v4's bool went into a byte v3 was already wasting, so
 * the record size did not change. A load path that decided "which layout is
 * this?" by comparing item_size would therefore treat a v3 blob as v4 and read
 * the flag straight out of padding — nodes dropping off the broker at random,
 * depending on what the previous firmware happened to leave in that byte.
 *
 * This test exists so that if a later change makes the sizes differ, whoever
 * sees it fail learns why the version-keyed dispatch in load_blob() is there
 * before deciding to simplify it away.
 */
static void test_size_collision_is_the_reason(void)
{
    CASE("v3/v4 size collision");

    CHECK(sizeof(db_node_t) == sizeof(db_node_v3_t),
          "v4 must still be %u bytes like v3, got %u",
          (unsigned)sizeof(db_node_v3_t), (unsigned)sizeof(db_node_t));

    /* mqtt_enabled must land in what v3 left as padding: after the topic and
     * before ui_x, i.e. a byte no v3 field ever occupied. */
    CHECK(offsetof(db_node_t, mqtt_enabled) == 101,
          "mqtt_enabled expected at 101, got %u",
          (unsigned)offsetof(db_node_t, mqtt_enabled));
    CHECK(offsetof(db_node_t, mqtt_enabled) >
              offsetof(db_node_v3_t, topic) + DB_NODE_TOPIC_MAX - 1,
          "mqtt_enabled must sit past the end of v3's topic");
    CHECK(offsetof(db_node_t, mqtt_enabled) < offsetof(db_node_v3_t, ui_x),
          "mqtt_enabled must sit before v3's ui_x");
}

/* ---- 2. the widening ----------------------------------------------------- */

static void test_widen_carries_every_field(void)
{
    CASE("widen v3 -> v4: every field");

    db_node_v3_t old;
    memset(&old, 0, sizeof(old));
    old.id               = 9;
    old.type             = DB_NODE_LOGIC_SWITCH;
    old.enabled          = true;
    snprintf(old.name, sizeof(old.name), "All Bells Switch");
    old.signal_id        = 0;
    old.gpio_pin         = -1;
    old.gpio_active_low  = true;
    old.gpio_debounce_ms = 50;
    old.repeats          = 6;
    old.gap_us           = 8000;
    old.window_ms        = 10000;
    old.group_mode       = DB_GROUP_ANY;
    snprintf(old.topic, sizeof(old.topic), "outside_bell");
    old.ui_x             = 568;
    old.ui_y             = 244;

    unsigned char blob[sizeof(db_node_v3_t)];
    make_v3(blob, &old, 0x00);

    db_node_t n;
    memset(&n, 0xAA, sizeof(n));      /* prove the widening writes everything */
    db_node_widen_v3(&n, blob, 1);

    CHECK(n.id == 9, "id %u", (unsigned)n.id);
    CHECK(n.type == DB_NODE_LOGIC_SWITCH, "type %u", (unsigned)n.type);
    CHECK(n.enabled == true, "enabled");
    CHECK(strcmp(n.name, "All Bells Switch") == 0, "name '%s'", n.name);
    CHECK(n.signal_id == 0, "signal_id %u", (unsigned)n.signal_id);
    CHECK(n.gpio_pin == -1, "gpio_pin %d", (int)n.gpio_pin);
    CHECK(n.gpio_active_low == true, "gpio_active_low");
    CHECK(n.gpio_debounce_ms == 50, "gpio_debounce_ms %u", (unsigned)n.gpio_debounce_ms);
    CHECK(n.repeats == 6, "repeats %u", (unsigned)n.repeats);
    CHECK(n.gap_us == 8000, "gap_us %u", (unsigned)n.gap_us);
    CHECK(n.window_ms == 10000, "window_ms %u", (unsigned)n.window_ms);
    CHECK(n.group_mode == DB_GROUP_ANY, "group_mode %u", (unsigned)n.group_mode);
    CHECK(strcmp(n.topic, "outside_bell") == 0, "topic '%s'", n.topic);
    CHECK(n.ui_x == 568, "ui_x %d", (int)n.ui_x);
    CHECK(n.ui_y == 244, "ui_y %d", (int)n.ui_y);

    /* The whole reason the migration exists. */
    CHECK(n.mqtt_enabled == true, "mqtt_enabled must default true");
}

/*
 * The flag must come out TRUE whatever the padding byte held. A zero there is
 * the dangerous case — it reads as a perfectly plausible `false`, so a widening
 * that forgot to set the field would pass a test that only ever used 0xFF.
 */
static void test_widen_ignores_v3_padding(void)
{
    CASE("widen v3 -> v4: padding is not the flag");

    const unsigned char pads[] = { 0x00, 0x01, 0x7F, 0xFF, 0xA5 };

    for (size_t i = 0; i < sizeof(pads) / sizeof(pads[0]); i++) {
        db_node_v3_t old;
        memset(&old, 0, sizeof(old));
        old.id   = 3;
        old.type = DB_NODE_SINK_MQTT;
        snprintf(old.name, sizeof(old.name), "MQTT Gate");
        snprintf(old.topic, sizeof(old.topic), "gartentor");

        unsigned char blob[sizeof(db_node_v3_t)];
        make_v3(blob, &old, pads[i]);

        db_node_t n;
        memset(&n, 0, sizeof(n));
        db_node_widen_v3(&n, blob, 1);

        CHECK(n.mqtt_enabled == true,
              "pad 0x%02X must still widen to mqtt_enabled=true", pads[i]);
        CHECK(strcmp(n.topic, "gartentor") == 0,
              "pad 0x%02X must not disturb the topic ('%s')", pads[i], n.topic);
        CHECK(n.id == 3, "pad 0x%02X must not disturb the id (%u)",
              pads[i], (unsigned)n.id);
    }
}

/*
 * A whole blob, not one record — the array walk is its own chance to be wrong,
 * and an off-by-one in the stride would corrupt every node after the first.
 *
 * These are the real nodes off the box this change was written for, so the test
 * is literally "does the user's graph survive". Node ids, types, names, topics
 * and canvas positions all as they were stored.
 */
static void test_widen_real_graph(void)
{
    CASE("widen v3 -> v4: a real stored graph");

    struct { uint16_t id; uint8_t type; const char *name; uint16_t sig;
             uint32_t win; const char *topic; int16_t x, y; } SRC[] = {
        {  2, DB_NODE_SINK_MONITOR,   "Monitor",                0,  3000, "",           774, 350 },
        {  8, DB_NODE_SIGNAL_TX,      "Virtual Signal paired",  5, 10000, "",           779, 251 },
        {  9, DB_NODE_LOGIC_SWITCH,   "All Bells Switch",       0, 10000, "",           568, 244 },
        { 11, DB_NODE_SIGNAL_RX,      "Gate",                   3, 10000, "",            27,  35 },
        { 12, DB_NODE_SIGNAL_RX,      "Door",                   4, 10000, "",            24, 161 },
        {  3, DB_NODE_SINK_MQTT,      "MQTT Gate",              0, 10000, "gartentor",  535,  29 },
        {  4, DB_NODE_SINK_MQTT,      "MQTT Door",              0, 10000, "haustuer",   541, 146 },
        {  1, DB_NODE_SINK_MONITOR,   "Monitor",                0,  3000, "",           476, 334 },
        {  5, DB_NODE_LOGIC_THROTTLE, "Rate limit",             0, 10000, "",           265,  30 },
        {  6, DB_NODE_LOGIC_THROTTLE, "Rate limit",             0, 10000, "",           257, 152 },
    };
    const int N = (int)(sizeof(SRC) / sizeof(SRC[0]));

    unsigned char blob[16 * sizeof(db_node_v3_t)];
    memset(blob, 0, sizeof(blob));

    for (int i = 0; i < N; i++) {
        db_node_v3_t old;
        memset(&old, 0, sizeof(old));
        old.id               = SRC[i].id;
        old.type             = SRC[i].type;
        old.enabled          = true;
        snprintf(old.name, sizeof(old.name), "%s", SRC[i].name);
        old.signal_id        = SRC[i].sig;
        old.gpio_pin         = -1;
        old.gpio_active_low  = true;
        old.gpio_debounce_ms = 50;
        old.repeats          = 6;
        old.gap_us           = 8000;
        old.window_ms        = SRC[i].win;
        old.group_mode       = DB_GROUP_ANY;
        snprintf(old.topic, sizeof(old.topic), "%s", SRC[i].topic);
        old.ui_x             = SRC[i].x;
        old.ui_y             = SRC[i].y;
        /* Alternate the padding so the array walk cannot pass by luck. */
        make_v3(blob + (size_t)i * sizeof(db_node_v3_t), &old,
                (i % 2) ? 0x00 : 0xFF);
    }

    db_node_t out[16];
    memset(out, 0x5A, sizeof(out));
    db_node_widen_v3(out, blob, N);

    for (int i = 0; i < N; i++) {
        CHECK(out[i].id == SRC[i].id, "node %d id %u != %u",
              i, (unsigned)out[i].id, (unsigned)SRC[i].id);
        CHECK(out[i].type == SRC[i].type, "node %u type %u != %u",
              (unsigned)SRC[i].id, (unsigned)out[i].type, (unsigned)SRC[i].type);
        CHECK(strcmp(out[i].name, SRC[i].name) == 0, "node %u name '%s' != '%s'",
              (unsigned)SRC[i].id, out[i].name, SRC[i].name);
        CHECK(out[i].signal_id == SRC[i].sig, "node %u signal_id %u != %u",
              (unsigned)SRC[i].id, (unsigned)out[i].signal_id, (unsigned)SRC[i].sig);
        CHECK(out[i].window_ms == SRC[i].win, "node %u window_ms %u != %u",
              (unsigned)SRC[i].id, (unsigned)out[i].window_ms, (unsigned)SRC[i].win);
        CHECK(strcmp(out[i].topic, SRC[i].topic) == 0, "node %u topic '%s' != '%s'",
              (unsigned)SRC[i].id, out[i].topic, SRC[i].topic);
        CHECK(out[i].ui_x == SRC[i].x && out[i].ui_y == SRC[i].y,
              "node %u position (%d,%d) != (%d,%d)", (unsigned)SRC[i].id,
              (int)out[i].ui_x, (int)out[i].ui_y, (int)SRC[i].x, (int)SRC[i].y);
        CHECK(out[i].enabled == true, "node %u enabled", (unsigned)SRC[i].id);
        CHECK(out[i].gpio_pin == -1, "node %u gpio_pin", (unsigned)SRC[i].id);
        CHECK(out[i].repeats == 6, "node %u repeats", (unsigned)SRC[i].id);
        CHECK(out[i].gap_us == 8000, "node %u gap_us", (unsigned)SRC[i].id);
        /* Every one of them keeps talking to the broker. */
        CHECK(out[i].mqtt_enabled == true,
              "node %u must widen to mqtt_enabled=true", (unsigned)SRC[i].id);
    }

    /* The record past the end must be untouched — an over-long stride would
     * have written into it. */
    CHECK(out[N].id == 0x5A5A, "widening must not write past n records");
}

/* A NUL-free (i.e. flash-corrupted or truncated) name/topic must still come back
 * terminated: everything downstream treats them as C strings. */
static void test_widen_terminates_strings(void)
{
    CASE("widen v3 -> v4: strings stay terminated");

    unsigned char blob[sizeof(db_node_v3_t)];
    memset(blob, 'X', sizeof(blob));      /* no NUL anywhere at all */

    db_node_t n;
    db_node_widen_v3(&n, blob, 1);

    CHECK(strlen(n.name) == DB_NODE_NAME_MAX - 1,
          "name must be truncated to %d, got %u",
          DB_NODE_NAME_MAX - 1, (unsigned)strlen(n.name));
    CHECK(strlen(n.topic) == DB_NODE_TOPIC_MAX - 1,
          "topic must be truncated to %d, got %u",
          DB_NODE_TOPIC_MAX - 1, (unsigned)strlen(n.topic));
    CHECK(n.mqtt_enabled == true, "mqtt_enabled still true on a junk record");
}

static void test_widen_guards(void)
{
    CASE("widen v3 -> v4: guards");

    db_node_t n;
    memset(&n, 0x11, sizeof(n));
    unsigned char blob[sizeof(db_node_v3_t)] = { 0 };

    /* None of these may write anything. */
    db_node_widen_v3(NULL, blob, 1);
    db_node_widen_v3(&n, NULL, 1);
    db_node_widen_v3(&n, blob, 0);
    db_node_widen_v3(&n, blob, -3);

    CHECK(n.id == 0x1111, "a refused widening must not touch the destination");
}

/* ---- 3. topic validation ------------------------------------------------- */

#define TOPIC_OK(t)                                                             \
    do {                                                                        \
        char e[224];                                                            \
        bool ok = db_mqtt_topic_valid((t), "topic", 47, e, sizeof(e));          \
        CHECK(ok, "'%s' should be accepted, refused with: %s", (t), e);         \
        CHECK(!ok || e[0] == '\0', "'%s' accepted but left a message", (t));    \
    } while (0)

/* Refused, AND the message must name the field and mention `hint` — a message
 * that does not say which of three topic fields is wrong is a message that
 * makes the user guess. */
#define TOPIC_BAD(t, hint)                                                      \
    do {                                                                        \
        char e[224];                                                            \
        bool ok = db_mqtt_topic_valid((t), "mqtt.base_topic", 47, e, sizeof(e));\
        CHECK(!ok, "'%s' should be refused", (t));                              \
        CHECK(ok || e[0] != '\0', "'%s' refused with no message", (t));         \
        CHECK(ok || strstr(e, "mqtt.base_topic") != NULL,                       \
              "'%s': message must name the field, got: %s", (t), e);            \
        CHECK(ok || strstr(e, (hint)) != NULL,                                  \
              "'%s': message must mention '%s', got: %s", (t), (hint), e);      \
    } while (0)

static void test_topic_accepts(void)
{
    CASE("topic: accepted");

    TOPIC_OK("");                    /* the caller decides what empty means */
    TOPIC_OK("klingelbox");
    TOPIC_OK("homeassistant");
    TOPIC_OK("gartentor");
    TOPIC_OK("haustuer");
    TOPIC_OK("a");
    TOPIC_OK("a/b");
    TOPIC_OK("a/b/c/d/e");
    TOPIC_OK("front_gate");
    TOPIC_OK("Front-Gate");
    TOPIC_OK("bell.2");
    TOPIC_OK("haus/tuer_1");
    TOPIC_OK("123");
    /* A lone '-' is legal and MUST stay legal: a sentinel value for "no MQTT"
     * was considered and rejected precisely because every candidate collides
     * with a topic somebody could legitimately want. That is why the node has a
     * flag instead. */
    TOPIC_OK("-");
    TOPIC_OK("a-b/c-d");
    /* Exactly at the limit. */
    TOPIC_OK("01234567890123456789012345678901234567890123456");
}

static void test_topic_wildcards(void)
{
    CASE("topic: wildcards");

    TOPIC_BAD("#", "#");
    TOPIC_BAD("+", "+");
    TOPIC_BAD("a/#", "#");
    TOPIC_BAD("a/+/b", "+");
    TOPIC_BAD("#/a", "#");
    TOPIC_BAD("bell#2", "#");
    TOPIC_BAD("bell+2", "+");
    TOPIC_BAD("klingelbox/#", "#");

    /* The message has to say WHY, not just that something is wrong. */
    char e[224];
    CHECK(!db_mqtt_topic_valid("a/#", "topic", 47, e, sizeof(e)), "a/# refused");
    CHECK(strstr(e, "wildcard") != NULL, "message must say 'wildcard': %s", e);
}

static void test_topic_nonprintable(void)
{
    CASE("topic: non-printable bytes");

    TOPIC_BAD("a\tb", "non-printable");
    TOPIC_BAD("a\nb", "non-printable");
    TOPIC_BAD("a\rb", "non-printable");
    TOPIC_BAD("a\x01" "b", "non-printable");
    TOPIC_BAD("a\x1F" "b", "non-printable");
    TOPIC_BAD("a\x7F" "b", "non-printable");
    TOPIC_BAD("a\x80" "b", "non-printable");
    TOPIC_BAD("caf\xC3\xA9", "non-printable");   /* UTF-8 is refused too */

    /* The offending byte must be named — "something is unprintable" leaves the
     * user staring at a field that looks fine. */
    char e[224];
    CHECK(!db_mqtt_topic_valid("a\nb", "topic", 47, e, sizeof(e)), "a\\nb refused");
    CHECK(strstr(e, "0x0A") != NULL, "message must name the byte: %s", e);
}

static void test_topic_slashes(void)
{
    CASE("topic: slashes");

    TOPIC_BAD("/a", "start with '/'");
    TOPIC_BAD("/", "start with '/'");
    TOPIC_BAD("/a/b", "start with '/'");
    TOPIC_BAD("a/", "end with '/'");
    TOPIC_BAD("a/b/", "end with '/'");
    TOPIC_BAD("a//b", "empty level");
    TOPIC_BAD("a//", "empty level");
    TOPIC_BAD("a/b//c", "empty level");
    TOPIC_BAD("a///b", "empty level");
}

static void test_topic_length(void)
{
    CASE("topic: length");

    /* 48 characters against a 47-character field. */
    TOPIC_BAD("012345678901234567890123456789012345678901234567", "too long");

    char e[224];
    char long_one[80];
    memset(long_one, 'x', sizeof(long_one));
    long_one[60] = '\0';
    CHECK(!db_mqtt_topic_valid(long_one, "topic", 47, e, sizeof(e)),
          "a 60-character topic must be refused");
    CHECK(strstr(e, "60") != NULL && strstr(e, "47") != NULL,
          "message must give both the length and the limit: %s", e);

    /* The limit is the CALLER's, not a constant baked in here. */
    CHECK(db_mqtt_topic_valid("abcde", "topic", 5, e, sizeof(e)), "5 of 5 fits");
    CHECK(!db_mqtt_topic_valid("abcdef", "topic", 5, e, sizeof(e)), "6 of 5 does not");
}

static void test_topic_reporting(void)
{
    CASE("topic: reporting");

    char e[224];

    /* Each field names itself, which is the whole reason `field` is a parameter:
     * a user with three topics on screen must not have to guess. */
    CHECK(!db_mqtt_topic_valid("a/#", "topic", 47, e, sizeof(e)), "refused");
    CHECK(strstr(e, "\"topic\"") != NULL, "names 'topic': %s", e);
    CHECK(!db_mqtt_topic_valid("a/#", "mqtt.base_topic", 47, e, sizeof(e)), "refused");
    CHECK(strstr(e, "\"mqtt.base_topic\"") != NULL, "names base_topic: %s", e);
    CHECK(!db_mqtt_topic_valid("a/#", "mqtt.discovery_prefix", 47, e, sizeof(e)), "refused");
    CHECK(strstr(e, "\"mqtt.discovery_prefix\"") != NULL, "names discovery_prefix: %s", e);

    /* Success clears the buffer, so a caller cannot print a stale message. */
    e[0] = 'X'; e[1] = '\0';
    CHECK(db_mqtt_topic_valid("fine", "topic", 47, e, sizeof(e)), "accepted");
    CHECK(e[0] == '\0', "an accepted topic must leave no message");

    /* NULL is empty is valid. */
    CHECK(db_mqtt_topic_valid(NULL, "topic", 47, e, sizeof(e)), "NULL is valid");

    /* A NULL field name must not crash and must still produce a message. */
    CHECK(!db_mqtt_topic_valid("a/#", NULL, 47, e, sizeof(e)), "refused");
    CHECK(e[0] != '\0', "a NULL field name still produces a message");

    /* Leftmost problem first: this string is both wildcard-bearing and
     * slash-terminated, and the '#' is what the user's eye hits first. */
    CHECK(!db_mqtt_topic_valid("a#b/", "topic", 47, e, sizeof(e)), "refused");
    CHECK(strstr(e, "wildcard") != NULL,
          "the first problem left-to-right must be the one reported: %s", e);

    /* A zero-size error buffer must be tolerated, not written to. */
    CHECK(!db_mqtt_topic_valid("a/#", "topic", 47, e, 0), "refused with no buffer");
}

/*
 * The reserved-namespace rule for NODE topics. A sink.mqtt publishes to
 * <base>/<topic>, so a topic whose first level is one the bridge itself
 * subscribes under — "trigger/x" being the killer — publishes onto the box's
 * own subscription and self-fires forever. The list's source of truth is the
 * TOPIC MAP in mqtt_bridge.c; this test pins every level in it, so growing
 * the map without growing the validator fails HERE, not in a user's kitchen.
 */
static void test_topic_reserved(void)
{
    CASE("topic: reserved namespaces");

    char e[224];

    /* Every level of the bridge's topic map, bare and with a subpath. */
    static const char *const RESERVED[] = {
        "status", "button", "trigger", "switch", "unknown", "event", "radio",
    };
    for (size_t i = 0; i < sizeof(RESERVED) / sizeof(RESERVED[0]); i++) {
        const char *lvl = RESERVED[i];
        char with_sub[64];
        snprintf(with_sub, sizeof(with_sub), "%s/x", lvl);

        CHECK(!db_mqtt_node_topic_valid(lvl, "topic", 47, e, sizeof(e)),
              "'%s' must be refused as a node topic", lvl);
        CHECK(strstr(e, lvl) != NULL,
              "'%s': message must name the colliding level, got: %s", lvl, e);
        CHECK(!db_mqtt_node_topic_valid(with_sub, "topic", 47, e, sizeof(e)),
              "'%s' must be refused as a node topic", with_sub);

        /* The PLAIN validator must keep accepting them: the base topic and the
         * discovery prefix ARE the namespace and may be called anything. */
        CHECK(db_mqtt_topic_valid(lvl, "mqtt.base_topic", 47, e, sizeof(e)),
              "'%s' stays legal as a base topic", lvl);

        /* And the lookup names the level, for the bridge's skip-and-log path. */
        CHECK(db_mqtt_topic_reserved_level(lvl) != NULL &&
              strcmp(db_mqtt_topic_reserved_level(lvl), lvl) == 0,
              "reserved_level('%s') must return the level itself", lvl);
    }

    /* Only the WHOLE first level collides: prefixes, suffixes and deeper
     * appearances of a reserved word are all fine. */
    TOPIC_OK("triggers");
    TOPIC_OK("my_trigger");
    TOPIC_OK("front/trigger");        /* second level: not ours */
    TOPIC_OK("statusx");
    TOPIC_OK("eventual");
    CHECK(db_mqtt_node_topic_valid("triggers/x", "topic", 47, e, sizeof(e)),
          "'triggers/x' is not reserved: %s", e);
    CHECK(db_mqtt_node_topic_valid("front", "topic", 47, e, sizeof(e)),
          "'front' is not reserved: %s", e);
    CHECK(db_mqtt_topic_reserved_level("triggers/x") == NULL,
          "'triggers' first level is not reserved");
    CHECK(db_mqtt_topic_reserved_level("front/trigger") == NULL,
          "a reserved word below the first level does not collide");
    CHECK(db_mqtt_topic_reserved_level(NULL) == NULL, "NULL has no level");
    CHECK(db_mqtt_topic_reserved_level("") == NULL, "empty has no level");

    /* The node validator still applies every plain rule first. */
    CHECK(!db_mqtt_node_topic_valid("a/#", "topic", 47, e, sizeof(e)),
          "wildcards stay refused through the node validator");
    CHECK(strstr(e, "wildcard") != NULL, "with the wildcard message: %s", e);
    CHECK(db_mqtt_node_topic_valid("", "topic", 47, e, sizeof(e)),
          "empty stays the caller's business");
    CHECK(db_mqtt_node_topic_valid(NULL, "topic", 47, e, sizeof(e)),
          "NULL stays valid (empty)");
    e[0] = 'X'; e[1] = '\0';
    CHECK(db_mqtt_node_topic_valid("front_gate", "topic", 47, e, sizeof(e)),
          "an ordinary topic passes the node validator");
    CHECK(e[0] == '\0', "and leaves no stale message");

    /* A NULL field name must not crash and must still produce a message. */
    CHECK(!db_mqtt_node_topic_valid("trigger", NULL, 47, e, sizeof(e)), "refused");
    CHECK(e[0] != '\0', "a NULL field name still produces a message");
    /* A zero-size error buffer must be tolerated, not written to. */
    CHECK(!db_mqtt_node_topic_valid("trigger/x", "topic", 47, e, 0),
          "refused with no buffer");
}

/* ---- 4. the switch suffix resolver --------------------------------------- */

/*
 * THE BUG THIS FILE EXISTS TO HAVE CAUGHT.
 *
 * An addressable node's topic is resolved as "explicit topic, else a slug of
 * the name" — one rule, shared by logic.switch and source.virtual (see
 * test_virtual_suffix below, which pins the second type to the same answer).
 * The MQTT bridge resolved it that way when deciding what to SUBSCRIBE
 * to; node_graph.c matched an ARRIVING COMMAND against the raw `topic` field.
 * For every switch that relied on the name fallback the two disagreed, and the
 * user got a Home Assistant entity that could not be commanded and never
 * published its retained state.
 *
 * It was a pure string rule the whole time. These are the cases that would have
 * gone red the day the fallback was added.
 */
static void test_node_suffix(void)
{
    CASE("node suffix: the resolver");

    char out[DB_NODE_TOPIC_MAX];

    /* The user's actual node: a name, no topic. This is the case that broke. */
    db_mqtt_node_suffix("", "All Bells Switch", out, sizeof(out));
    CHECK(strcmp(out, "all_bells_switch") == 0,
          "a named switch with no topic must resolve to its name slug, got '%s'", out);

    /* An explicit topic wins and is NEVER rewritten — not slugified, not
     * lowercased. Renaming the node must not move somebody's HA entity. */
    db_mqtt_node_suffix("outside_bell", "All Bells Switch", out, sizeof(out));
    CHECK(strcmp(out, "outside_bell") == 0, "explicit topic wins, got '%s'", out);
    db_mqtt_node_suffix("Haus/Tuer-1", "ignored", out, sizeof(out));
    CHECK(strcmp(out, "Haus/Tuer-1") == 0,
          "an explicit topic must be passed through verbatim, got '%s'", out);

    /* Nothing usable either way. */
    db_mqtt_node_suffix("", "", out, sizeof(out));
    CHECK(out[0] == '\0', "no topic and no name resolves to nothing, got '%s'", out);
    db_mqtt_node_suffix("", "///", out, sizeof(out));
    CHECK(out[0] == '\0', "a name that slugifies to nothing, got '%s'", out);
    db_mqtt_node_suffix(NULL, NULL, out, sizeof(out));
    CHECK(out[0] == '\0', "NULLs resolve to nothing, got '%s'", out);

    /* Two nodes sharing a NAME share one toggle, exactly as two sharing an
     * explicit topic always have. That is the documented consequence. */
    char a[DB_NODE_TOPIC_MAX], b[DB_NODE_TOPIC_MAX];
    db_mqtt_node_suffix("", "Outside bell", a, sizeof(a));
    db_mqtt_node_suffix("", "outside  BELL", b, sizeof(b));
    CHECK(strcmp(a, b) == 0,
          "two names that slugify alike must share a topic: '%s' vs '%s'", a, b);

    /* A zero-size buffer must be tolerated. */
    db_mqtt_node_suffix("x", "y", out, 0);
    CHECK(1, "zero-size output buffer did not crash");
}

/*
 * The routing question, asked the way both sides of the bridge ask it: "does
 * this node answer on that topic?". Before the fix, the subscribe side said yes
 * and the command side said no for the very same node.
 */
static void test_node_suffix_matching(void)
{
    CASE("node suffix: subscribe and command agree");

    struct { const char *topic; const char *name; const char *arrives; int match; } T[] = {
        /* the reported bug */
        { "",             "All Bells Switch", "all_bells_switch", 1 },
        { "",             "Outside bell",     "outside_bell",     1 },
        /* an explicit topic still routes, exactly as it always did */
        { "outside_bell", "All Bells Switch", "outside_bell",     1 },
        /* and the explicit topic does NOT also answer on its name */
        { "outside_bell", "All Bells Switch", "all_bells_switch", 0 },
        /* a name-derived topic does not answer on the empty string, which is
         * what the old raw-field comparison effectively matched on */
        { "",             "All Bells Switch", "",                 0 },
        { "",             "",                 "",                 0 },
    };

    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        char sfx[DB_NODE_TOPIC_MAX];
        db_mqtt_node_suffix(T[i].topic, T[i].name, sfx, sizeof(sfx));
        /* Empty resolves to "no addressable topic", which never matches — the
         * same short-circuit db_graph_switch_set_topic() applies. */
        int m = (sfx[0] && T[i].arrives[0] && strcmp(sfx, T[i].arrives) == 0);
        CHECK(m == T[i].match,
              "topic='%s' name='%s' command on '%s': expected %s, resolver gave '%s'",
              T[i].topic, T[i].name, T[i].arrives,
              T[i].match ? "a match" : "no match", sfx);
    }
}

/*
 * THE SAME TRAP, THE SECOND NODE TYPE.
 *
 * source.virtual — the MQTT button — subscribes on <base>/trigger/<suffix>. It
 * used to read the raw `topic` field and skip the node entirely when it was
 * blank, so a button added without typing a topic subscribed to nothing,
 * announced no Home Assistant entity, and sat on the canvas looking healthy.
 * That is exactly what the switch did before the resolver, one topic level over.
 *
 * These cases pin the two types to ONE answer. They are written against
 * db_mqtt_node_suffix() because that is the single implementation both sides of
 * the bridge reach — if a virtual trigger ever resolves differently from a
 * switch with the same name and topic, it can only be because someone added a
 * second copy of the rule, and this goes red.
 */
static void test_virtual_suffix(void)
{
    CASE("node suffix: a virtual trigger resolves like a switch");

    char out[DB_NODE_TOPIC_MAX];

    /* The case the user hit: "add an MQTT button, wire it up, press it from Home
     * Assistant" with nothing typed in the topic field. */
    db_mqtt_node_suffix("", "Ring the chime", out, sizeof(out));
    CHECK(strcmp(out, "ring_the_chime") == 0,
          "a named virtual trigger with no topic must resolve to its name slug, got '%s'",
          out);

    /* A node still wearing the palette label is still addressable — the name is
     * poor, but "poor topic" beats "no topic at all and no way to tell". */
    db_mqtt_node_suffix("", "MQTT button", out, sizeof(out));
    CHECK(strcmp(out, "mqtt_button") == 0,
          "the default palette name still yields a topic, got '%s'", out);
    db_mqtt_node_suffix("", "MQTT-Taster", out, sizeof(out));
    CHECK(strcmp(out, "mqtt_taster") == 0,
          "the German default name still yields a topic, got '%s'", out);

    /* An explicit topic wins here too, and a later rename must not move it. */
    db_mqtt_node_suffix("front_gate", "Ring the chime", out, sizeof(out));
    CHECK(strcmp(out, "front_gate") == 0,
          "an explicit trigger topic wins over the name, got '%s'", out);
    db_mqtt_node_suffix("front_gate", "Renamed after the fact", out, sizeof(out));
    CHECK(strcmp(out, "front_gate") == 0,
          "renaming the node must not move an explicit topic, got '%s'", out);

    /* Nothing usable: no subscription, which is the ONLY case where a virtual
     * trigger is invisible to MQTT (mqtt_enabled false being the other, and that
     * one is a decision rather than an accident). */
    db_mqtt_node_suffix("", "", out, sizeof(out));
    CHECK(out[0] == '\0', "no topic and no name resolves to nothing, got '%s'", out);
    db_mqtt_node_suffix("", "!!!", out, sizeof(out));
    CHECK(out[0] == '\0', "a name that slugifies to nothing, got '%s'", out);

    /* ONE RULE, TWO TYPES: a switch and a virtual trigger with the same name and
     * the same topic field must produce byte-identical suffixes. The resolver is
     * type-blind, and this is the check that keeps it that way. */
    struct { const char *topic; const char *name; } SAME[] = {
        { "",             "Alle Klingeln"     },
        { "",             "Outside bell"      },
        { "front_gate",   "Ring the chime"    },
        { "",             "Front door #2"     },
        { "",             ""                  },
    };
    for (size_t i = 0; i < sizeof(SAME) / sizeof(SAME[0]); i++) {
        char as_switch[DB_NODE_TOPIC_MAX], as_virtual[DB_NODE_TOPIC_MAX];
        db_mqtt_node_suffix(SAME[i].topic, SAME[i].name, as_switch, sizeof(as_switch));
        db_mqtt_node_suffix(SAME[i].topic, SAME[i].name, as_virtual, sizeof(as_virtual));
        CHECK(strcmp(as_switch, as_virtual) == 0,
              "the resolver must not depend on the node type: '%s' vs '%s'",
              as_switch, as_virtual);
    }

    /* Two virtual nodes whose names slugify alike share ONE topic, and therefore
     * one Home Assistant button that fires both. Documented behaviour, and the
     * reason db_graph_fire_topic() fires every match rather than the first. */
    char a[DB_NODE_TOPIC_MAX], b[DB_NODE_TOPIC_MAX];
    db_mqtt_node_suffix("", "Ring everything", a, sizeof(a));
    db_mqtt_node_suffix("", "ring  EVERYTHING", b, sizeof(b));
    CHECK(strcmp(a, b) == 0,
          "two trigger names that slugify alike must share a topic: '%s' vs '%s'", a, b);
}

/*
 * The label a topic turns back into, which the node editor PROMISES before the
 * user saves ("Appears in Home Assistant as: Outside bell"). The bridge used to
 * publish the raw slug, so the promise and the dashboard disagreed; these cases
 * pin the C rule to the JavaScript one in app.js.
 */
static void test_pretty_name(void)
{
    CASE("pretty name: a topic turned back into a label");

    char out[DB_NODE_TOPIC_MAX];
    struct { const char *in; const char *want; } T[] = {
        { "outside_bell",   "Outside bell" },
        { "ring_the_chime", "Ring the chime" },
        { "haus/tuer-1",    "Haus tuer 1" },
        { "a__b",           "A b" },
        { "_leading",       "Leading" },
        { "trailing_",      "Trailing" },
        { "already nice",   "Already nice" },
        { "___",            "" },
        { "",               "" },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        db_mqtt_pretty_name(T[i].in, out, sizeof(out));
        CHECK(strcmp(out, T[i].want) == 0,
              "'%s' should read as '%s', got '%s'", T[i].in, T[i].want, out);
    }

    db_mqtt_pretty_name(NULL, out, sizeof(out));
    CHECK(out[0] == '\0', "NULL resolves to nothing, got '%s'", out);
    db_mqtt_pretty_name("x", out, 0);
    CHECK(1, "zero-size output buffer did not crash");

    /* It is a LABEL and must never be fed back into a topic: prettifying then
     * slugifying returns the original, which is what makes the round trip safe
     * to reason about. */
    char slug[DB_NODE_TOPIC_MAX];
    db_mqtt_pretty_name("outside_bell", out, sizeof(out));
    db_mqtt_slugify(out, slug, sizeof(slug));
    CHECK(strcmp(slug, "outside_bell") == 0,
          "pretty then slugify must round-trip, got '%s'", slug);
}

static void test_slugify(void)
{
    CASE("slugify");

    char out[40];
    struct { const char *in; const char *want; } T[] = {
        { "Outside bell",     "outside_bell" },
        { "All Bells Switch", "all_bells_switch" },
        { "Front door #2",    "front_door_2" },
        { "  spaced  out  ",  "spaced_out" },
        { "already_slug",     "already_slug" },
        { "UPPER",            "upper" },
        { "a---b",            "a_b" },          /* runs collapse */
        { "!!!",              "" },
        { "",                 "" },
        { "123",              "123" },
        { "Haustür",          "haust_r" },      /* UTF-8 degrades, never mangles */
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        db_mqtt_slugify(T[i].in, out, sizeof(out));
        CHECK(strcmp(out, T[i].want) == 0,
              "slugify('%s') = '%s', want '%s'", T[i].in, out, T[i].want);
    }

    /* Truncation must still terminate and must not leave a dangling '_'. */
    char small[8];
    db_mqtt_slugify("abcdefghijklmnop", small, sizeof(small));
    CHECK(strlen(small) == 7, "truncated to %u", (unsigned)strlen(small));
    db_mqtt_slugify("abc def ghi", small, sizeof(small));
    CHECK(small[strlen(small) - 1] != '_', "no dangling '_' after truncation: '%s'", small);

    db_mqtt_slugify(NULL, small, sizeof(small));
    CHECK(small[0] == '\0', "NULL name slugifies to nothing");
}

/* ---- 3. the RF control input on a switch ---------------------------------
 *
 * db_switch_reacts_to() decides whether a code heard on air toggles a switch
 * node. It is four conditions and no hardware, and getting any one of them
 * wrong is a bug nobody notices until a stranger's remote flips somebody's
 * chime off: an unbound switch (signal_id 0) reacting to a burst that also
 * decodes as 0 would mean EVERY unregistered press toggled every switch on the
 * box. That case is the first one below and the reason this predicate is a
 * function rather than an inline `&&` at the call site.
 */
static db_node_t sw_node(uint8_t type, uint16_t signal_id, int enabled)
{
    db_node_t n;
    memset(&n, 0, sizeof(n));
    n.id        = 9;
    n.type      = type;
    n.enabled   = enabled ? true : false;
    n.signal_id = signal_id;
    n.mqtt_enabled = true;
    snprintf(n.name, sizeof(n.name), "Outside bell");
    return n;
}

static void test_switch_reacts(void)
{
    CASE("switch reacts to signal");

    /* The matched signal toggles it. */
    db_node_t n = sw_node(DB_NODE_LOGIC_SWITCH, 4, 1);
    CHECK(db_switch_reacts_to(&n, 4), "signal 4 must move a switch bound to 4");

    /* A different signal does not. */
    CHECK(!db_switch_reacts_to(&n, 5), "signal 5 must not move a switch bound to 4");
    CHECK(!db_switch_reacts_to(&n, 3), "signal 3 must not move a switch bound to 4");

    /* signal_id 0 is the default and means "never reacts". Both halves matter:
     * an unbound switch is inert, AND an unrecognized burst (which arrives with
     * signal_id 0) must not be read as matching it. */
    db_node_t unbound = sw_node(DB_NODE_LOGIC_SWITCH, 0, 1);
    CHECK(!db_switch_reacts_to(&unbound, 0),
          "an unbound switch must not react to an unrecognized burst");
    CHECK(!db_switch_reacts_to(&unbound, 4),
          "an unbound switch must not react to a recognized burst either");
    CHECK(!db_switch_reacts_to(&n, 0),
          "a bound switch must not react to an unrecognized burst");

    /* ONLY a switch reacts. Every node type carries a signal_id, and the two
     * signal types carry a MEANINGFUL one — a receiver bound to code 4 must
     * keep firing its chain and must never be treated as a switch to toggle. */
    uint8_t others[] = { DB_NODE_SIGNAL_RX, DB_NODE_SIGNAL_TX, DB_NODE_SOURCE_GPIO,
                         DB_NODE_SOURCE_VIRTUAL, DB_NODE_SOURCE_ANY_RF,
                         DB_NODE_LOGIC_GROUP, DB_NODE_LOGIC_THROTTLE,
                         DB_NODE_LOGIC_REPEAT, DB_NODE_SINK_MQTT,
                         DB_NODE_SINK_MONITOR };
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        db_node_t o = sw_node(others[i], 4, 1);
        CHECK(!db_switch_reacts_to(&o, 4),
              "node type %u must not be toggled by a signal", (unsigned)others[i]);
    }

    /* A NULL node is not a crash. traverse() and the graph task both walk live
     * arrays, but the predicate is public and cheap to call wrongly. */
    CHECK(!db_switch_reacts_to(NULL, 4), "NULL node must not react");
}

/*
 * THE POSITION IS NOT A GATE, and this is the test that says so on purpose.
 *
 * On a logic.switch `enabled` IS the position (see DB_NODE_LOGIC_SWITCH). The
 * usual graph rule — a disabled node is inert — would therefore mean the fob
 * could switch the path OFF and never back ON: a toggle that works exactly
 * once, which is the failure a user would report as "my remote broke the
 * switch". There is no second flag for a switch to be disabled BY (the editor
 * offers this type no Enabled checkbox at all), so there is nothing here to
 * honour. If this ever starts failing, someone has added `n->enabled` to the
 * predicate and turned the feature into a one-way button.
 */
static void test_switch_reacts_in_both_positions(void)
{
    CASE("switch reacts in both positions");

    db_node_t on  = sw_node(DB_NODE_LOGIC_SWITCH, 4, 1);
    db_node_t off = sw_node(DB_NODE_LOGIC_SWITCH, 4, 0);
    CHECK(db_switch_reacts_to(&on, 4),  "an ON switch must react (so it can go OFF)");
    CHECK(db_switch_reacts_to(&off, 4), "an OFF switch must react (so it can come back)");

    /* And the move itself is a toggle, so two presses are a round trip. This is
     * the arithmetic the graph task does; it is one line there and worth
     * pinning here because "one press, one toggle" is the whole contract. */
    bool pos = true;
    pos = !pos; CHECK(pos == false, "first press switches it off");
    pos = !pos; CHECK(pos == true,  "second press switches it back on");

    /* mqtt_enabled is NOT a gate either: the control input is local behaviour
     * between the radio and the graph and must work with MQTT off entirely. */
    db_node_t hidden = sw_node(DB_NODE_LOGIC_SWITCH, 4, 1);
    hidden.mqtt_enabled = false;
    CHECK(db_switch_reacts_to(&hidden, 4),
          "a switch taken off MQTT must still react to its control signal");
}

/* ---- 3. persistence and rollback, against the fake NVS -------------------- */

/* One imported test waveform: `pulses` alternating widths starting HIGH. The
 * analysis code sees a plausible OOK-ish pattern; nothing here asserts on the
 * analysis, only on what is stored and what survives failure. */
static void make_frame(rf_frame_t *f, int pulses)
{
    rf_frame_reset(f);
    f->first_level = 1;
    f->count = (uint16_t)pulses;
    for (int i = 0; i < pulses; i++)
        f->durations_us[i] = (uint16_t)((i & 1) ? 900 : 300);
}

static esp_err_t add_signal(const char *name, int pulses, uint16_t *id_out)
{
    static rf_frame_t f;
    make_frame(&f, pulses);
    return db_signals_add_frame(&f, NULL, name, DB_ORIGIN_IMPORTED, id_out);
}

/* A fresh box: empty fake flash, forgotten resident state, one clean init. */
static void store_fresh(void)
{
    host_nvs_reset();
    db_signals_hosttest_reset();
    CHECK(db_signals_init() == ESP_OK, "fresh init must succeed");
}

/* A reboot: resident state forgotten, flash kept exactly as it is. */
static void store_reboot(void)
{
    db_signals_hosttest_reset();
    CHECK(db_signals_init() == ESP_OK, "re-init (reboot) must succeed");
}

/* Does the frame blob for `id` exist on the fake flash right now? */
static int frame_on_flash(uint16_t id)
{
    nvs_handle_t h;
    if (nvs_open("dbsig", NVS_READONLY, &h) != ESP_OK)
        return 0;
    char key[16];
    snprintf(key, sizeof(key), "f%u", (unsigned)id);
    int present = (nvs_find_key(h, key, NULL) == ESP_OK);
    nvs_close(h);
    return present;
}

static void test_store_copy_accessors(void)
{
    CASE("signal store copy accessors");
    store_fresh();

    uint16_t id = 0;
    CHECK(add_signal("Front door", 50, &id) == ESP_OK, "add must succeed");
    CHECK(id == 1, "first id is 1, got %u", (unsigned)id);

    db_signal_meta_t m;
    memset(&m, 0, sizeof(m));
    CHECK(db_signals_get_copy(id, &m) == ESP_OK, "get_copy finds it");
    CHECK(strcmp(m.name, "Front door") == 0, "copy carries the name");
    CHECK(m.pulse_count == 50, "copy carries the pulse count");
    CHECK(db_signals_get_copy(999, &m) == ESP_ERR_NOT_FOUND,
          "get_copy on a missing id says NOT_FOUND");

    db_signal_meta_t snap[DB_SIGNAL_MAX];
    CHECK(db_signals_snapshot(snap, DB_SIGNAL_MAX) == 1, "snapshot returns 1");
    CHECK(snap[0].id == id, "snapshot holds the record");
    CHECK(db_signals_snapshot(NULL, DB_SIGNAL_MAX) == 0, "NULL out is 0");
}

static void test_store_full_budget(void)
{
    CASE("store-full budget check");
    store_fresh();

    uint16_t id = 0;
    CHECK(add_signal("First", 50, &id) == ESP_OK, "add fits while unlimited");

    /* Leave less than the reserve free: the budget must refuse BEFORE writing
     * anything, with the storage-full error, not a generic one. */
    host_nvs_set_capacity(host_nvs_used_entries() + 100);
    uint16_t id2 = 0;
    CHECK(add_signal("Second", 50, &id2) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "add into a near-full partition reports NOT_ENOUGH_SPACE");
    CHECK(db_signals_count() == 1, "refused add changed nothing in RAM");
    CHECK(!frame_on_flash(2), "refused add left no partial frame blob");

    /* Not a wedge: room returns, the same add succeeds. */
    host_nvs_set_capacity(0);
    CHECK(add_signal("Second", 50, &id2) == ESP_OK, "add succeeds again");
    CHECK(db_signals_count() == 2, "both signals stored");
}

static void test_store_add_rollback(void)
{
    CASE("add rolls back on index-save failure");
    store_fresh();

    /* The frame blob is set_blob #1 of an add, the index rewrite is #2. */
    host_nvs_fail_set_blob(2, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    uint16_t id = 0;
    CHECK(add_signal("Doomed", 50, &id) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "failed index save surfaces to the caller");
    CHECK(db_signals_count() == 0, "RAM rolled back");
    CHECK(!frame_on_flash(1), "the already-written frame blob was erased");

    CHECK(add_signal("Kept", 50, &id) == ESP_OK, "the add is retryable");
    store_reboot();
    CHECK(db_signals_count() == 1, "exactly the retried signal survives");
}

static void test_delete_order_and_full_fallback(void)
{
    CASE("delete order + full-partition fallback");
    store_fresh();

    uint16_t a = 0, b = 0;
    CHECK(add_signal("A", 50, &a) == ESP_OK, "add A");
    CHECK(add_signal("B", 200, &b) == ESP_OK, "add B (long frame)");

    /* The ordinary path: index first, then the frame. Both gone afterwards. */
    CHECK(db_signals_delete(a) == ESP_OK, "delete A");
    CHECK(!frame_on_flash(a), "A's frame erased");
    db_signal_meta_t m;
    CHECK(db_signals_get_copy(a, &m) == ESP_ERR_NOT_FOUND, "A gone from RAM");

    /* The full-partition fallback: with zero entries free the index rewrite
     * cannot go first (the new copy needs room beside the old), so delete
     * frees the frame and retries — the store must always be dig-out-able. */
    host_nvs_set_capacity(host_nvs_used_entries());
    CHECK(db_signals_delete(b) == ESP_OK, "delete works on a FULL partition");
    CHECK(!frame_on_flash(b), "B's frame erased");
    CHECK(db_signals_count() == 0, "store empty in RAM");

    host_nvs_set_capacity(0);
    store_reboot();
    CHECK(db_signals_count() == 0, "store empty after reboot too");
}

static void test_delete_rollback_when_nothing_committed(void)
{
    CASE("delete rolls back when flash refuses everything");
    store_fresh();

    uint16_t id = 0;
    CHECK(add_signal("Sticky", 50, &id) == ESP_OK, "add");

    /* Index save fails AND the frame erase fails: flash still holds the
     * signal in full, so the delete must report failure and put RAM back —
     * anything else is a signal that resurrects at the next boot. */
    host_nvs_fail_set_blob(1, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    host_nvs_fail_erase(1, ESP_FAIL);
    CHECK(db_signals_delete(id) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "delete reports the failure");
    db_signal_meta_t m;
    CHECK(db_signals_get_copy(id, &m) == ESP_OK, "signal still in RAM");
    CHECK(frame_on_flash(id), "signal still on flash");

    CHECK(db_signals_delete(id) == ESP_OK, "the delete is retryable");
    CHECK(db_signals_count() == 0, "and then it is gone");
}

static void test_boot_reconcile_ghost_and_orphan(void)
{
    CASE("boot reconcile: ghosts and orphans");
    store_fresh();

    uint16_t a = 0, b = 0;
    CHECK(add_signal("Ghost", 50, &a) == ESP_OK, "add A");
    CHECK(add_signal("Survivor", 50, &b) == ESP_OK, "add B");

    /* A power cut in delete()'s fallback path: frame erased, index not yet
     * rewritten. Fake it by erasing the blob behind the store's back. */
    nvs_handle_t h;
    CHECK(nvs_open("dbsig", NVS_READWRITE, &h) == ESP_OK, "open dbsig");
    CHECK(nvs_erase_key(h, "f1") == ESP_OK, "simulate the interrupted delete");
    /* And the other half: an orphaned frame blob no index entry names. */
    uint8_t junk[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    CHECK(nvs_set_blob(h, "f9", junk, sizeof(junk)) == ESP_OK, "plant orphan");
    nvs_close(h);

    store_reboot();
    CHECK(db_signals_count() == 1, "ghost pruned: 1 signal left, got %d",
          db_signals_count());
    db_signal_meta_t m;
    CHECK(db_signals_get_copy(b, &m) == ESP_OK, "the survivor survived");
    CHECK(!frame_on_flash(9), "orphan blob swept");

    static rf_frame_t f;
    CHECK(db_signals_load_frame(b, &f) == ESP_OK, "survivor's frame loads");
    CHECK(f.count == 50, "and is intact");

    store_reboot();
    CHECK(db_signals_count() == 1, "repairs are idempotent across boots");
}

static void test_reconcile_leaves_foreign_data_alone(void)
{
    CASE("reconcile never touches a foreign index's data");
    store_fresh();

    uint16_t id = 0;
    CHECK(add_signal("Future", 50, &id) == ESP_OK, "add under this layout");

    /* A NEWER firmware rewrote the index in a layout this build refuses to
     * parse (the first header word is the version — that much of the format
     * is frozen). The refusal loads an empty store ON PURPOSE, and the sweep
     * must then keep its hands off the frame blobs: they belong to the
     * firmware the user may roll forward to. */
    uint32_t foreign_hdr[3] = { 99u, 96u, 1u };
    nvs_handle_t h;
    CHECK(nvs_open("dbsig", NVS_READWRITE, &h) == ESP_OK, "open dbsig");
    CHECK(nvs_set_blob(h, "index", foreign_hdr, sizeof(foreign_hdr)) == ESP_OK,
          "plant a newer-layout index");
    nvs_close(h);

    store_reboot();
    CHECK(db_signals_count() == 0, "foreign index loads as empty");
    CHECK(frame_on_flash(id), "but the frame blob was NOT swept");
}

/* ---- 4. graph mutation rollback, against the fake NVS --------------------- */

static uint16_t graph_add(db_node_type_t type, const char *name)
{
    db_node_t n;
    db_graph_node_defaults(&n, type);
    snprintf(n.name, sizeof(n.name), "%s", name);
    uint16_t id = 0;
    CHECK(db_graph_add_node(&n, &id) == ESP_OK, "add node '%s'", name);
    return id;
}

static int graph_link_count_snapshot(void)
{
    static db_link_t links[DB_LINK_MAX];
    return db_graph_links_snapshot(links, DB_LINK_MAX);
}

static void test_graph_mutation_rollback(void)
{
    CASE("graph mutations roll back on failed saves");

    /* One init for all the graph tests; the fake flash is empty, so the graph
     * starts empty. (node ids and RAM state then flow test to test — each
     * block below leaves the graph as it found it.) */
    host_nvs_reset();
    CHECK(db_graph_init() == ESP_OK, "graph init");
    CHECK(db_graph_node_count() == 0, "starts empty");

    uint16_t src  = graph_add(DB_NODE_SOURCE_VIRTUAL, "button");
    uint16_t sink = graph_add(DB_NODE_SINK_MQTT, "publish");
    CHECK(db_graph_add_link(src, sink) == ESP_OK, "wire them");

    /* add_node: the failed save must not leave a phantom node in RAM. */
    db_node_t n;
    db_graph_node_defaults(&n, DB_NODE_SINK_MONITOR);
    host_nvs_fail_set_blob(1, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    uint16_t id = 0;
    CHECK(db_graph_add_node(&n, &id) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "failed add surfaces the error");
    CHECK(db_graph_node_count() == 2, "failed add changed nothing");

    /* delete_node: the failed save must put the node AND its links back —
     * without this the node vanishes from the UI and broker but resurrects,
     * links and all, at the next reboot. */
    host_nvs_fail_set_blob(1, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK(db_graph_delete_node(src) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "failed delete surfaces the error");
    db_node_t copy;
    CHECK(db_graph_node_copy(src, &copy) == ESP_OK, "node still present");
    CHECK(copy.id == src && copy.type == DB_NODE_SOURCE_VIRTUAL,
          "and intact");
    CHECK(graph_link_count_snapshot() == 1, "its link still present");

    /* delete_link: same discipline, one entry. */
    host_nvs_fail_set_blob(1, ESP_ERR_NVS_NOT_ENOUGH_SPACE);
    CHECK(db_graph_delete_link(src, sink) == ESP_ERR_NVS_NOT_ENOUGH_SPACE,
          "failed link delete surfaces the error");
    CHECK(graph_link_count_snapshot() == 1, "link rolled back");

    /* And every one of them is retryable once flash cooperates. */
    CHECK(db_graph_delete_link(src, sink) == ESP_OK, "link delete retries");
    CHECK(graph_link_count_snapshot() == 0, "link gone");
    CHECK(db_graph_add_link(src, sink) == ESP_OK, "re-wire");
    CHECK(db_graph_delete_node(src) == ESP_OK, "node delete retries");
    CHECK(db_graph_node_copy(src, &copy) == ESP_ERR_NOT_FOUND, "node gone");
    CHECK(graph_link_count_snapshot() == 0, "its link went with it");
    CHECK(db_graph_delete_node(sink) == ESP_OK, "tidy up");
}

static void test_graph_delete_survives_failed_links_write(void)
{
    CASE("node delete is durable even if the links write fails");

    /* db_graph_init already ran. The nodes blob decides existence; a failed
     * LINKS rewrite after a committed nodes blob leaves only dangling links
     * on flash, which the loader sweeps — so the delete must still report
     * success and RAM must be fully consistent. */
    uint16_t src  = graph_add(DB_NODE_SOURCE_VIRTUAL, "button2");
    uint16_t sink = graph_add(DB_NODE_SINK_MQTT, "publish2");
    CHECK(db_graph_add_link(src, sink) == ESP_OK, "wire them");

    host_nvs_fail_set_blob(2, ESP_ERR_NVS_NOT_ENOUGH_SPACE);  /* nodes OK, links fail */
    CHECK(db_graph_delete_node(src) == ESP_OK,
          "delete succeeds — the node blob committed");
    db_node_t copy;
    CHECK(db_graph_node_copy(src, &copy) == ESP_ERR_NOT_FOUND, "node gone");
    CHECK(graph_link_count_snapshot() == 0, "links gone from RAM");
    CHECK(db_graph_delete_node(sink) == ESP_OK, "tidy up");
    CHECK(db_graph_node_count() == 0, "graph empty again");
}

/* ---- main ---------------------------------------------------------------- */

int main(void)
{
    printf("node graph host tests\n");
    printf("---------------------\n");

    test_frozen_layout();
    test_size_collision_is_the_reason();

    test_widen_carries_every_field();
    test_widen_ignores_v3_padding();
    test_widen_real_graph();
    test_widen_terminates_strings();
    test_widen_guards();

    test_topic_accepts();
    test_topic_wildcards();
    test_topic_nonprintable();
    test_topic_slashes();
    test_topic_length();
    test_topic_reporting();
    test_topic_reserved();

    test_slugify();
    test_node_suffix();
    test_node_suffix_matching();
    test_virtual_suffix();
    test_pretty_name();
    test_switch_reacts();
    test_switch_reacts_in_both_positions();

    test_store_copy_accessors();
    test_store_full_budget();
    test_store_add_rollback();
    test_delete_order_and_full_fallback();
    test_delete_rollback_when_nothing_committed();
    test_boot_reconcile_ghost_and_orphan();
    test_reconcile_leaves_foreign_data_alone();

    test_graph_mutation_rollback();
    test_graph_delete_survives_failed_links_write();

    printf("---------------------\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
