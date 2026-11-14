idf_component_register(
    SRCS "src/dash_odo.c" "src/dash_odo_core.c"
    INCLUDE_DIRS "include"
    REQUIRES dash_store
)
