/*
 * esp_err.h - host-test stub.
 *
 * node_graph.h declares esp_err_t returns, and node_migrate.c has to include it
 * to see db_node_t. Nothing in the migration path CALLS anything from ESP-IDF —
 * it is struct copying — so the type alone is enough to compile it on a host.
 *
 * If this stub ever stops being enough, that is the signal to look at: it means
 * node_graph.h has grown a real dependency on the framework, and the frozen
 * layouts can no longer be tested without a device.
 */
#ifndef DB_HOSTTEST_ESP_ERR_H
#define DB_HOSTTEST_ESP_ERR_H

typedef int esp_err_t;

#endif
