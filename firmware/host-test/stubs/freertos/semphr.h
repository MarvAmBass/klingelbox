/* freertos/semphr.h - host-test stub. See FreeRTOS.h for why locks are
 * no-ops here: the binary is single-threaded, so a mutex that always succeeds
 * is behaviourally identical to the real one. The handle is a non-NULL dummy
 * because the firmware guards `if (s_lock)` before taking it. */
#ifndef DB_HOSTTEST_FREERTOS_SEMPHR_H
#define DB_HOSTTEST_FREERTOS_SEMPHR_H

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;
typedef struct { int dummy; } StaticSemaphore_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buf)
{
    return (SemaphoreHandle_t)buf;
}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait)
{
    (void)h; (void)wait; return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t h)
{
    (void)h; return pdTRUE;
}

#endif
