/*
 * mqtt_topic.h - ONE rule for every MQTT topic a user can type.
 *
 * WHY THIS IS ITS OWN FILE. Three different screens let a user type something
 * that ends up inside a topic the box publishes to: a node's topic suffix, the
 * MQTT base topic, and the Home Assistant discovery prefix. They used to accept
 * anything at all — `{"topic":"a/#"}` was stored without a word — and `#` and
 * `+` are MQTT WILDCARDS: publishing to a topic containing one is illegal, so a
 * broker rejects the message or drops the connection. One typo in the base topic
 * takes the whole bridge down, because the base topic is the prefix of every
 * topic the box publishes.
 *
 * Three near-identical copies of the check would drift — this is exactly the
 * kind of rule that gets fixed in one place and quietly left broken in the other
 * two. So there is one validator and every call site uses it.
 *
 * NO ESP-IDF, ON PURPOSE. Nothing here needs a device: it is a string rule, and
 * host-test/ exercises it with plain gcc in a millisecond. That is also why it
 * does not live in mqtt_bridge.c, which drags in esp-mqtt.
 *
 * WHAT IS REFUSED, AND WHY
 *
 *   '#' and '+'      MQTT wildcards. A subscription may contain them; a
 *                    PUBLISH may not. Every string this guards is published to.
 *   control / high   Anything below 0x20, DEL, or >= 0x80. A topic nobody can
 *                    see is a topic nobody can debug, and a stray newline
 *                    pasted from a config file is the usual way one arrives.
 *   leading '/'      Legal MQTT, and almost always a mistake here: "/foo" is
 *   trailing '/'     an EMPTY first level, and the box already puts the
 *   empty level      separators in. Refused rather than silently producing a
 *                    topic nobody can read.
 *   over-length      The field's own storage limit, checked before the value is
 *                    truncated into it rather than after.
 *
 * WHAT IS ACCEPTED. An EMPTY string is valid here, always. Emptiness means
 * different things to different callers — "this node has no MQTT" for a node
 * suffix, "use the default" for the base topic — and that policy belongs to the
 * caller, not to a syntax check. A '-' is a perfectly legal topic level and
 * stays legal: a sentinel value was considered for "no MQTT" and rejected for
 * precisely that reason, which is why there is a flag on the node instead.
 */
#ifndef DB_MQTT_TOPIC_H
#define DB_MQTT_TOPIC_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * True when `topic` may be published to and subscribed on.
 *
 * `field` names the field in the message ("topic", "mqtt.base_topic", ...), so
 * a user who has three topics on screen can tell WHICH one was rejected without
 * guessing. `max_len` is that field's usable length, EXCLUDING the terminator.
 *
 * On failure a complete sentence naming the offending character is written to
 * `err` (never left empty, and always NUL-terminated when errsz > 0). On success
 * `err` is set to "".
 *
 * A NULL topic is treated as empty, and empty is valid — see the header comment.
 */
bool db_mqtt_topic_valid(const char *topic, const char *field, size_t max_len,
                         char *err, size_t errsz);

/*
 * The stricter rule for a NODE's topic: everything db_mqtt_topic_valid()
 * checks, PLUS the reserved-namespace rule below. Node topics get the extra
 * rule because they are composed UNDER the base topic (`<base>/<topic>` for a
 * sink.mqtt), while the base topic and the discovery prefix ARE the namespace
 * and may legitimately be called anything — which is why this is a second
 * entry point and not a change to db_mqtt_topic_valid() itself.
 */
bool db_mqtt_node_topic_valid(const char *topic, const char *field,
                              size_t max_len, char *err, size_t errsz);

/*
 * If `topic`'s FIRST level is one the bridge itself owns under <base>/, return
 * that level's name; NULL otherwise (including for NULL/empty topics).
 *
 * WHY A FIRST-LEVEL RULE EXISTS AT ALL. A sink.mqtt node publishes to
 * <base>/<topic>. Give it the topic "trigger/x" while a virtual trigger is
 * subscribed on <base>/trigger/x and every publish lands back on the box's own
 * subscription: the sink fires the trigger, the trigger's chain reaches the
 * sink, and the loop runs FOREVER — ringing chimes and keying the RF carrier
 * until someone edits the graph (one message from a malicious broker starts
 * it). MQTT 3.1.1 has no no-local option, so the only reliable place to break
 * the cycle is here, before the topic is ever stored.
 *
 * THE LIST IS THE TOPIC MAP. Its source of truth is the "TOPIC MAP" comment at
 * the top of mqtt_bridge.c — every first level the bridge subscribes to or
 * publishes under <base>/. If the bridge grows a namespace, it must be added
 * here in the same change, or the new namespace ships without the guard.
 *
 * Exposed separately (not only inside db_mqtt_node_topic_valid) so the bridge
 * can SKIP, rather than publish, a reserved topic that an older firmware
 * already stored — the graph keeps working, only the dangerous publish is
 * suppressed.
 */
const char *db_mqtt_topic_reserved_level(const char *topic);

/*
 * Turn a human name into a topic segment: lowercase, alphanumerics kept, every
 * run of anything else collapsed to a single '_', no leading or trailing '_'.
 *
 *     "Outside bell"   -> "outside_bell"
 *     "Front door #2"  -> "front_door_2"
 *     "!!!"            -> ""            (nothing usable; the caller decides)
 *
 * Always NUL-terminates when outsz > 0, and truncates rather than overflowing.
 *
 * THIS IS THE ONLY IMPLEMENTATION. It used to be a static in mqtt_bridge.c,
 * which was fine until a second place needed the same answer — see
 * db_mqtt_node_suffix() below for what a second copy cost.
 */
void db_mqtt_slugify(const char *name, char *out, size_t outsz);

/*
 * THE topic a node answers on, from its explicit topic and its name.
 *
 * TWO NODE TYPES, ONE RULE. A logic.switch is subscribed as
 * <base>/switch/<suffix>/set and a source.virtual as <base>/trigger/<suffix>;
 * the topics differ, the way the SUFFIX is arrived at does not. It was called
 * db_mqtt_switch_suffix() while only the switch used it, and the name is now
 * wrong rather than merely narrow — hence db_mqtt_node_suffix(). There is still
 * exactly one implementation, which is the whole point: the two types cannot
 * drift apart into "blank means a slug of the name here, blank means invisible
 * there", which is precisely the trap each of them fell into in turn.
 *
 * An explicit `topic` always wins and is never rewritten, so once you have typed
 * one, renaming the node cannot move your Home Assistant entity out from under
 * you. Left blank, the NAME is slugified instead, because "MQTT topic
 * (optional)" made the single thing connecting a node to Home Assistant look
 * like a detail you could skip — and skipping it silently meant no HA entity at
 * all, on a node that looked perfectly healthy on the canvas. Both empty (or a
 * name that slugifies to nothing) yields "", meaning the node has no addressable
 * topic.
 *
 * WHY THIS IS A SHARED FUNCTION AND NOT A RULE PEOPLE REIMPLEMENT. It was
 * written twice: the MQTT bridge resolved the suffix this way when deciding what
 * to SUBSCRIBE to, while node_graph.c matched an arriving command against the
 * raw `topic` field. A switch that relied on the name fallback therefore got a
 * Home Assistant entity that could not be commanded and never published its
 * retained state — the entity was real, the wiring behind it was not. One
 * resolver, reached from both sides, is the fix and the guard against it
 * recurring; db_graph_node_suffix() in node_graph.h is the node-shaped door
 * onto it.
 */
void db_mqtt_node_suffix(const char *topic, const char *name,
                         char *out, size_t outsz);

/*
 * A topic suffix turned back into something worth reading:
 *
 *     "outside_bell"   -> "Outside bell"
 *     "haus/tuer-1"    -> "Haus tuer 1"
 *     ""               -> ""
 *
 * Separator runs ('_', '-', '/') collapse to one space, the ends are trimmed,
 * and the first character is capitalised. Nothing else is touched — this is a
 * LABEL, not a topic, so it is never fed back into a topic.
 *
 * WHY IT EXISTS. A node still wearing its palette name is named after its topic
 * in Home Assistant, and the editor tells the user exactly what that name will
 * be — using this rule, in JavaScript (prettyTopic() in app.js). The bridge
 * published the raw slug instead, so the editor promised "Outside bell" and the
 * dashboard showed "outside_bell". Same rule, both sides, one place to read it.
 */
void db_mqtt_pretty_name(const char *suffix, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* DB_MQTT_TOPIC_H */
