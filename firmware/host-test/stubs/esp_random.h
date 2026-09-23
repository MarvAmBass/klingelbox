/* esp_random.h - host-test stub. Deterministic-enough pseudo-randomness from
 * the C library; the store only uses it to draw EV1527 addresses, and a test
 * asserts on structure, never on which address came out. */
#ifndef DB_HOSTTEST_ESP_RANDOM_H
#define DB_HOSTTEST_ESP_RANDOM_H

#include <stddef.h>
#include <stdint.h>

uint32_t esp_random(void);                     /* host_env.c */
void     esp_fill_random(void *buf, size_t len);   /* host_env.c */

#endif
