idf_component_register(
    SRCS "src/dash_can.c" "src/dash_rusefi.c"
    INCLUDE_DIRS "include"
    # driver/twai.h is the legacy TWAI driver, which lives in "driver".
    REQUIRES dash_ui dash_signals dash_source driver
)
