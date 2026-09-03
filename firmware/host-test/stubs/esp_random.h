/* esp_random.h - host-test stub. Deterministic-enough pseudo-randomness from
 * the C library; the store only uses it to draw EV1527 addresses, and a test
 * asserts on structure, never on which address came out. */
#ifndef DB_HOSTTEST_ESP_RANDOM_H
#define DB_HOSTTEST_ESP_RANDOM_H

#include <stdint.h>

uint32_t esp_random(void);   /* host_env.c */

#endif
