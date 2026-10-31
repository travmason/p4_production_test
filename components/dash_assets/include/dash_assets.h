/*
 * dash_assets - the dashboard artwork, as LVGL image descriptors.
 *
 * The background is the full 1600x720 panel with every telltale dimmed to its
 * unlit state. Each telltale also exists as a full-brightness sprite, drawn on
 * top at the exact coordinates it occupies in the background when that lamp
 * lights. dash_geometry.h carries those coordinates, measured from the artwork.
 */

#pragma once

#include "dash_geometry.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_image_dsc_t dash_img_bg;

extern const lv_image_dsc_t dash_img_tt_turn_left;
extern const lv_image_dsc_t dash_img_tt_turn_right;
extern const lv_image_dsc_t dash_img_tt_headlight;
extern const lv_image_dsc_t dash_img_tt_snow;
extern const lv_image_dsc_t dash_img_tt_brake;
extern const lv_image_dsc_t dash_img_tt_warning;
extern const lv_image_dsc_t dash_img_tt_engine;
extern const lv_image_dsc_t dash_img_tt_battery;

/* Lifted from the cluster artwork for reuse on other screens. */
extern const lv_image_dsc_t dash_img_ic_fuel;
extern const lv_image_dsc_t dash_img_ic_coolant;

#ifdef __cplusplus
}
#endif
