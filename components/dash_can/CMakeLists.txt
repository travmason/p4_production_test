idf_component_register(
    SRCS "src/dash_can.c"
    INCLUDE_DIRS "include"
    # driver/twai.h is the legacy TWAI driver, which lives in "driver".
    REQUIRES dash_ui driver
)
