/*
 * Widgets and palette shared by the dashboard screens.
 *
 * Each screen lives in its own file and exposes a create + refresh pair; the
 * manager in dash_ui.c owns them and drives whichever is on show.
 */

#pragma once

#include "dash_signals.h"
#include "lvgl.h"

/* Generated faces - LVGL's built-in Montserrat stops at 48. */
LV_FONT_DECLARE(lv_font_montserrat_60);
LV_FONT_DECLARE(lv_font_dash_digits_120);

#define COL_AMBER      lv_color_hex(0xFFA320)
#define COL_VALUE      lv_color_hex(0xF5C542)
#define COL_BIG        lv_color_hex(0xFFE9A8)
#define COL_CORNER     lv_color_hex(0xE8C878)
#define COL_DIM        lv_color_hex(0x6B6B6B)
#define COL_WHITE      lv_color_hex(0xF2F2F2)
#define COL_RED        lv_color_hex(0xFF0000)
#define COL_RULE       lv_color_hex(0x8A6A2A)

#define SCREEN_W       1600
#define SCREEN_H       720

/* ------------------------------------------------------------------------- *
 * A label that only touches LVGL when its text actually changes.
 *
 * Every invalidated pixel costs work, so repainting numbers that have not moved
 * is bandwidth the DSI bridge would rather have.
 * ------------------------------------------------------------------------- */
typedef struct {
    lv_obj_t *obj;
    char      shown[24];
} value_t;

void dash_text_set(value_t *v, const char *next);
void dash_value_set(value_t *v, const char *fmt, float x);

/** Centred label with a fixed box, so changing digits never shift it. */
lv_obj_t *dash_label(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t w,
                     const lv_font_t *font, lv_color_t colour);

/** Left-aligned label anchored at x, vertically centred on cy. */
lv_obj_t *dash_label_left(lv_obj_t *parent, int32_t x, int32_t cy,
                          const lv_font_t *font, lv_color_t colour);

/** Hidden image, for telltale sprites. */
lv_obj_t *dash_sprite(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t x, int32_t y);

/** Plain filled rectangle. */
lv_obj_t *dash_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                    lv_color_t colour);

void dash_show(lv_obj_t *o, bool on, bool *state);

/* ------------------------------------------------------------------------- *
 * Segmented bar, styled after a stepped LED bar graph.
 * ------------------------------------------------------------------------- */
#define SEG_MAX 24

typedef struct {
    lv_obj_t *seg[SEG_MAX];
    uint8_t   count;
    uint8_t   lit;
    uint8_t   danger;
    bool      danger_at_low;
    bool      alarm;
} segbar_t;

void dash_segbar_create(segbar_t *b, lv_obj_t *parent, int32_t x, int32_t y,
                        int32_t w, int32_t h, uint8_t count, bool vertical,
                        int32_t gap, uint8_t danger, bool danger_at_low);
void dash_segbar_set(segbar_t *b, float frac);

/** Alarm: the whole gauge turns red. Segments up to the level are filled red,
    the rest are empty with a red outline - so with no pressure at all it is an
    empty red gauge. */
void dash_segbar_set_alarm(segbar_t *b, bool alarm);

/** 0..1 position of v between lo and hi, unclamped. */
static inline float dash_norm(float v, float lo, float hi)
{
    return (v - lo) / (hi - lo);
}
