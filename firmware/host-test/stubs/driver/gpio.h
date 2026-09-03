/*
 * driver/gpio.h - host-test stub.
 *
 * Was deliberately empty while only node_graph.h was compiled here (a node
 * stores its pin as a plain int8_t). test_node_graph now links node_graph.c
 * itself, whose wired-input reconciliation calls the GPIO driver, and
 * board_pins.h/cc1101.h name pins with GPIO_NUM_* — so this stub carries the
 * types, the constants, and no-op functions. Nothing here simulates hardware:
 * the tests exercise persistence and rollback, and a "configured" pin that
 * does nothing at all is exactly enough for that.
 */
#ifndef DB_HOSTTEST_DRIVER_GPIO_H
#define DB_HOSTTEST_DRIVER_GPIO_H

#include <stdint.h>

#include "esp_err.h"

typedef int gpio_num_t;

#define GPIO_NUM_NC  (-1)
#define GPIO_NUM_0   0
#define GPIO_NUM_4   4
#define GPIO_NUM_5   5
#define GPIO_NUM_6   6
#define GPIO_NUM_7   7
#define GPIO_NUM_10  10
#define GPIO_NUM_11  11
#define GPIO_NUM_12  12
#define GPIO_NUM_13  13

#define GPIO_IS_VALID_GPIO(n)        ((n) >= 0 && (n) < 49)
#define GPIO_IS_VALID_OUTPUT_GPIO(n) ((n) >= 0 && (n) < 47)

typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum {
    GPIO_INTR_DISABLE = 0,
    GPIO_INTR_POSEDGE,
    GPIO_INTR_NEGEDGE,
    GPIO_INTR_ANYEDGE,
} gpio_int_type_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

typedef void (*gpio_isr_t)(void *arg);

static inline esp_err_t gpio_config(const gpio_config_t *cfg) { (void)cfg; return ESP_OK; }
static inline esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
static inline esp_err_t gpio_isr_handler_add(gpio_num_t pin, gpio_isr_t fn, void *arg)
{ (void)pin; (void)fn; (void)arg; return ESP_OK; }
static inline esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { (void)pin; return ESP_OK; }
static inline esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
static inline esp_err_t gpio_set_intr_type(gpio_num_t pin, gpio_int_type_t t)
{ (void)pin; (void)t; return ESP_OK; }
static inline int gpio_get_level(gpio_num_t pin) { (void)pin; return 1; }

#endif
