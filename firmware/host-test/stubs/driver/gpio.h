/*
 * driver/gpio.h - host-test stub. Deliberately empty.
 *
 * node_graph.h includes it, but stores a wired button's pin as a plain int8_t
 * rather than a gpio_num_t, so nothing the header declares is actually needed to
 * describe a node. See stubs/esp_err.h for why these two files exist.
 */
#ifndef DB_HOSTTEST_DRIVER_GPIO_H
#define DB_HOSTTEST_DRIVER_GPIO_H
#endif
