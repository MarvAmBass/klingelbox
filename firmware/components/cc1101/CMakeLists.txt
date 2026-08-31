# CC1101 chip driver: SPI transport, register/strobe access, probe, radio
# configuration and async-serial mode switching. Deliberately app-independent —
# it knows nothing about pulses, doorbells or EV1527 (see components/rfpulse).
#
# `driver` supplies spi_master and gpio; `esp_timer` supplies esp_timer_get_time()
# used for the bounded chip-ready / state-change timeouts.
idf_component_register(SRCS "cc1101.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_timer)
