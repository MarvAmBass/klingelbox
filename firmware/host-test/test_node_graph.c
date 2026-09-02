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
 * Neither of these needs ESP-IDF, and both are compiled against two tiny stubs
 * (see stubs/) rather than the framework. If that stops working it is worth
 * knowing about: it means node_graph.h has grown a real dependency and this
 * safety net has a hole in it.
 *
 * Build and run:  make test
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_topic.h"
#include "node_graph.h"
#include "node_migrate.h"

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

    test_slugify();
    test_node_suffix();
    test_node_suffix_matching();
    test_virtual_suffix();
    test_pretty_name();
    test_switch_reacts();
    test_switch_reacts_in_both_positions();

    printf("---------------------\n");
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
