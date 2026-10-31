idf_component_register(
    SRCS "src/dash_touch.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl esp_driver_gpio usb
)
