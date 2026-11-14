idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_p4_function_ev_board lvgl dash_signals dash_source dash_ui dash_touch dash_can dash_inputs
)
