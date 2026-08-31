/*
 * db_mqtt.h - MQTT bridge + Home Assistant discovery (see mqtt_bridge.c).
 *
 * WHY THE ODD FILE NAME. This header is db_mqtt.h and not the obvious
 * mqtt_bridge.h/mqtt_client.h because esp-mqtt ships its own <mqtt_client.h>,
 * which mqtt_bridge.c must include. A same-named header in main/ (which is on
 * the include path with priority) silently shadows it and the component fails to
 * compile in a way that points nowhere near the real cause. The reference
 * firmware hit exactly this; the name is deliberate, do not "fix" it.
 *
 * WHAT THIS EXPOSES, AND WHY IT LOOKS LIKE THIS
 *
 * A doorbell press is an EVENT, not a state. That single observation shapes the
 * whole MQTT surface:
 *
 *   - press state topics are published UNRETAINED: a retained press would ring
 *     every HA restart, and "the last press was 6 hours ago" is not a state
 *     anybody wants replayed on subscribe;
 *   - discovery advertises each button as a Home Assistant *device trigger*
 *     (`device_automation`), not a binary_sensor. A binary_sensor would need an
 *     artificial off-again reset and shows up in HA as a stuck "on" whenever the
 *     reset is lost; a device trigger is the entity type HA actually built for
 *     momentary events, and it appears natively in the automation editor's
 *     "When ... is pressed" picker;
 *   - availability is a retained `<base>/status` topic that doubles as the MQTT
 *     Last Will, so a box that loses power shows in HA as *unavailable* rather
 *     than as a doorbell that simply never rings again.
 *
 * The bridge is also an INPUT, in two ways. Every stored signal gets a `press`
 * command topic that transmits it, and every enabled SOURCE_VIRTUAL node with a
 * topic suffix gets a `<base>/trigger/<suffix>` topic that fires it. That is what
 * lets an HA automation ring a chime, and what lets anything at all — Node-RED, a
 * shell one-liner, another ESP — start a node-graph chain with no RF involved.
 * The box is a two-way gateway, not a sensor.
 *
 * THREADING CONTRACT. Every entry point below is safe to call from any task and
 * none of them block on the radio or the network: they hand a small message to
 * the bridge's own task through a queue. In particular the esp-mqtt event task
 * never transmits — a transmit takes the radio mutex and keys the carrier for
 * hundreds of milliseconds, which inside the event handler would stall the
 * client's keepalive and drop the connection it arrived on.
 */
#ifndef DB_MQTT_H
#define DB_MQTT_H

#include <stdbool.h>
#include <stdint.h>

#include "db_config.h"
#include "event_log.h"
#include "node_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the bridge if cfg->mqtt_enabled; a no-op otherwise (so app_main can call
 * it unconditionally). Keeps the POINTER to the live config — the caller must
 * keep *cfg alive for the lifetime of the bridge, exactly like wifi_mgr does.
 * Safe to call before the network is up: esp-mqtt reconnects on its own.
 */
void db_mqtt_start(db_config_t *cfg);

/* Publish a final retained "offline", stop the client and shut the task down.
 * Blocks until the worker has left any in-flight transmit. */
void db_mqtt_stop(void);

/* True while the broker connection is up (for GET /api/system). */
bool db_mqtt_connected(void);

/* Wake the publisher so the retained radio state goes out now instead of on the
 * next periodic tick. No-op when MQTT is disabled or not started yet. */
void db_mqtt_notify_publish(void);

/* A stored signal was recognized. Publishes the unretained press payload on
 * <base>/button/<slug>/state, which is also what fires the HA device trigger.
 * Called from the RF event path — returns immediately. */
void db_mqtt_on_signal_press(uint16_t signal_id, int rssi_dbm, uint8_t repeats);

/* A signal was added, renamed or deleted: re-resolve the slugs and re-publish
 * discovery, clearing the retained configs of anything that went away so HA
 * removes the entity instead of leaving a dangling one. */
void db_mqtt_on_signals_changed(void);

/* A node or link was added, changed or deleted: re-sync the
 * <base>/trigger/<suffix> subscriptions of the enabled SOURCE_VIRTUAL nodes
 * (subscribing to what is new, unsubscribing from what is gone) and re-publish
 * their HA button discovery, clearing the configs of nodes that vanished. */
void db_mqtt_on_graph_changed(void);

/*
 * Node-graph MQTT sink. Matches db_sink_fn, so app_main registers it with
 * db_graph_set_mqtt_handler(db_mqtt_sink, NULL).
 *
 * Publishes WHAT CAUSED the node to fire — the docs/API.md trigger document — to
 * <base>/event and, when the node carries a topic suffix, to <base>/<suffix>. A
 * trigger with signal_id == 0 is an unregistered burst (the normal case behind a
 * SOURCE_ANY_RF proxy) and is additionally published to the catch-all
 * <base>/unknown topics, which is what makes it visible in Home Assistant
 * despite having no entity of its own.
 */
void db_mqtt_sink(const db_node_t *node, const db_trigger_t *trig, void *ctx);

/* Publish a system/domain event on <base>/event using event_log.h's vocabulary.
 * Used for boot, radio state and learn-mode announcements; ids may be 0 and
 * `text` may be NULL. Safe from any task. */
void db_mqtt_publish_event(db_event_kind_t kind, uint16_t signal_id,
                           uint16_t node_id, const char *text);

#ifdef __cplusplus
}
#endif

#endif /* DB_MQTT_H */
