/*
 * freertos/FreeRTOS.h - host-test stub.
 *
 * The test binary is single-threaded BY DESIGN: what is under test is
 * persistence and rollback logic, not scheduling. Locks are therefore no-ops
 * (there is nobody to exclude), queues accept and hold nothing (the graph
 * task is never started, so mutations run to completion on the test's own
 * thread exactly as they do under the firmware's mutex), and task creation
 * pretends to succeed. If a test ever needs real concurrency it needs a real
 * harness, not a fatter stub.
 */
#ifndef DB_HOSTTEST_FREERTOS_H
#define DB_HOSTTEST_FREERTOS_H

#include <stdint.h>

typedef int      BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;

#define pdFALSE 0
#define pdTRUE  1
#define pdFAIL  0
#define pdPASS  1

#define portMAX_DELAY     ((TickType_t)0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

/* ISR-side yield: nothing to yield to on the host. */
#define portYIELD_FROM_ISR() do { } while (0)

#endif
