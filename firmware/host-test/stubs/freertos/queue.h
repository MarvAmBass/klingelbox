/* freertos/queue.h - host-test stub. The queue "exists" (non-NULL handle,
 * because the firmware posts triggers only `if (s_queue)`) but stores nothing:
 * the graph task that would drain it is never started on the host, and the
 * mutations under test do not go through the queue at all. */
#ifndef DB_HOSTTEST_FREERTOS_QUEUE_H
#define DB_HOSTTEST_FREERTOS_QUEUE_H

#include "freertos/FreeRTOS.h"

typedef void *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    (void)len; (void)item_size;
    static int dummy;
    return (QueueHandle_t)&dummy;
}
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    (void)q; (void)item; (void)wait; return pdTRUE;
}
static inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void *item,
                                           BaseType_t *woken)
{
    (void)q; (void)item;
    if (woken) *woken = pdFALSE;
    return pdTRUE;
}
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    (void)q; (void)item; (void)wait; return pdFALSE;
}

#endif
