idf_component_register(
    SRCS "src/dash_inputs.c" "src/dash_debounce.c"
    INCLUDE_DIRS "include"
    REQUIRES dash_signals dash_source driver
)
