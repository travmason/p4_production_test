/*
 * Track screen, following the Ford GT cluster.
 *
 * The defining element is a single rpm band that climbs out of the lower left,
 * flattens, and runs across the top, with a bright marker riding along it. Gear
 * and speed sit large underneath; everything else is pushed to the corners.
 *
 * Layout notes
 * ------------
 * The top corners are deliberately left empty: the physical dash curves down
 * there, so anything in them would be hidden. The band therefore stops short of
 * the right edge and the rpm box sits below the top margin rather than in the
 * corner itself.
 *
 * Drawn entirely with LVGL primitives apart from the fuel and coolant icons,
 * which are lifted from the cluster artwork by tools/make_assets.py.
 */

#include <math.h>
#include <stdio.h>

#include "dash_assets.h"
#include "dash_ui_common.h"
#include "dash_ui_screens.h"

#define RPM_MAX_DISP   8000.0f
#define RPM_SHIFT      6500.0f
#define RPM_RED_DISP   7000.0f
#define SMOOTHING      0.28f

/* ---- the band ---------------------------------------------------------- */

/*
 * Built from vertical slices rather than one shape: it has to follow a curve
 * AND fill progressively, and a stack of slices does both. The gaps between
 * slices also give the hatched texture the original has.
 */
#define BAND_SEGS      104
#define BAND_X0        140
#define BAND_X1        1310
#define BAND_GAP       3

#define BAND_Y_LOW     452.0f   /* centreline where the band starts, lower left */
#define BAND_Y_TOP     190.0f   /* centreline once it has flattened out        */
#define BAND_HALF_LOW  7.0f     /* half-thickness at the start                 */
#define BAND_HALF_TOP  34.0f    /* half-thickness along the plateau            */
#define BAND_KNEE      0.30f    /* fraction of the span spent climbing         */

#define TICK_FIRST     1
#define TICK_LAST      7

/* ---- everything else --------------------------------------------------- */

#define RPMBOX_X       1348
#define RPMBOX_Y       198     /* below the top margin, clear of the curve */
#define RPMBOX_W       196
#define RPMBOX_H       104

#define SPEED_CX       620
#define SPEED_CY       486
#define GEAR_CX        1290
#define GEAR_CY        486

#define PRNDM_X0       132
#define PRNDM_DX       64
#define PRNDM_Y        648

#define COL_BAND_LIT   lv_color_hex(0x9EA2A6)
#define COL_BAND_RED   lv_color_hex(0xC81509)
#define COL_MARKER     lv_color_hex(0xFFFFFF)
#define COL_OUTLINE    lv_color_hex(0x6E7276)

static struct {
    lv_obj_t *seg[BAND_SEGS];
    uint8_t   seg_state[BAND_SEGS];   /* 0 off, 1 lit, 2 redline */
    uint8_t   lit;

    lv_obj_t *marker;
    lv_point_precise_t top_pts[BAND_SEGS + 1];
    lv_point_precise_t bot_pts[BAND_SEGS + 1];

    value_t   rpm;
    value_t   speed;
    value_t   gear;
    value_t   range;
    value_t   coolant;
    value_t   odo;

    lv_obj_t *rpm_box;
    lv_obj_t *prndm[5];
    int       prndm_shown;

    lv_obj_t *fuel_fill;
    lv_obj_t *temp_fill;
    int32_t   fuel_w, temp_w;

    float     speed_shown;
    float     rpm_shown;
    int       rpm_zone_shown;
} ui;

static const char *kPrndm[5] = {"P", "R", "N", "D", "M"};

/* ------------------------------------------------------------------------- *
 * Band geometry
 * ------------------------------------------------------------------------- */

static float smoothstep(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }
    return x * x * (3.0f - 2.0f * x);
}

/* Climbs over the first BAND_KNEE of the span, then holds level. */
static float band_rise(float t)
{
    return smoothstep(t / BAND_KNEE);
}

static float band_centre(float t)
{
    const float k = band_rise(t);
    return BAND_Y_LOW + (BAND_Y_TOP - BAND_Y_LOW) * k;
}

static float band_half(float t)
{
    const float k = band_rise(t);
    return BAND_HALF_LOW + (BAND_HALF_TOP - BAND_HALF_LOW) * k;
}

static int32_t band_x(float t)
{
    return BAND_X0 + (int32_t)((BAND_X1 - BAND_X0) * t);
}

/* ------------------------------------------------------------------------- *
 * Construction
 * ------------------------------------------------------------------------- */

static lv_obj_t *stroke(lv_obj_t *parent, lv_point_precise_t *pts, uint32_t n,
                        int32_t width, lv_color_t c)
{
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, pts, n);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, c, 0);
    lv_obj_set_style_line_rounded(l, false, 0);
    lv_obj_set_pos(l, 0, 0);
    return l;
}

static void build_band(lv_obj_t *scr)
{
    const int32_t seg_w = (BAND_X1 - BAND_X0) / BAND_SEGS;

    for (int i = 0; i < BAND_SEGS; i++) {
        const float t = (i + 0.5f) / BAND_SEGS;
        const float c = band_centre(t);
        const float h = band_half(t);

        lv_obj_t *o = lv_obj_create(scr);
        lv_obj_remove_style_all(o);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(o, seg_w - BAND_GAP, (int32_t)(h * 2.0f));
        lv_obj_set_pos(o, BAND_X0 + i * seg_w, (int32_t)(c - h));
        lv_obj_set_style_bg_color(o, COL_BAND_LIT, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);   /* starts unlit */
        ui.seg[i] = o;
        ui.seg_state[i] = 0;
    }

    /* Continuous outline along both edges. Segments alone would give a dashed
       edge; two polylines keep the curve unbroken. */
    for (int i = 0; i <= BAND_SEGS; i++) {
        const float t = (float)i / BAND_SEGS;
        const int32_t x = band_x(t);
        const float c = band_centre(t);
        const float h = band_half(t);
        ui.top_pts[i] = (lv_point_precise_t) {x, (int32_t)(c - h)};
        ui.bot_pts[i] = (lv_point_precise_t) {x, (int32_t)(c + h)};
    }
    stroke(scr, ui.top_pts, BAND_SEGS + 1, 2, COL_OUTLINE);
    stroke(scr, ui.bot_pts, BAND_SEGS + 1, 2, COL_OUTLINE);

    /* Marker rides the band; sized for the plateau and repositioned each frame. */
    ui.marker = lv_obj_create(scr);
    lv_obj_remove_style_all(ui.marker);
    lv_obj_remove_flag(ui.marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(ui.marker, 7, (int32_t)(BAND_HALF_TOP * 2.0f) + 22);
    lv_obj_set_style_bg_color(ui.marker, COL_MARKER, 0);
    lv_obj_set_style_bg_opa(ui.marker, LV_OPA_COVER, 0);
    lv_obj_set_pos(ui.marker, BAND_X0, (int32_t)(BAND_Y_LOW));

    /* Tick numbers, sitting just under the band and following the curve. */
    for (int k = TICK_FIRST; k <= TICK_LAST; k++) {
        const float t = (k * 1000.0f) / RPM_MAX_DISP;
        const int32_t x = band_x(t);
        const int32_t y = (int32_t)(band_centre(t) + band_half(t)) + 30;
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", k);
        lv_obj_t *l = dash_label(scr, x, y, 60, &lv_font_montserrat_28,
                                 (k * 1000.0f >= RPM_RED_DISP) ? COL_RED : COL_WHITE);
        lv_label_set_text(l, buf);
    }
}

/* Small red level bar, as used for range and coolant in the original. */
static lv_obj_t *mini_bar(lv_obj_t *scr, int32_t x, int32_t y, int32_t w)
{
    dash_rect(scr, x, y, w, 6, lv_color_hex(0x3A1010));
    lv_obj_t *fill = dash_rect(scr, x, y, 1, 6, lv_color_hex(0xE01808));
    return fill;
}

lv_obj_t *dash_screen_track_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Deliberately NOT lv_obj_remove_style_all(): that strips the theme's
       opaque screen background, and without a known-opaque root LVGL widens
       every redraw. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    build_band(scr);

    /* ---- range and coolant, upper left, clear of the climbing band ---- */
    lv_obj_t *fuel_ic = lv_image_create(scr);
    lv_image_set_src(fuel_ic, &dash_img_ic_fuel);
    lv_obj_set_pos(fuel_ic, 56, 150);
    ui.range.obj = dash_label_left(scr, 104, 168, &lv_font_montserrat_28, COL_WHITE);
    ui.fuel_fill = mini_bar(scr, 56, 196, 168);

    lv_obj_t *cool_ic = lv_image_create(scr);
    lv_image_set_src(cool_ic, &dash_img_ic_coolant);
    lv_obj_set_pos(cool_ic, 56, 226);
    ui.coolant.obj = dash_label_left(scr, 104, 246, &lv_font_montserrat_28, COL_WHITE);
    ui.temp_fill = mini_bar(scr, 56, 274, 168);

    /* ---- rpm readout, boxed and red ---- */
    ui.rpm_box = lv_obj_create(scr);
    lv_obj_remove_style_all(ui.rpm_box);
    lv_obj_set_pos(ui.rpm_box, RPMBOX_X, RPMBOX_Y);
    lv_obj_set_size(ui.rpm_box, RPMBOX_W, RPMBOX_H);
    lv_obj_set_style_border_color(ui.rpm_box, COL_RED, 0);
    lv_obj_set_style_border_width(ui.rpm_box, 3, 0);
    lv_obj_set_style_border_opa(ui.rpm_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui.rpm_box, 3, 0);
    lv_obj_remove_flag(ui.rpm_box, LV_OBJ_FLAG_SCROLLABLE);

    ui.rpm.obj = dash_label(scr, RPMBOX_X + RPMBOX_W / 2, RPMBOX_Y + 34, RPMBOX_W - 14,
                            &lv_font_montserrat_48, COL_WHITE);
    lv_obj_t *rpm_unit = dash_label(scr, RPMBOX_X + RPMBOX_W / 2, RPMBOX_Y + 78, RPMBOX_W,
                                    &lv_font_montserrat_20, COL_RED);
    lv_label_set_text(rpm_unit, "RPM");

    /* ---- speed, dominant ---- */
    lv_obj_t *unit = dash_label(scr, SPEED_CX, SPEED_CY - 122, 320,
                                &lv_font_montserrat_28, COL_DIM);
    lv_label_set_text(unit, "km/h");
    ui.speed.obj = dash_label(scr, SPEED_CX, SPEED_CY, 620,
                              &lv_font_dash_digits_120, COL_WHITE);

    /* ---- gear ---- */
    lv_obj_t *gear_lbl = dash_label(scr, GEAR_CX, GEAR_CY - 122, 260,
                                    &lv_font_montserrat_28, COL_DIM);
    lv_label_set_text(gear_lbl, "GEAR");
    ui.gear.obj = dash_label(scr, GEAR_CX, GEAR_CY, 280,
                             &lv_font_dash_digits_120, COL_WHITE);

    /* Dotted rule linking the two, as on the original. */
    for (int32_t x = SPEED_CX + 250; x < GEAR_CX - 160; x += 24) {
        dash_rect(scr, x, SPEED_CY + 4, 7, 4, COL_DIM);
    }

    /* ---- PRNDM ---- */
    for (int i = 0; i < 5; i++) {
        ui.prndm[i] = dash_label(scr, PRNDM_X0 + i * PRNDM_DX, PRNDM_Y, 58,
                                 &lv_font_montserrat_28, COL_DIM);
        lv_label_set_text(ui.prndm[i], kPrndm[i]);
    }
    ui.prndm_shown = -1;

    /* ---- odometer ---- */
    ui.odo.obj = dash_label(scr, 1400, PRNDM_Y, 300, &lv_font_montserrat_28, COL_DIM);

    return scr;
}

/* ------------------------------------------------------------------------- *
 * Refresh
 * ------------------------------------------------------------------------- */

static void band_set(float frac)
{
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }

    uint8_t lit = (uint8_t)(frac * BAND_SEGS + 0.5f);
    if (lit > BAND_SEGS) {
        lit = BAND_SEGS;
    }

    if (lit != ui.lit) {
        const uint8_t lo = lit < ui.lit ? lit : ui.lit;
        const uint8_t hi = lit > ui.lit ? lit : ui.lit;
        for (uint8_t i = lo; i < hi; i++) {
            const float t = (i + 0.5f) / BAND_SEGS;
            const uint8_t want = (i >= lit) ? 0
                                 : (t * RPM_MAX_DISP >= RPM_RED_DISP ? 2 : 1);
            if (want == ui.seg_state[i]) {
                continue;
            }
            ui.seg_state[i] = want;
            if (want == 0) {
                lv_obj_set_style_bg_opa(ui.seg[i], LV_OPA_TRANSP, 0);
            } else {
                lv_obj_set_style_bg_color(ui.seg[i],
                                          want == 2 ? COL_BAND_RED : COL_BAND_LIT, 0);
                lv_obj_set_style_bg_opa(ui.seg[i], LV_OPA_COVER, 0);
            }
        }
        ui.lit = lit;
    }

    /* Marker follows the curve, so it stays centred on the band. */
    const float c = band_centre(frac);
    const float h = band_half(frac);
    lv_obj_set_pos(ui.marker, band_x(frac) - 3, (int32_t)(c - h - 11));
    lv_obj_set_height(ui.marker, (int32_t)(h * 2.0f) + 22);
}

static void set_fill(lv_obj_t *bar, int32_t *shown, int32_t track_w, float frac)
{
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    int32_t w = (int32_t)(track_w * frac);
    if (w < 1) {
        w = 1;
    }
    if (w == *shown) {
        return;
    }
    *shown = w;
    lv_obj_set_width(bar, w);
}

void dash_screen_track_refresh(const dash_signals_t *s)
{
    ui.speed_shown += (s->speed_kmh - ui.speed_shown) * SMOOTHING;
    ui.rpm_shown   += (s->rpm - ui.rpm_shown) * SMOOTHING;

    band_set(ui.rpm_shown / RPM_MAX_DISP);

    const int zone = (ui.rpm_shown >= RPM_RED_DISP) ? 2
                     : (ui.rpm_shown >= RPM_SHIFT) ? 1 : 0;
    if (zone != ui.rpm_zone_shown) {
        ui.rpm_zone_shown = zone;
        lv_obj_set_style_text_color(ui.rpm.obj,
                                    zone == 2 ? COL_RED
                                    : zone == 1 ? lv_color_hex(0xFFB020) : COL_WHITE, 0);
    }

    dash_value_set(&ui.rpm, "%.0f", (float)((int)(ui.rpm_shown / 10.0f + 0.5f)) * 10.0f);
    dash_value_set(&ui.speed, "%.0f", ui.speed_shown);
    dash_value_set(&ui.gear, "%.0f", (float)s->gear);
    dash_value_set(&ui.range, "%.0f km", s->fuel_level_pct * 6.2f);
    dash_value_set(&ui.coolant, "%.0f°", s->coolant_temp_c);
    dash_value_set(&ui.odo, "%.1f km", s->odometer_km);

    set_fill(ui.fuel_fill, &ui.fuel_w, 168, s->fuel_level_pct / 100.0f);
    set_fill(ui.temp_fill, &ui.temp_w, 168, dash_norm(s->coolant_temp_c, 60.0f, 110.0f));

    /* The simulator only ever drives, so this tracks motion rather than a real
       selector. */
    const int want = (s->speed_kmh < 1.0f) ? 2 : 3;   /* N when stopped, else D */
    if (want != ui.prndm_shown) {
        for (int i = 0; i < 5; i++) {
            lv_obj_set_style_text_color(ui.prndm[i], i == want ? COL_WHITE : COL_DIM, 0);
        }
        ui.prndm_shown = want;
    }
}
