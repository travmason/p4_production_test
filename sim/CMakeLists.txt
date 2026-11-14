#
# Mac simulator for the dashboard. A plain CMake project, separate from the
# ESP-IDF build: it compiles the firmware's UI sources as they are, against
# LVGL's SDL driver and a handful of host stand-ins for ESP-IDF and FreeRTOS.
#
#     cmake -S sim -B sim/build
#     cmake --build sim/build -j
#     ./sim/build/dash_sim
#
# Needs SDL2 (brew install sdl2) and the LVGL sources that the ESP-IDF
# component manager downloads, so run `idf.py build` (or `idf.py reconfigure`)
# once first. Using that copy keeps the simulator on the firmware's LVGL version.
#
cmake_minimum_required(VERSION 3.16)
project(dash_sim C ASM)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_C_STANDARD 11)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(COMP "${ROOT}/components")
set(LVGL "${ROOT}/managed_components/lvgl__lvgl")

if(NOT EXISTS "${LVGL}/lvgl.h")
    message(FATAL_ERROR
        "LVGL was not found at ${LVGL}.\n"
        "Run `idf.py reconfigure` in the project root once so the component "
        "manager downloads it.")
endif()

find_package(SDL2 REQUIRED)

# ---- LVGL -------------------------------------------------------------------
file(GLOB_RECURSE LVGL_SRCS "${LVGL}/src/*.c")
add_library(lvgl STATIC ${LVGL_SRCS})
# lv_conf.h is picked up from this directory.
target_include_directories(lvgl PUBLIC "${LVGL}" "${CMAKE_CURRENT_LIST_DIR}")
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)
target_link_libraries(lvgl PUBLIC SDL2::SDL2)

# ---- Artwork ----------------------------------------------------------------
# The firmware embeds each .bin with EMBED_FILES, which gives it a
# _binary_<name>_bin_start symbol. Do the same here with .incbin.
set(ASSET_DIR "${COMP}/dash_assets/assets")
file(GLOB ASSET_BINS "${ASSET_DIR}/*.bin")
set(ASSET_ASM "${CMAKE_CURRENT_BINARY_DIR}/dash_assets_embed.S")
set(ASM_TEXT "    .section __DATA,__const\n")
foreach(bin ${ASSET_BINS})
    get_filename_component(name "${bin}" NAME_WE)
    string(APPEND ASM_TEXT
        "    .globl _binary_${name}_bin_start\n"
        "    .p2align 4\n"
        "_binary_${name}_bin_start:\n"
        "    .incbin \"${bin}\"\n")
endforeach()
file(WRITE "${ASSET_ASM}" "${ASM_TEXT}")
# .incbin is invisible to the dependency scanner, so say it by hand: regenerate
# the artwork with tools/make_assets.py and the next build picks it up.
set_source_files_properties("${ASSET_ASM}" PROPERTIES OBJECT_DEPENDS "${ASSET_BINS}")

# ---- The dashboard, as built for the board ------------------------------------
add_executable(dash_sim
    main.c
    shim/shim.c
    "${ASSET_ASM}"
    "${COMP}/dash_assets/src/dash_assets.c"
    "${COMP}/dash_signals/src/dash_signals.c"
    "${COMP}/dash_source_sim/src/dash_source_sim.c"
    "${COMP}/dash_source/src/dash_source.c"
    "${COMP}/dash_ui/src/dash_ui.c"
    "${COMP}/dash_ui/src/dash_ui_common.c"
    "${COMP}/dash_ui/src/dash_ui_cluster.c"
    "${COMP}/dash_ui/src/dash_ui_track.c"
    "${COMP}/dash_ui/src/dash_ui_data.c"
    "${COMP}/dash_ui/fonts/lv_font_montserrat_60.c"
    "${COMP}/dash_ui/fonts/lv_font_dash_digits_120.c"
)
target_include_directories(dash_sim PRIVATE
    shim
    "${COMP}/dash_assets/include"
    "${COMP}/dash_signals/include"
    "${COMP}/dash_source_sim/include"
    "${COMP}/dash_source/include"
    "${COMP}/dash_source/src"
    "${COMP}/dash_ui/include"
    "${COMP}/dash_ui/src"
)
target_compile_options(dash_sim PRIVATE -Wall -Wextra)
target_link_libraries(dash_sim PRIVATE lvgl m pthread)

# ---- Host tests ---------------------------------------------------------------
# Pure-C pieces of the firmware that need no board: `ctest --test-dir sim/build`.
enable_testing()
add_executable(test_rusefi
    "${ROOT}/tests/test_rusefi.c"
    "${COMP}/dash_can/src/dash_rusefi.c"
)
target_include_directories(test_rusefi PRIVATE "${COMP}/dash_can/include")
target_compile_options(test_rusefi PRIVATE -Wall -Wextra)
target_link_libraries(test_rusefi PRIVATE m)
add_test(NAME rusefi_decoder COMMAND test_rusefi)

add_executable(test_debounce
    "${ROOT}/tests/test_debounce.c"
    "${COMP}/dash_inputs/src/dash_debounce.c"
)
target_include_directories(test_debounce PRIVATE "${COMP}/dash_inputs/include")
target_compile_options(test_debounce PRIVATE -Wall -Wextra)
add_test(NAME debounce COMMAND test_debounce)

add_executable(test_oil_alarm
    "${ROOT}/tests/test_oil_alarm.c"
    "${COMP}/dash_can/src/dash_oil_alarm.c"
)
target_include_directories(test_oil_alarm PRIVATE "${COMP}/dash_can/include")
target_compile_options(test_oil_alarm PRIVATE -Wall -Wextra)
add_test(NAME oil_alarm COMMAND test_oil_alarm)

add_executable(test_gps
    "${ROOT}/tests/test_gps.c"
    "${COMP}/dash_gps_proto/src/dash_gps_proto.c"
    "${COMP}/dash_gps_proto/src/dash_ubx.c"
)
target_include_directories(test_gps PRIVATE "${COMP}/dash_gps_proto/include")
target_compile_options(test_gps PRIVATE -Wall -Wextra)
target_link_libraries(test_gps PRIVATE m)
add_test(NAME gps COMMAND test_gps)
