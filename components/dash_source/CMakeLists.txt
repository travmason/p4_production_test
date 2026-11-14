idf_component_register(
    SRCS "src/dash_source.c" "src/dash_source_nvs.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES dash_signals dash_source_sim dash_ui nvs_flash
)
