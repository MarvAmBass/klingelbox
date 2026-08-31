idf_component_register(SRCS "rf_frame.c" "rf_capture.c" "rf_transmit.c" "rf_decode.c" "rf_ev1527.c"
                       INCLUDE_DIRS "include" REQUIRES driver esp_timer)
