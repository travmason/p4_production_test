idf_component_register(
    SRCS
        "src/dash_ui.c"
        "src/dash_ui_common.c"
        "src/dash_ui_cluster.c"
        "src/dash_ui_track.c"
        "src/dash_ui_data.c"
        "fonts/lv_font_montserrat_60.c"
        "fonts/lv_font_dash_digits_120.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES dash_signals dash_assets lvgl
)
