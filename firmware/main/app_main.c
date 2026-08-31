/*
 * app_main.c - Klingelbox entry point. "Die Klingel, lokal und ohne Cloud."
 *
 * A local, cloud-free 433 MHz doorbell server: it captures the raw OOK pulse
 * trains of wireless doorbell buttons, decodes them where it can, replays them,
 * synthesizes new signals to pair the user's own chimes to, and routes every
 * press through a user-built node graph to other chimes, MQTT and Home Assistant.
 *
 * Boot order (each step depends on the ones above it):
 *   1. NVS + config              — everything else reads the live config
 *   2. event log                 — so later steps can record what they did
 *   3. signal store + node graph — the domain, loaded from flash
 *   4. RF service                — radio up, capture running
 *   5. dispatch task             — see THE DISPATCH TASK below
 *   6. Wi-Fi (APSTA / recovery) + captive DNS + mDNS
 *   7. HTTP server + web UI
 *   8. MQTT bridge
 *   9. mark the OTA image valid   — LAST, on purpose: only once the box is
 *      actually serving is the running image proven good enough to keep. Doing
 *      this earlier would let a firmware that boots but cannot serve confirm
 *      itself and defeat the rollback.
 *
 * ---------------------------------------------------------------------------
 * THE DISPATCH TASK — the one non-obvious piece of wiring in this file.
 *
 * The RF listener callback runs on the capture task *while that task holds the
 * radio mutex*. Doing real work there would be wrong in two separate ways:
 *
 *   1. DEADLOCK. A node graph traversal can reach the input of a signal node,
 *      which transmits by calling rf_service_transmit(), which takes the same
 *      non-recursive mutex the caller is already holding. The radio would wedge
 *      permanently.
 *   2. LATENCY. Even without a transmit, publishing MQTT or writing NVS while
 *      holding the radio lock stalls capture — precisely when a burst is still
 *      arriving and we most want to be listening.
 *
 * So the listener does nothing but copy the event to a queue. All matching,
 * learn-mode handling, graph traversal and sink work happens on this task, which
 * holds no locks and may block freely.
 * ---------------------------------------------------------------------------
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mdns.h"

#include "board_pins.h"
#include "db_config.h"
#include "db_diag.h"
#include "db_mqtt.h"
#include "dns_server.h"
#include "event_log.h"
#include "http_api.h"
#include "node_graph.h"
#include "ota.h"
#include "rf_service.h"
#include "signal_store.h"
#include "update_check.h"
#include "wifi_mgr.h"

static const char *TAG = "klingelbox";

/* One long-lived config instance. http_api, wifi_mgr and mqtt hold pointers into
 * it and read it live, so it must outlive app_main's stack frame. */
static db_config_t s_cfg;

/* ---------------------------------------------------------------- dispatch */

/* An rf_event_t is ~2 KB; three in flight is ample, since a burst is already
 * coalesced before it reaches us. Overflow is reported rather than silent. */
#define DISPATCH_QUEUE_DEPTH 3
static QueueHandle_t s_dispatch_q;

/* Transmit requests raised by the input side of a signal node. Kept separate
 * from the dispatch queue so that a graph which transmits cannot stall the
 * traversal that produced it. */
typedef struct {
    uint16_t signal_id;
    uint8_t  repeats;
    uint32_t gap_us;
    uint16_t node_id;
} tx_req_t;
static QueueHandle_t s_tx_q;

static void tx_task(void *arg)
{
    (void)arg;
    static rf_frame_t frame;   /* 1 KB — off the task stack */
    tx_req_t req;

    for (;;) {
        if (xQueueReceive(s_tx_q, &req, portMAX_DELAY) != pdTRUE)
            continue;
        if (db_signals_load_frame(req.signal_id, &frame) != ESP_OK) {
            ESP_LOGW(TAG, "transmit: signal %u has no stored frame", req.signal_id);
            db_events_push(DB_EV_TRANSMIT, req.signal_id, req.node_id, 0, 0,
                           "failed: no stored frame");
            continue;
        }
        esp_err_t err = rf_service_transmit(&frame, req.repeats, req.gap_us);
        /* The activity feed is read by users, not by us: db_err_text() rather
         * than the raw constant (see db_diag.h). The constant is in the log. */
        if (err == ESP_OK) {
            db_events_push(DB_EV_TRANSMIT, req.signal_id, req.node_id, 0,
                           req.repeats, "sent");
        } else {
            ESP_LOGW(TAG, "transmit of signal %u failed: %s",
                     req.signal_id, esp_err_to_name(err));
            db_events_push(DB_EV_TRANSMIT, req.signal_id, req.node_id, 0,
                           req.repeats, "not sent — %s", db_err_text(err));
        }
    }
}

/* Transmit handler: called for a signal node reached over an inbound link (its
 * input side), never for one an RF match started. Runs on the dispatch task;
 * still queues rather than transmitting inline, so one slow send cannot delay
 * the rest of a fan-out. */
static void transmit_sink(const db_node_t *node, const db_trigger_t *trig, void *ctx)
{
    (void)trig;
    (void)ctx;
    if (!node->signal_id)
        return;
    tx_req_t req = {
        .signal_id = node->signal_id,
        .repeats   = node->repeats ? node->repeats : s_cfg.tx_repeats,
        .gap_us    = node->gap_us ? node->gap_us : s_cfg.tx_gap_us,
        .node_id   = node->id,
    };
    if (xQueueSend(s_tx_q, &req, 0) != pdTRUE)
        ESP_LOGW(TAG, "transmit queue full; dropped node %u", node->id);
}

/* Build the trigger that travels with a traversal (see node_graph.h). */
static void trigger_from_event(db_trigger_t *trig, const rf_event_t *ev, uint16_t signal_id)
{
    memset(trig, 0, sizeof(*trig));
    trig->signal_id     = signal_id;
    trig->fingerprint   = ev->fingerprint;
    trig->rssi_dbm      = (int16_t)ev->rssi_dbm;
    trig->repeats       = ev->repeats;
    trig->decoded_valid = ev->decoded.valid;
    if (ev->decoded.valid) {
        snprintf(trig->protocol, sizeof(trig->protocol), "%s", ev->decoded.protocol);
        trig->decoded_id     = ev->decoded.id;
        trig->decoded_button = ev->decoded.button;
    }

    const db_signal_meta_t *meta = signal_id ? db_signals_get(signal_id) : NULL;
    /* strlcpy, not snprintf("%s"): the label is a short display name and both
     * sources can legitimately be longer, so truncation is intended. strlcpy
     * says that outright and always NUL-terminates, where snprintf makes the
     * compiler warn about a truncation we actually want. */
    if (meta && meta->name[0])
        strlcpy(trig->label, meta->name, sizeof(trig->label));
    else if (ev->decoded.valid)
        strlcpy(trig->label, ev->decoded.text, sizeof(trig->label));
    else
        snprintf(trig->label, sizeof(trig->label), "unknown %08" PRIx32, ev->fingerprint);
}

static void dispatch_task(void *arg)
{
    (void)arg;
    static rf_event_t ev;      /* ~2 KB — off the task stack */
    db_trigger_t trig;

    for (;;) {
        if (xQueueReceive(s_dispatch_q, &ev, portMAX_DELAY) != pdTRUE)
            continue;

        uint16_t sid = db_signals_match(&ev);
        trigger_from_event(&trig, &ev, sid);

        if (sid) {
            db_events_push(DB_EV_BUTTON_PRESS, sid, 0, ev.rssi_dbm, ev.repeats,
                           "%s", trig.label);
            db_mqtt_on_signal_press(sid, ev.rssi_dbm, ev.repeats);
        } else {
            db_events_push(DB_EV_RF_UNMATCHED, 0, 0, ev.rssi_dbm, ev.repeats,
                           "%s", trig.label);
            /* Cheap and safe when learn mode is not armed. */
            if (db_signals_learn_offer(&ev))
                db_events_push(DB_EV_LEARN, 0, 0, ev.rssi_dbm, ev.repeats,
                               "candidate: %s", trig.label);
        }

        /* Fires the matching signal node's OUTPUT side AND every source.any_rf
         * node — including for unrecognized bursts (sid == 0). */
        db_graph_on_rf(&trig);
    }
}

/* Runs on the capture task with the radio mutex held: copy and get out. */
static void on_rf_event(const rf_event_t *ev, void *ctx)
{
    (void)ctx;
    if (!s_dispatch_q)
        return;
    if (xQueueSend(s_dispatch_q, ev, 0) != pdTRUE)
        ESP_LOGW(TAG, "dispatch queue full; dropped a burst");
}

/* -------------------------------------------------------------------- mdns */

static void mdns_start(const db_config_t *cfg)
{
    const char *host = cfg->hostname[0] ? cfg->hostname : "klingelbox";

    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns init failed");
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set("Klingelbox");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mdns: http://%s.local", host);
}

/* -------------------------------------------------------------------- main */

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "Klingelbox %s (IDF %s)", app->version, app->idf_ver);

    /* 1. config */
    ESP_ERROR_CHECK(db_nvs_init());
    db_config_load(&s_cfg);

    /* 2-3. event log + domain */
    db_events_init();
    db_signals_init();
    db_graph_init();
    db_graph_set_transmit_handler(transmit_sink, NULL);
    db_graph_set_mqtt_handler(db_mqtt_sink, NULL);

    /* 5. dispatch + transmit tasks, created BEFORE the radio so no burst is
     * dropped between capture starting and the consumer existing. */
    s_dispatch_q = xQueueCreate(DISPATCH_QUEUE_DEPTH, sizeof(rf_event_t));
    s_tx_q       = xQueueCreate(4, sizeof(tx_req_t));
    if (!s_dispatch_q || !s_tx_q) {
        ESP_LOGE(TAG, "out of memory creating queues");
        return;
    }
    xTaskCreate(dispatch_task, "db_dispatch", 5120, NULL, 6, NULL);
    xTaskCreate(tx_task, "db_tx", 4096, NULL, 5, NULL);

    /* 4. radio. A missing CC1101 is reported through db_diag and left running,
     * so the web UI can explain the fault instead of the box appearing dead. */
    rf_service_set_listener(on_rf_event, NULL);
    esp_err_t err = rf_service_start();
    if (err != ESP_OK)
        ESP_LOGE(TAG, "rf_service_start: %s", esp_err_to_name(err));

    /* Wired GPIO inputs, if the user configured any source.gpio nodes. With none
     * configured this touches no hardware at all. */
    db_graph_apply_gpio_inputs();

    /* 6. network */
    ESP_ERROR_CHECK(db_wifi_start(&s_cfg));
    db_dns_start();
    mdns_start(&s_cfg);

    /* 7-8. services. db_update_init() only records the running version and
     * creates its mutex — it touches no network, and the first GitHub fetch
     * happens when the UI or a REST caller asks for it. */
    db_update_init();
    db_http_start(&s_cfg);
    db_mqtt_start(&s_cfg);

    /* 9. only now is this image proven good. */
    db_ota_mark_valid();

    db_events_push(DB_EV_SYSTEM, 0, 0, 0, 0, "Klingelbox %s up", app->version);

    if (db_wifi_mode() == DB_WIFI_RECOVERY) {
        char ssid[33] = {0};
        db_wifi_ap_ssid(ssid);
        ESP_LOGW(TAG, "RECOVERY: join '%s' and open http://192.168.66.1", ssid);
    } else {
        ESP_LOGI(TAG, "up: http://%s.local", s_cfg.hostname);
    }

    /* Heartbeat. On a quiet band the noise floor is the single most useful
     * number for judging whether the antenna and AGC are sane. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        int rssi = 0;
        if (rf_service_rssi(&rssi) == ESP_OK)
            ESP_LOGI(TAG, "idle: noise floor %d dBm, heap %" PRIu32 ", signals %d",
                     rssi, esp_get_free_heap_size(), db_signals_count());
    }
}
