#include "dash_ui_common.h"

#include <stdio.h>
#include <string.h>

void dash_text_set(value_t *v, const char *next)
{
    if (strcmp(next, v->shown) == 0) {
        return;
    }
    strncpy(v->shown, next, sizeof(v->shown) - 1);
    v->shown[sizeof(v->shown) - 1] = '\0';
    lv_label_set_text(v->obj, next);
}

/*
 * LVGL's built-in snprintf drops %f unless LV_USE_FLOAT is set - it prints the
 * bare conversion character instead - so formatting goes through the C library.
 */
void dash_value_set(value_t *v, const char *fmt, float x)
{
    char next[sizeof(v->shown)];
    snprintf(next, sizeof(next), fmt, x);

    /* A small negative rounds to "-0" or "-0.0" - an outside temperature just
       below freezing, boost a hair into vacuum. A reading of zero has no sign,
       so drop the minus when every digit of the number is zero. */
    if (next[0] == '-') {
        const char *p = next + 1;
        while (*p == '0' || *p == '.') {
            p++;
        }
        if (*p < '1' || *p > '9') {
            memmove(next, next + 1, strlen(next));
        }
    }

    dash_text_set(v, next);
}

lv_obj_t *dash_label(lv_obj_t *parent, int32_t cx, int32_t cy, int32_t w,
                     const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "--");
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, cx - w / 2, cy - (int32_t)lv_font_get_line_height(font) / 2);
    return l;
}

lv_obj_t *dash_label_left(lv_obj_t *parent, int32_t x, int32_t cy,
                          const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, "--");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_obj_set_pos(l, x, cy - (int32_t)lv_font_get_line_height(font) / 2);
    return l;
}

lv_obj_t *dash_sprite(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t x, int32_t y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    return img;
}

lv_obj_t *dash_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                    lv_color_t colour)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, colour, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void dash_show(lv_obj_t *o, bool on, bool *state)
{
    if (o == NULL || on == *state) {
        return;
    }
    *state = on;
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------------- *
 * Segmented bar
 *
 * The three states are attached once per segment against a state selector, and
 * lighting a segment flips its state.
 *
 * They must not be swapped with lv_obj_remove_style_all(): lv_obj_set_pos() and
 * lv_obj_set_size() store x/y/width/height as *local styles*, so clearing all
 * styles also clears a segment's geometry and drops it at (0,0) in default size.
 * ------------------------------------------------------------------------- */

#define SEG_STATE_ON      LV_STATE_CHECKED
#define SEG_STATE_DANGER  LV_STATE_USER_1
#define SEG_STATE_ALARM   LV_STATE_USER_2   /* unlit, but outlined in red */

static lv_style_t style_seg_off;
static lv_style_t style_seg_on;
static lv_style_t style_seg_danger;
static lv_style_t style_seg_alarm;
static bool       styles_ready;

static void seg_style(lv_style_t *s, uint32_t top, uint32_t bottom, uint32_t border)
{
    lv_style_init(s);
    lv_style_set_bg_color(s, lv_color_hex(top));
    lv_style_set_bg_grad_color(s, lv_color_hex(bottom));
    lv_style_set_bg_grad_dir(s, LV_GRAD_DIR_VER);
    lv_style_set_bg_opa(s, LV_OPA_COVER);
    lv_style_set_border_color(s, lv_color_hex(border));
    lv_style_set_border_width(s, 1);
    lv_style_set_border_opa(s, LV_OPA_COVER);
    lv_style_set_radius(s, 3);
}

static void styles_init(void)
{
    if (styles_ready) {
        return;
    }
    styles_ready = true;
    seg_style(&style_seg_off, 0x241B10, 0x1B140B, 0x3E301C);
    seg_style(&style_seg_on, 0xFFD27A, 0xD8860F, 0xFFE7B0);
    seg_style(&style_seg_danger, 0xFF7A55, 0xC81509, 0xFFA98F);
    seg_style(&style_seg_alarm, 0x3A0A06, 0x2A0604, 0xE0180C);
}

void dash_segbar_create(segbar_t *b, lv_obj_t *parent, int32_t x, int32_t y,
                        int32_t w, int32_t h, uint8_t count, bool vertical,
                        int32_t gap, uint8_t danger, bool danger_at_low)
{
    styles_init();

    b->count         = count > SEG_MAX ? SEG_MAX : count;
    b->lit           = 0;
    b->danger        = danger;
    b->danger_at_low = danger_at_low;
    b->alarm         = false;

    const int32_t span = vertical ? h : w;
    const int32_t seg = (span - gap * (b->count - 1)) / b->count;

    for (int i = 0; i < b->count; i++) {
        lv_obj_t *o = lv_obj_create(parent);
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(o, &style_seg_off, LV_STATE_DEFAULT);
        lv_obj_add_style(o, &style_seg_on, SEG_STATE_ON);
        lv_obj_add_style(o, &style_seg_danger, SEG_STATE_DANGER);
        lv_obj_add_style(o, &style_seg_alarm, SEG_STATE_ALARM);
        if (vertical) {
            /* index 0 at the bottom, so the bar grows upward */
            lv_obj_set_size(o, w, seg);
            lv_obj_set_pos(o, x, y + (span - seg) - i * (seg + gap));
        } else {
            lv_obj_set_size(o, seg, h);
            lv_obj_set_pos(o, x + i * (seg + gap), y);
        }
        b->seg[i] = o;
    }
}

static bool is_danger(const segbar_t *b, int i)
{
    if (b->danger == 0) {
        return false;
    }
    return b->danger_at_low ? (i < b->danger) : (i >= b->count - b->danger);
}

static void restyle(const segbar_t *b, uint8_t i)
{
    lv_obj_remove_state(b->seg[i], SEG_STATE_ON | SEG_STATE_DANGER | SEG_STATE_ALARM);
    if (b->alarm) {
        /* The whole gauge red: what pressure there is filled in, the rest
           empty with a red outline. One state each, never both. */
        lv_obj_add_state(b->seg[i], i < b->lit ? SEG_STATE_DANGER : SEG_STATE_ALARM);
    } else if (i < b->lit) {
        lv_obj_add_state(b->seg[i], is_danger(b, i) ? SEG_STATE_DANGER : SEG_STATE_ON);
    }
}

void dash_segbar_set(segbar_t *b, float frac)
{
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    uint8_t lit = (uint8_t)(frac * b->count + 0.5f);
    if (lit > b->count) {
        lit = b->count;
    }
    if (lit == b->lit) {
        return;
    }

    /* Restyle only the segments that actually crossed the threshold. */
    const uint8_t lo = lit < b->lit ? lit : b->lit;
    const uint8_t hi = lit > b->lit ? lit : b->lit;
    b->lit = lit;
    for (uint8_t i = lo; i < hi; i++) {
        restyle(b, i);
    }
}

void dash_segbar_set_alarm(segbar_t *b, bool alarm)
{
    if (alarm == b->alarm) {
        return;
    }
    b->alarm = alarm;
    for (uint8_t i = 0; i < b->count; i++) {
        restyle(b, i);
    }
}
