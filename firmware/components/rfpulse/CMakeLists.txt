idf_component_register(SRCS "rf_frame.c" "rf_capture.c" "rf_transmit.c" "rf_decode.c" "rf_ev1527.c"
                            "rf_group.c"
                       INCLUDE_DIRS "include" REQUIRES driver esp_timer)
