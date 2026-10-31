idf_component_register(
    SRCS "src/dash_can.c"
    INCLUDE_DIRS "include"
    REQUIRES dash_ui esp_driver_twai esp32_p4_function_ev_board
)
