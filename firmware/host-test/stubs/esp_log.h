/* esp_log.h - host-test stub. Logging is swallowed by a real variadic
 * function rather than a bare no-op macro so every argument expression is
 * still compiled and evaluated — a log line with a typo in it must fail the
 * host build exactly as it fails the device build. Set HOSTTEST_LOG=1 in the
 * environment to see the output when debugging a test. */
#ifndef DB_HOSTTEST_ESP_LOG_H
#define DB_HOSTTEST_ESP_LOG_H

void host_log(const char *tag, const char *fmt, ...)   /* host_env.c */
    __attribute__((format(printf, 2, 3)));

#define ESP_LOGE host_log
#define ESP_LOGW host_log
#define ESP_LOGI host_log
#define ESP_LOGD host_log
#define ESP_LOGV host_log

#endif
