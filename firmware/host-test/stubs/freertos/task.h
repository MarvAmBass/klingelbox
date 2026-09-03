/* freertos/task.h - host-test stub. xTaskCreate REPORTS success and starts
 * nothing: the graph task's whole job is to drain a queue the stub queue never
 * fills, and running it would drag real concurrency into a deliberately
 * single-threaded binary. */
#ifndef DB_HOSTTEST_FREERTOS_TASK_H
#define DB_HOSTTEST_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                                     uint32_t stack, void *arg,
                                     UBaseType_t prio, TaskHandle_t *out)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)prio;
    if (out) *out = 0;
    return pdPASS;
}
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }

#endif
