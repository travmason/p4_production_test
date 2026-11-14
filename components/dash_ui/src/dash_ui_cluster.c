/*
 * The main instrument cluster, composited over the artwork in dash_assets.
 *
 * Only what moves is drawn here: two sweep arcs with their centre readouts,
 * four segmented bar gauges, the middle column figures, a clock, an outside
 * temperature, and the telltale sprites that overlay their dimmed counterparts
 * in the background image.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dash_assets.h"
#include "dash_ui_common.h"
#include "dash_ui_screens.h"

/*
 * Sweep arc. It rides just inside the dial's outer ring, over the tick marks,
 * so the lit length reads directly against the printed numbers. The radial
 * gradient runs from near-white and opaque at the outer edge to yellow and
 * nearly transparent at the inner edge.
 */
#define ARC_R_OUT      296
#define ARC_BANDS      8
#define ARC_BAND_W     3       /* 8 x 3 = 24 px of thickness, r 272..296 */
#define ARC_RANGE      1000
#define ARC_OPA_OUT    255
#define ARC_OPA_IN     35

/* ARC_ZONE_RED uses 0xFF0000, sampled from the redline printed on the artwork,
   so the arc and the redline are the same red. */
typedef enum {
    ARC_ZONE_NORMAL = 0,
    ARC_ZONE_WARN,
    ARC_ZONE_RED,
    ARC_ZONE_COUNT
} arc_zone_t;

static const struct {
    uint32_t outer;
    uint32_t inner;
} kZoneCol[ARC_ZONE_COUNT] = {
    [ARC_ZONE_NORMAL] = {0xFFFBF0, 0xFFD24A},
    [ARC_ZONE_WARN]   = {0xFFC8B8, 0xFF7060},
    [ARC_ZONE_RED]    = {0xFF5040, 0xFF0000},
};

#define RPM_WARN       6500.0f
#define RPM_RED        7000.0f
#define RPM_ZONE_HYST  120.0f   /* stops the colour flickering on the boundary */

#define SPEED_MAX      160.0f
#define RPM_MAX        8000.0f
#define RPM_STEP       50.0f    /* the readout only moves in these increments */
#define TEMP_LO        60.0f
#define TEMP_HI        110.0f
#define OILP_MAX       80.0f
#define OILT_LO        40.0f
#define OILT_HI        140.0f

#define SMOOTHING      0.28f

#define BAR_SEGS       12
#define VBAR_X_L       678     /* clear of the left dial's outer ring */
#define VBAR_X_R       886
#define VBAR_W         34
#define VBAR_Y         248
#define VBAR_SEG_H     17
#define VBAR_GAP       4
#define HBAR_Y         526     /* just under the artwork's track rule */
#define HBAR_H         22
#define HBAR_GAP       3

/* The artwork's bottom corners are clear below y=622, right across the width. */
#define CORNER_Y       695
#define TEMP_CX        140
#define CLOCK_CX       1460

/* High-beam blue, as 8-bit red, green, blue. */
#define HIGH_BEAM_R    0x2F
#define HIGH_BEAM_G    0x7B
#define HIGH_BEAM_B    0xFF
/* The "psi" caption and oil can over the pressure bar, in the background. */
#define OIL_ICON_X     876
#define OIL_ICON_Y     166
#define OIL_ICON_W     56
#define OIL_ICON_H     70

/* Ice warning sits above the outside temperature it belongs to. Its sprite is
   erased from the artwork's telltale row, so this is the only place it shows. */
#define SNOW_Y         626
#define OAT_ICE_C      4.0f

typedef struct {
    lv_obj_t  *band[ARC_BANDS];
    value_t    readout;
    float      shown;
    int32_t    last_v;
    arc_zone_t zone;
} arcgauge_t;

static struct {
    arcgauge_t speedo;
    arcgauge_t tacho;

    segbar_t  fuel;
    segbar_t  water;
    segbar_t  oil_temp;
    segbar_t  oil_press;

    value_t   trip;
    value_t   total;
    value_t   turbo;
    value_t   clock;
    value_t   ambient;

    lv_obj_t *telltale[7];
    uint32_t  telltale_bit[7];
    uint32_t  telltale_shown;

    lv_obj_t *snow;        /* driven by outside temperature, not a telltale bit */
    lv_obj_t *oil;         /* red copy of the oil pressure caption; may be NULL */
    bool      oil_alarm;
    bool      oil_shown;
    lv_obj_t *high_beam;   /* the headlight lamp, recoloured blue; may be NULL */
    bool      high_beam_shown;
    bool      snow_shown;
} ui;

enum {
    TT_I_TURN_LEFT, TT_I_HEADLIGHT, TT_I_TURN_RIGHT,
    TT_I_BRAKE, TT_I_WARNING, TT_I_ENGINE, TT_I_BATTERY, TT_I_COUNT
};

/* ------------------------------------------------------------------------- *
 * Sweep arc
 * ------------------------------------------------------------------------- */

/* Recolour every band. Cheap because it only runs when the zone changes. */
static void arc_set_zone(arcgauge_t *g, arc_zone_t z)
{
    if (z == g->zone) {
        return;
    }
    g->zone = z;
    for (int i = 0; i < ARC_BANDS; i++) {
        const uint8_t mix = (uint8_t)(255 * i / (ARC_BANDS - 1));
        lv_obj_set_style_arc_color(g->band[i],
                                   lv_color_mix(lv_color_hex(kZoneCol[z].inner),
                                                lv_color_hex(kZoneCol[z].outer), mix),
                                   LV_PART_INDICATOR);
    }
}

/* Latching thresholds, so an engine idling exactly on 6500 does not strobe. */
static arc_zone_t rpm_zone(float rpm, arc_zone_t cur)
{
    switch (cur) {
    case ARC_ZONE_RED:
        return rpm < RPM_RED - RPM_ZONE_HYST ? ARC_ZONE_WARN : ARC_ZONE_RED;
    case ARC_ZONE_WARN:
        if (rpm >= RPM_RED) {
            return ARC_ZONE_RED;
        }
        return rpm < RPM_WARN - RPM_ZONE_HYST ? ARC_ZONE_NORMAL : ARC_ZONE_WARN;
    default:
        return rpm >= RPM_WARN ? ARC_ZONE_WARN : ARC_ZONE_NORMAL;
    }
}

/*
 * The radial gradient is built from concentric arc bands. lv_arc invalidates
 * only the angular slice that changed, so a moving arc repaints a thin wedge
 * rather than the whole 600 px dial.
 */
static void arc_create(arcgauge_t *g, lv_obj_t *parent, int32_t cx, int32_t cy,
                       int32_t start_deg, int32_t sweep_deg)
{
    const int32_t end_deg = (start_deg + sweep_deg) % 360;

    for (int i = 0; i < ARC_BANDS; i++) {
        const int32_t r = ARC_R_OUT - i * ARC_BAND_W;
        const uint8_t mix = (uint8_t)(255 * i / (ARC_BANDS - 1));
        const lv_opa_t opa =
            (lv_opa_t)(ARC_OPA_OUT - (ARC_OPA_OUT - ARC_OPA_IN) * i / (ARC_BANDS - 1));

        lv_obj_t *a = lv_arc_create(parent);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(a, r * 2, r * 2);
        lv_obj_set_pos(a, cx - r, cy - r);
        lv_obj_set_style_pad_all(a, 0, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);

        /* Hide the track; the artwork already provides the dial face. */
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, ARC_BAND_W, LV_PART_MAIN);

        lv_obj_set_style_arc_width(a, ARC_BAND_W, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a,
                                   lv_color_mix(lv_color_hex(kZoneCol[ARC_ZONE_NORMAL].inner),
                                                lv_color_hex(kZoneCol[ARC_ZONE_NORMAL].outer), mix),
                                   LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);

        lv_arc_set_bg_angles(a, start_deg, end_deg);
        lv_arc_set_range(a, 0, ARC_RANGE);
        lv_arc_set_value(a, 0);

        g->band[i] = a;
    }

    g->shown  = 0.0f;
    g->last_v = -1;
    g->zone   = ARC_ZONE_NORMAL;

    g->readout.obj = dash_label(parent, cx, cy - 30, 340, &lv_font_montserrat_60, COL_BIG);
    g->readout.shown[0] = '\0';
}

static void arc_set(arcgauge_t *g, float frac)
{
    if (frac < 0.0f) {
        frac = 0.0f;
    } else if (frac > 1.0f) {
        frac = 1.0f;
    }
    const int32_t v = (int32_t)(frac * ARC_RANGE);
    if (v == g->last_v) {
        return;
    }
    g->last_v = v;
    for (int i = 0; i < ARC_BANDS; i++) {
        lv_arc_set_value(g->band[i], v);
    }
}

/* ------------------------------------------------------------------------- *
 * Screen
 * ------------------------------------------------------------------------- */

/*
 * A recoloured copy of part of an opaque picture: each pixel keeps its
 * brightness and takes the given colour.
 *
 * The artwork is opaque pictures on black, so LVGL's own recolour would tint
 * the black as well and give a coloured box. Built once per use, a few
 * kilobytes each. Returns NULL if the memory is not there.
 */
static lv_obj_t *tinted_sprite(lv_obj_t *parent, const lv_image_dsc_t *src,
                               int32_t sx, int32_t sy, int32_t w, int32_t h,
                               uint32_t tr, uint32_t tg, uint32_t tb,
                               int32_t x, int32_t y)
{
    const bool rgb565 = src->header.cf == LV_COLOR_FORMAT_RGB565;
    const uint32_t bpp = rgb565 ? 2 : 3;

    lv_image_dsc_t *dsc = malloc(sizeof(*dsc) + (size_t)w * h * bpp);
    if (dsc == NULL) {
        return NULL;
    }
    uint8_t *px = (uint8_t *)(dsc + 1);

    for (int32_t row = 0; row < h; row++) {
        const uint8_t *in = src->data + (size_t)(sy + row) * src->header.stride + (size_t)sx * bpp;
        uint8_t *out = px + (size_t)row * w * bpp;
        for (int32_t i = 0; i < w; i++) {
            uint32_t r, g, b;
            if (rgb565) {
                const uint16_t v = (uint16_t)(in[i * 2] | (in[i * 2 + 1] << 8));
                r = ((v >> 11) & 0x1F) * 255 / 31;
                g = ((v >> 5) & 0x3F) * 255 / 63;
                b = (v & 0x1F) * 255 / 31;
            } else {
                b = in[i * 3];
                g = in[i * 3 + 1];
                r = in[i * 3 + 2];
            }
            uint32_t level = r > g ? r : g;
            if (b > level) {
                level = b;
            }
            r = tr * level / 255;
            g = tg * level / 255;
            b = tb * level / 255;
            if (rgb565) {
                const uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                out[i * 2]     = (uint8_t)(v & 0xFF);
                out[i * 2 + 1] = (uint8_t)(v >> 8);
            } else {
                out[i * 3]     = (uint8_t)b;
                out[i * 3 + 1] = (uint8_t)g;
                out[i * 3 + 2] = (uint8_t)r;
            }
        }
    }

    *dsc = *src;
    dsc->header.w = (uint32_t)w;
    dsc->header.h = (uint32_t)h;
    dsc->header.stride = (uint32_t)(w * bpp);
    dsc->data_size = (uint32_t)(w * h * bpp);
    dsc->data = px;
    return dash_sprite(parent, dsc, x, y);
}

lv_obj_t *dash_screen_cluster_create(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    /* Deliberately NOT lv_obj_remove_style_all(): that strips the theme's
       opaque screen background, and without a known-opaque root LVGL widens
       every redraw, which pins the draw thread solid on the arc-heavy cluster. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_radius(scr, 0, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Created first, so everything else lands on top of it. */
    lv_obj_t *bg = lv_image_create(scr);
    lv_image_set_src(bg, &dash_img_bg);
    lv_obj_set_pos(bg, 0, 0);

    /* Horizontal bars sit just below the artwork's FUEL / TEMP rule, spanning
       exactly its width. The E/F and L/H letters are outside that span. */
    dash_segbar_create(&ui.fuel, scr, FUEL_TRACK_X, HBAR_Y, FUEL_TRACK_W, HBAR_H,
                       BAR_SEGS, false, HBAR_GAP, 2, true);
    dash_segbar_create(&ui.water, scr, TEMP_TRACK_X, HBAR_Y, TEMP_TRACK_W, HBAR_H,
                       BAR_SEGS, false, HBAR_GAP, 2, false);

    /* Vertical bars run down the clear gutters under the two oil-can icons. */
    const int32_t vbar_h = BAR_SEGS * VBAR_SEG_H + (BAR_SEGS - 1) * VBAR_GAP;
    dash_segbar_create(&ui.oil_temp, scr, VBAR_X_L, VBAR_Y, VBAR_W, vbar_h,
                       BAR_SEGS, true, VBAR_GAP, 2, false);
    dash_segbar_create(&ui.oil_press, scr, VBAR_X_R, VBAR_Y, VBAR_W, vbar_h,
                       BAR_SEGS, true, VBAR_GAP, 2, true);

    ui.trip.obj  = dash_label(scr, VAL_TRIP_CX, VAL_TRIP_CY, 150, &lv_font_montserrat_28, COL_VALUE);
    ui.total.obj = dash_label(scr, VAL_TOTAL_CX, VAL_TOTAL_CY, 150, &lv_font_montserrat_28, COL_VALUE);
    ui.turbo.obj = dash_label(scr, VAL_TURBO_CX, VAL_TURBO_CY, 150, &lv_font_montserrat_28, COL_VALUE);

    ui.ambient.obj = dash_label(scr, TEMP_CX, CORNER_Y, 220, &lv_font_montserrat_28, COL_CORNER);
    ui.clock.obj   = dash_label(scr, CLOCK_CX, CORNER_Y, 220, &lv_font_montserrat_28, COL_CORNER);

    arc_create(&ui.speedo, scr, DIAL_L_CX, DIAL_L_CY, DIAL_L_START, DIAL_L_SWEEP);
    arc_create(&ui.tacho, scr, DIAL_R_CX, DIAL_R_CY, DIAL_R_START, DIAL_R_SWEEP);

    static const struct {
        const lv_image_dsc_t *src;
        int32_t x, y;
        uint32_t bit;
    } kTelltales[TT_I_COUNT] = {
        [TT_I_TURN_LEFT]  = {&dash_img_tt_turn_left,  TT_TURN_LEFT_X,  TT_TURN_LEFT_Y,  DASH_TT_TURN_LEFT},
        [TT_I_HEADLIGHT]  = {&dash_img_tt_headlight,  TT_HEADLIGHT_X,  TT_HEADLIGHT_Y,  DASH_TT_HEADLIGHT},
        [TT_I_TURN_RIGHT] = {&dash_img_tt_turn_right, TT_TURN_RIGHT_X, TT_TURN_RIGHT_Y, DASH_TT_TURN_RIGHT},
        [TT_I_BRAKE]      = {&dash_img_tt_brake,      TT_BRAKE_X,      TT_BRAKE_Y,      DASH_TT_BRAKE},
        [TT_I_WARNING]    = {&dash_img_tt_warning,    TT_WARNING_X,    TT_WARNING_Y,    DASH_TT_WARNING},
        [TT_I_ENGINE]     = {&dash_img_tt_engine,     TT_ENGINE_X,     TT_ENGINE_Y,     DASH_TT_CHECK_ENGINE},
        [TT_I_BATTERY]    = {&dash_img_tt_battery,    TT_BATTERY_X,    TT_BATTERY_Y,    DASH_TT_BATTERY},
    };

    for (int i = 0; i < TT_I_COUNT; i++) {
        ui.telltale[i]     = dash_sprite(scr, kTelltales[i].src, kTelltales[i].x, kTelltales[i].y);
        ui.telltale_bit[i] = kTelltales[i].bit;
    }
    /* High beam: the headlight lamp in blue. If this fails, high beam falls
       back to lighting the green lamp. */
    ui.high_beam = tinted_sprite(scr, &dash_img_tt_headlight, 0, 0,
                                 TT_HEADLIGHT_W, TT_HEADLIGHT_H,
                                 HIGH_BEAM_R, HIGH_BEAM_G, HIGH_BEAM_B,
                                 TT_HEADLIGHT_X, TT_HEADLIGHT_Y);
    ui.telltale_shown = 0;

    /* Low oil pressure: the "psi" caption and oil can above the pressure bar,
       lifted from the background and turned red, to lay over the amber ones. */
    ui.oil = tinted_sprite(scr, &dash_img_bg, OIL_ICON_X, OIL_ICON_Y,
                           OIL_ICON_W, OIL_ICON_H, 0xFF, 0x00, 0x00,
                           OIL_ICON_X, OIL_ICON_Y);
    ui.oil_shown = false;
    ui.high_beam_shown = false;

    ui.snow = dash_sprite(scr, &dash_img_tt_snow, TEMP_CX - TT_SNOW_W / 2, SNOW_Y);
    ui.snow_shown = false;

    return scr;
}

void dash_screen_cluster_refresh(const dash_signals_t *s)
{
    ui.speedo.shown += (s->speed_kmh - ui.speedo.shown) * SMOOTHING;
    ui.tacho.shown  += (s->rpm - ui.tacho.shown) * SMOOTHING;
    arc_set(&ui.speedo, ui.speedo.shown / SPEED_MAX);
    arc_set(&ui.tacho, ui.tacho.shown / RPM_MAX);
    arc_set_zone(&ui.tacho, rpm_zone(ui.tacho.shown, ui.tacho.zone));

    dash_value_set(&ui.speedo.readout, "%.0f", ui.speedo.shown);
    /* Quantised, because a tachometer counting single revs is unreadable. */
    dash_value_set(&ui.tacho.readout, "%.0f",
                   (float)((int)(ui.tacho.shown / RPM_STEP + 0.5f)) * RPM_STEP);

    dash_segbar_set(&ui.fuel, s->fuel_level_pct / 100.0f);
    dash_segbar_set(&ui.water, dash_norm(s->coolant_temp_c, TEMP_LO, TEMP_HI));
    dash_segbar_set(&ui.oil_temp, dash_norm(s->oil_temp_c, OILT_LO, OILT_HI));
    dash_segbar_set(&ui.oil_press, s->oil_pressure_psi / OILP_MAX);

    dash_value_set(&ui.trip, "%.1f", s->trip_km);
    dash_value_set(&ui.total, "%.0f", s->odometer_km);
    dash_value_set(&ui.turbo, "%.1f", s->turbo_psi);
    dash_value_set(&ui.ambient, "%.0f°C", s->ambient_temp_c);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", tm.tm_hour, tm.tm_min);
    dash_text_set(&ui.clock, hhmm);

    dash_show(ui.snow, s->ambient_temp_c < OAT_ICE_C, &ui.snow_shown);

    /* Low oil pressure has no lamp of its own. The pressure gauge goes red -
       bar and caption - and the general warning lamp lights with it. */
    const bool oil_alarm = (s->telltales & DASH_TT_OIL) != 0;
    if (oil_alarm != ui.oil_alarm) {
        ui.oil_alarm = oil_alarm;
        dash_segbar_set_alarm(&ui.oil_press, oil_alarm);
        if (ui.oil != NULL) {
            dash_show(ui.oil, oil_alarm, &ui.oil_shown);
        }
    }

    /* High beam has no lamp of its own in the artwork either: the headlight
       lamp shows in the conventional blue instead of green. */
    uint32_t want = s->telltales;
    if (oil_alarm) {
        want |= DASH_TT_WARNING;
    }
    const bool high_beam = (want & DASH_TT_HIGH_BEAM) != 0;
    if (ui.high_beam != NULL) {
        dash_show(ui.high_beam, high_beam, &ui.high_beam_shown);
        if (high_beam) {
            want &= ~(uint32_t)DASH_TT_HEADLIGHT;
        }
    } else if (high_beam) {
        want |= DASH_TT_HEADLIGHT;
    }

    for (int i = 0; i < TT_I_COUNT; i++) {
        const bool on = (want & ui.telltale_bit[i]) != 0;
        const bool was = (ui.telltale_shown & (1u << i)) != 0;
        if (on == was) {
            continue;
        }
        if (on) {
            ui.telltale_shown |= (1u << i);
            lv_obj_remove_flag(ui.telltale[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            ui.telltale_shown &= ~(1u << i);
            lv_obj_add_flag(ui.telltale[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}
