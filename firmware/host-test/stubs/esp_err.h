/*
 * esp_err.h - host-test stub.
 *
 * Originally just `typedef int esp_err_t;` so node_graph.h could be compiled
 * for the frozen-layout tests. The net has since widened: test_node_graph now
 * links node_graph.c and signal_store.c themselves — the persistence logic
 * whose rollback behaviour destroys user data when it is wrong — against a
 * fake in-RAM NVS (see host_env.c). Those files compare against the real
 * ESP_ERR_* values, so this stub carries them, byte-for-byte the values from
 * esp_common/include/esp_err.h: an error code is part of a module's contract
 * (signal_store.h documents ESP_ERR_NVS_NOT_ENOUGH_SPACE by name), and a stub
 * that renumbered them would test a different contract.
 */
#ifndef DB_HOSTTEST_ESP_ERR_H
#define DB_HOSTTEST_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                 0
#define ESP_FAIL               (-1)
#define ESP_ERR_NO_MEM         0x101
#define ESP_ERR_INVALID_ARG    0x102
#define ESP_ERR_INVALID_STATE  0x103
#define ESP_ERR_INVALID_SIZE   0x104
#define ESP_ERR_NOT_FOUND      0x105
#define ESP_ERR_NOT_SUPPORTED  0x106
#define ESP_ERR_TIMEOUT        0x107

const char *esp_err_to_name(esp_err_t err);   /* host_env.c */

/* The firmware only wraps calls it expects to succeed; in the test binary a
 * failure here is a broken test, so abort loudly. */
#include <stdio.h>
#include <stdlib.h>
#define ESP_ERROR_CHECK(x)                                                    \
    do {                                                                      \
        esp_err_t err_rc_ = (x);                                              \
        if (err_rc_ != ESP_OK) {                                              \
            fprintf(stderr, "ESP_ERROR_CHECK failed (%d) at %s:%d\n",         \
                    err_rc_, __FILE__, __LINE__);                             \
            abort();                                                          \
        }                                                                     \
    } while (0)

#endif
