#include "dash_assets.h"

/*
 * The pixel data is already in LVGL's in-memory layout, so it blits to the
 * framebuffer without conversion. It stays in memory-mapped flash rather than
 * being copied to PSRAM, which keeps redraws off the PSRAM bus that the DSI
 * bridge is already saturating streaming out the framebuffer.
 */

#if DASH_BPP == 2
#define DASH_CF LV_COLOR_FORMAT_RGB565
#else
#define DASH_CF LV_COLOR_FORMAT_RGB888
#endif

/*
 * The artwork depth is deliberately independent of LVGL's. Storing it as RGB565
 * against an RGB888 frame buffer costs a cheap widening in LVGL's blend path but
 * moves a third fewer bytes out of flash on every redraw - and flash and PSRAM
 * share the MSPI bus that the DSI bridge is streaming the frame buffer over.
 */
_Static_assert(DASH_BPP == 2 || DASH_BPP == 3, "unsupported asset pixel depth");

#define EMBEDDED(sym) extern const uint8_t sym[] asm(#sym)

#define DASH_IMAGE(name, sym, width, height)                    \
    EMBEDDED(sym);                                              \
    const lv_image_dsc_t name = {                               \
        .header = {                                             \
            .magic  = LV_IMAGE_HEADER_MAGIC,                    \
            .cf     = DASH_CF,                                  \
            .flags  = 0,                                        \
            .w      = (width),                                  \
            .h      = (height),                                 \
            .stride = (width) * DASH_BPP,                       \
        },                                                      \
        .data_size = (uint32_t)(width) * (height) * DASH_BPP,   \
        .data      = sym,                                       \
    }

DASH_IMAGE(dash_img_bg, _binary_dash_bg_bin_start, DASH_BG_W, DASH_BG_H);

DASH_IMAGE(dash_img_tt_turn_left,  _binary_tt_turn_left_bin_start,  TT_TURN_LEFT_W,  TT_TURN_LEFT_H);
DASH_IMAGE(dash_img_tt_turn_right, _binary_tt_turn_right_bin_start, TT_TURN_RIGHT_W, TT_TURN_RIGHT_H);
DASH_IMAGE(dash_img_tt_headlight,  _binary_tt_headlight_bin_start,  TT_HEADLIGHT_W,  TT_HEADLIGHT_H);
DASH_IMAGE(dash_img_tt_snow,       _binary_tt_snow_bin_start,       TT_SNOW_W,       TT_SNOW_H);
DASH_IMAGE(dash_img_tt_brake,      _binary_tt_brake_bin_start,      TT_BRAKE_W,      TT_BRAKE_H);
DASH_IMAGE(dash_img_tt_warning,    _binary_tt_warning_bin_start,    TT_WARNING_W,    TT_WARNING_H);
DASH_IMAGE(dash_img_tt_engine,     _binary_tt_engine_bin_start,     TT_ENGINE_W,     TT_ENGINE_H);
DASH_IMAGE(dash_img_tt_battery,    _binary_tt_battery_bin_start,    TT_BATTERY_W,    TT_BATTERY_H);

DASH_IMAGE(dash_img_ic_fuel,    _binary_ic_fuel_bin_start,    IC_FUEL_W,    IC_FUEL_H);
DASH_IMAGE(dash_img_ic_coolant, _binary_ic_coolant_bin_start, IC_COOLANT_W, IC_COOLANT_H);
