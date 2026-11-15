set(srcs "main.c" "gps_can.c")
if(CONFIG_GPS_SOURCE_DRAGY)
    list(APPEND srcs "gps_dragy.c")
endif()
if(CONFIG_GPS_SOURCE_UBLOX)
    list(APPEND srcs "gps_ublox.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES dash_gps_proto driver bt nvs_flash
)
