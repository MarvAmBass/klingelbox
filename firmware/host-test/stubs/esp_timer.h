/* esp_timer.h - host-test stub. A settable fake clock, because the logic
 * under test reasons about time (deferred switch saves, fragment windows) and
 * a test must be able to move the clock rather than sleep. */
#ifndef DB_HOSTTEST_ESP_TIMER_H
#define DB_HOSTTEST_ESP_TIMER_H

#include <stdint.h>

int64_t esp_timer_get_time(void);            /* host_env.c: the fake clock */
void    host_time_advance_us(int64_t us);    /* test hook                  */

#endif
