/* mqtt_topic.c - see mqtt_topic.h for the rules and the reasoning behind them. */
#include "mqtt_topic.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Report the FIRST problem found, scanning left to right, so the character the
 * message names is the one the user's eye lands on first. Reporting the worst
 * problem instead would mean a user fixing '#' only to be told about the '/'
 * that was already there — two round trips for one paste. */
bool db_mqtt_topic_valid(const char *topic, const char *field, size_t max_len,
                         char *err, size_t errsz)
{
    if (errsz) err[0] = '\0';
    if (!field) field = "topic";
    if (!topic) topic = "";

    size_t n = strlen(topic);

    /* Empty is the caller's business, not a syntax error. */
    if (n == 0)
        return true;

    if (n > max_len) {
        if (errsz)
            snprintf(err, errsz,
                     "\"%s\" is too long: %u characters, the limit is %u.",
                     field, (unsigned)n, (unsigned)max_len);
        return false;
    }

    if (topic[0] == '/') {
        if (errsz)
            snprintf(err, errsz,
                     "\"%s\" must not start with '/' — a leading slash makes an "
                     "empty first topic level. The box adds the separators itself.",
                     field);
        return false;
    }

    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)topic[i];

        if (c == '#' || c == '+') {
            if (errsz)
                snprintf(err, errsz,
                         "\"%s\" contains '%c', which is an MQTT wildcard. A "
                         "message cannot be published to a topic containing "
                         "'#' or '+' — the broker refuses it.",
                         field, (char)c);
            return false;
        }

        if (c < 0x20 || c >= 0x7F) {
            if (errsz)
                snprintf(err, errsz,
                         "\"%s\" contains a non-printable character (byte 0x%02X) "
                         "at position %u. Only plain printable ASCII is allowed.",
                         field, (unsigned)c, (unsigned)(i + 1));
            return false;
        }

        /* An empty level. The trailing case is reported separately because
         * "ends with '/'" is a clearer thing to be told than "level 3 is empty". */
        if (c == '/' && i + 1 < n && topic[i + 1] == '/') {
            if (errsz)
                snprintf(err, errsz,
                         "\"%s\" contains an empty level ('//') at position %u. "
                         "That is legal MQTT but almost always a typo, so it is "
                         "refused rather than producing a topic nobody can read.",
                         field, (unsigned)(i + 1));
            return false;
        }
    }

    if (topic[n - 1] == '/') {
        if (errsz)
            snprintf(err, errsz,
                     "\"%s\" must not end with '/' — a trailing slash makes an "
                     "empty last topic level.",
                     field);
        return false;
    }

    return true;
}

/*
 * First topic levels the bridge owns under <base>/. SOURCE OF TRUTH: the
 * "TOPIC MAP" comment at the top of mqtt_bridge.c — this list is every first
 * level that map subscribes to or publishes on, nothing more and nothing less.
 * (status, event and radio have no '/' in the map; they are still first levels
 * and still collide, because "status/loud" would shadow nothing today but the
 * bare "status" topic itself is one of ours.)
 */
static const char *const RESERVED_LEVELS[] = {
    "status",    /* <base>/status — availability + LWT                       */
    "button",    /* <base>/button/<slug>/state|press                         */
    "trigger",   /* <base>/trigger/<suffix> — SUBSCRIBED: the loop starter   */
    "switch",    /* <base>/switch/<suffix>/set|state — /set is SUBSCRIBED    */
    "unknown",   /* <base>/unknown, <base>/unknown/state                     */
    "event",     /* <base>/event — every node firing                         */
    "radio",     /* <base>/radio — retained telemetry                        */
};

const char *db_mqtt_topic_reserved_level(const char *topic)
{
    if (!topic || !topic[0])
        return NULL;
    const char *slash = strchr(topic, '/');
    size_t flen = slash ? (size_t)(slash - topic) : strlen(topic);
    for (size_t i = 0; i < sizeof(RESERVED_LEVELS) / sizeof(RESERVED_LEVELS[0]); i++) {
        if (flen == strlen(RESERVED_LEVELS[i]) &&
            strncmp(topic, RESERVED_LEVELS[i], flen) == 0)
            return RESERVED_LEVELS[i];
    }
    return NULL;
}

bool db_mqtt_node_topic_valid(const char *topic, const char *field,
                              size_t max_len, char *err, size_t errsz)
{
    if (!db_mqtt_topic_valid(topic, field, max_len, err, errsz))
        return false;
    if (!field) field = "topic";

    const char *rsv = db_mqtt_topic_reserved_level(topic);
    if (rsv) {
        if (errsz)
            snprintf(err, errsz,
                     "\"%s\" starts with \"%s\", a topic level the box itself "
                     "publishes and listens under. A node publishing there "
                     "would feed the box its own messages back — at worst a "
                     "chain that fires itself forever. Choose a first level "
                     "of your own.",
                     field, rsv);
        return false;
    }
    return true;
}

/* Lowercase alphanumerics survive; every other byte becomes '_', runs collapse,
 * and leading/trailing separators are trimmed. Multi-byte UTF-8 (an umlaut in a
 * signal name) degrades to a single '_' rather than mangling the topic.
 *
 * Moved here verbatim from mqtt_bridge.c: same characters in, same slug out, so
 * no existing topic or Home Assistant object_id changes. */
void db_mqtt_slugify(const char *name, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    if (!name) { out[0] = '\0'; return; }

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && o + 1 < outsz; p++) {
        if (isalnum(*p)) {
            out[o++] = (char)tolower(*p);
        } else if (o && out[o - 1] != '_') {
            out[o++] = '_';
        }
    }
    /* A name ending in punctuation would otherwise leave a dangling '_'. */
    while (o && out[o - 1] == '_') o--;
    out[o] = '\0';
}

void db_mqtt_node_suffix(const char *topic, const char *name,
                         char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    /* An explicit topic wins and is copied through UNCHANGED — not slugified.
     * The user typed it; rewriting it would move their entity. */
    if (topic && topic[0]) {
        size_t n = strlen(topic);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, topic, n);
        out[n] = '\0';
        return;
    }
    db_mqtt_slugify(name, out, outsz);
}

/* See mqtt_topic.h. Deliberately byte-for-byte the same rule as prettyTopic()
 * in app.js, which is what the node editor shows the user before they save. */
void db_mqtt_pretty_name(const char *suffix, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return;
    out[0] = '\0';
    if (!suffix)
        return;

    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)suffix; *p && o + 1 < outsz; p++) {
        if (*p == '_' || *p == '-' || *p == '/') {
            /* No leading space, and a run of separators is one space. */
            if (o && out[o - 1] != ' ')
                out[o++] = ' ';
        } else {
            out[o++] = (char)*p;
        }
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = '\0';

    if (out[0])
        out[0] = (char)toupper((unsigned char)out[0]);
}
