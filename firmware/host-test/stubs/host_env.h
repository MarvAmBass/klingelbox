/*
 * host_env.h - the test's control panel over the fake ESP environment
 * (host_env.c). Everything here exists so a test can put the "flash" into a
 * state that takes a real box weeks of use or a perfectly-timed power cut to
 * reach, then watch how the persistence code digs itself out.
 */
#ifndef DB_HOSTTEST_HOST_ENV_H
#define DB_HOSTTEST_HOST_ENV_H

#include <stddef.h>
#include <stdint.h>

/* Wipe the fake flash and clear every injected failure / capacity limit. */
void host_nvs_reset(void);

/* Cap the fake partition, in 32-byte NVS entries. 0 = unlimited (the
 * default). The accounting mirrors the firmware's own estimate closely
 * enough that "available < the reserve" reliably trips the store-full check. */
void   host_nvs_set_capacity(size_t entries);
size_t host_nvs_used_entries(void);
size_t host_nvs_available_entries(void);

/* Fail the countdown-th UPCOMING call (1 = the very next) with the given
 * error; 0 disarms. One-shot: after firing, subsequent calls succeed again —
 * which is what lets a test tell "rolled back and retryable" from "wedged". */
void host_nvs_fail_set_blob(int countdown, int err);
void host_nvs_fail_erase(int countdown, int err);

/* Move the fake esp_timer clock forward. */
void host_time_advance_us(int64_t us);

#endif
