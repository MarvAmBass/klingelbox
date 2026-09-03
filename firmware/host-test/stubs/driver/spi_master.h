/* driver/spi_master.h - host-test stub. Included transitively through
 * board_pins.h and cc1101.h; only the type and host names are referenced from
 * headers the persistence tests compile, never any SPI function. */
#ifndef DB_HOSTTEST_DRIVER_SPI_MASTER_H
#define DB_HOSTTEST_DRIVER_SPI_MASTER_H

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;

#endif
