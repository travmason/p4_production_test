#
# Dashboard artwork. The .bin files are generated from assets/dash_bg_source.png
# by tools/make_assets.py - regenerate them if the artwork changes:
#
#     python tools/make_assets.py assets/dash_bg_source.png
#
# They are embedded rather than compiled as C arrays: the background alone is
# 3.3 MB, which would be painfully slow as source.
#
idf_component_register(
    SRCS "src/dash_assets.c"
    INCLUDE_DIRS "include"
    REQUIRES lvgl
    EMBED_FILES
        "assets/dash_bg.bin"
        "assets/tt_turn_left.bin"
        "assets/tt_turn_right.bin"
        "assets/tt_headlight.bin"
        "assets/tt_snow.bin"
        "assets/tt_brake.bin"
        "assets/tt_warning.bin"
        "assets/tt_engine.bin"
        "assets/tt_battery.bin"
        "assets/ic_fuel.bin"
        "assets/ic_coolant.bin"
)
