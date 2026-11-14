/*
 * Data screen: every signal in the model as text, for when you want numbers
 * rather than an impression.
 *
 * Speed and rpm keep bar graphs at the top because they are the two you glance
 * at; a stylised rule separates them from the tabular readouts below.
 */

#include <stdio.h>

#include "dash_ui_common.h"
#include "dash_ui_screens.h"

#define SPEED_MAX      160.0f
#define RPM_MAX_DISP   8000.0f
#define RPM_RED_DISP   7000.0f

#define BAR_X          330
#define BAR_W          1090
#define BAR_H          46
#define BAR_SEGS       40
#define BAR_GAP        3
#define BAR_SPEED_Y    64
#define BAR_RPM_Y      132

#define DIVIDER_Y      226

/* Three columns of label/value pairs. */
#define COL0_X         120
#define COL1_X         650
#define COL2_X         1160
#define ROW0_Y         300
#define ROW_DY         66
#define VALUE_DX       300

typedef struct {
    const char *label;
    value_t     value;
} field_t;

static struct {
    segbar_t speed_bar;
    segbar_t rpm_bar;
    value_t  speed_num;
    value_t  rpm_num;

    field_t  f[15];
    uint8_t  count;
} ui;

/* Indices into ui.f[], in the order they are laid out. */
enum {
    F_GEAR, F_COOLANT, F_OIL_T, F_OIL_P, F_FUEL,
    F_TURBO, F_TRIP, F_ODO, F_OAT, F_LAMPS,
    F_COUNT
};

static void add_field(lv_obj_t *scr, int idx, const char *label, int32_t x, int32_t y)
{
    lv_obj_t *l = dash_label_left(scr, x, y, &lv_font_montserrat_20, COL_DIM);
    lv_label_set_text(l, label);
    ui.f[idx].label = label;
    ui.f[idx].value.obj = dash_label_left(scr, x + VALUE_DX, y, &lv_font_montserrat_28, COL_WHITE);
    lv_obj_set_style_text_align(ui.f[idx].value.obj, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(ui.f[idx].value.obj, 160);
    lv_obj_set_pos(ui.f[idx].value.obj, x + VALUE_DX - 160, y - 18);
}

lv_obj_t *dash_screen_data_create(void)
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

    /* ---- bar graphs ---- */
    lv_obj_t *sl = dash_label_left(scr, 120, BAR_SPEED_Y + BAR_H / 2, &lv_font_montserrat_28, COL_DIM);
    lv_label_set_text(sl, "SPEED");
    dash_segbar_create(&ui.speed_bar, scr, BAR_X, BAR_SPEED_Y, BAR_W, BAR_H,
                       BAR_SEGS, false, BAR_GAP, 0, false);
    ui.speed_num.obj = dash_label(scr, 1500, BAR_SPEED_Y + BAR_H / 2, 180,
                                  &lv_font_montserrat_28, COL_WHITE);

    lv_obj_t *rl = dash_label_left(scr, 120, BAR_RPM_Y + BAR_H / 2, &lv_font_montserrat_28, COL_DIM);
    lv_label_set_text(rl, "RPM");
    dash_segbar_create(&ui.rpm_bar, scr, BAR_X, BAR_RPM_Y, BAR_W, BAR_H,
                       BAR_SEGS, false, BAR_GAP,
                       (uint8_t)(BAR_SEGS * (1.0f - RPM_RED_DISP / RPM_MAX_DISP) + 0.5f),
                       false);
    ui.rpm_num.obj = dash_label(scr, 1500, BAR_RPM_Y + BAR_H / 2, 180,
                                &lv_font_montserrat_28, COL_WHITE);

    /* ---- stylised divider: a bright centre fading out to a hairline ---- */
    dash_rect(scr, 120, DIVIDER_Y, 1360, 3, COL_RULE);
    dash_rect(scr, 120, DIVIDER_Y + 6, 1360, 1, lv_color_hex(0x3A2E18));
    /* A short bright accent at the left, so the rule reads as deliberate. */
    dash_rect(scr, 120, DIVIDER_Y - 3, 220, 9, COL_AMBER);

    /* ---- tabular readouts ---- */
    add_field(scr, F_GEAR,    "GEAR",          COL0_X, ROW0_Y);
    add_field(scr, F_COOLANT, "COOLANT",       COL0_X, ROW0_Y + ROW_DY);
    add_field(scr, F_OIL_T,   "OIL TEMP",      COL0_X, ROW0_Y + ROW_DY * 2);
    add_field(scr, F_OIL_P,   "OIL PRESSURE",  COL0_X, ROW0_Y + ROW_DY * 3);

    add_field(scr, F_FUEL,    "FUEL",          COL1_X, ROW0_Y);
    add_field(scr, F_TURBO,   "BOOST",         COL1_X, ROW0_Y + ROW_DY);
    add_field(scr, F_TRIP,    "TRIP",          COL1_X, ROW0_Y + ROW_DY * 2);
    add_field(scr, F_ODO,     "ODOMETER",      COL1_X, ROW0_Y + ROW_DY * 3);

    add_field(scr, F_OAT,     "OUTSIDE AIR",   COL2_X, ROW0_Y);
    add_field(scr, F_LAMPS,   "LAMPS",         COL2_X, ROW0_Y + ROW_DY);

    ui.count = F_COUNT;
    return scr;
}

/* Compact list of whichever warning lamps are lit. */
static void format_lamps(char *out, size_t n, uint32_t tt)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } kNames[] = {
        {DASH_TT_CHECK_ENGINE, "ENG"},
        {DASH_TT_BATTERY,      "BAT"},
        {DASH_TT_WARNING,      "!"},
        {DASH_TT_BRAKE,        "BRK"},
        {DASH_TT_OIL,          "OIL"},
        {DASH_TT_TURN_LEFT,    "<"},
        {DASH_TT_TURN_RIGHT,   ">"},
        {DASH_TT_HEADLIGHT,    "LIGHTS"},
        {DASH_TT_HIGH_BEAM,    "HIGH"},
        {DASH_TT_LOW_TEMP,     "ICE"},
    };

    out[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        if ((tt & kNames[i].bit) == 0) {
            continue;
        }
        const int w = snprintf(out + used, n - used, "%s%s", used ? " " : "", kNames[i].name);
        if (w < 0 || (size_t)w >= n - used) {
            break;
        }
        used += (size_t)w;
    }
    if (used == 0) {
        snprintf(out, n, "--");
    }
}

void dash_screen_data_refresh(const dash_signals_t *s)
{
    dash_segbar_set(&ui.speed_bar, s->speed_kmh / SPEED_MAX);
    dash_segbar_set(&ui.rpm_bar, s->rpm / RPM_MAX_DISP);
    dash_value_set(&ui.speed_num, "%.0f km/h", s->speed_kmh);
    dash_value_set(&ui.rpm_num, "%.0f", (float)((int)(s->rpm / 50.0f + 0.5f)) * 50.0f);

    dash_value_set(&ui.f[F_GEAR].value,    "%.0f",     (float)s->gear);
    dash_value_set(&ui.f[F_COOLANT].value, "%.0f°C",   s->coolant_temp_c);
    dash_value_set(&ui.f[F_OIL_T].value,   "%.0f°C",   s->oil_temp_c);
    dash_value_set(&ui.f[F_OIL_P].value,   "%.0f psi", s->oil_pressure_psi);
    dash_value_set(&ui.f[F_FUEL].value,    "%.0f%%",   s->fuel_level_pct);
    dash_value_set(&ui.f[F_TURBO].value,   "%.1f psi", s->turbo_psi);
    dash_value_set(&ui.f[F_TRIP].value,    "%.1f km",  s->trip_km);
    dash_value_set(&ui.f[F_ODO].value,     "%.0f km",  s->odometer_km);
    dash_value_set(&ui.f[F_OAT].value,     "%.0f°C",   s->ambient_temp_c);

    char lamps[24];
    format_lamps(lamps, sizeof(lamps), s->telltales);
    dash_text_set(&ui.f[F_LAMPS].value, lamps);
}
