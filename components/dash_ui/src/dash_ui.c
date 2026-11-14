#include "dash_ui.h"

#include <stdatomic.h>

#include "dash_signals.h"
#include "dash_ui_common.h"
#include "dash_ui_screens.h"
#include "esp_log.h"

static const char *TAG = "dash_ui";

/*
 * Screen manager.
 *
 * Three screens, navigated by swiping up and down. Only the visible one is
 * refreshed - the others keep their widgets but cost nothing until shown.
 */

typedef struct {
    const char *name;
    lv_obj_t *(*create)(void);
    void (*refresh)(const dash_signals_t *s);
    lv_obj_t *screen;
} screen_def_t;

static screen_def_t s_screens[] = {
    {"cluster", dash_screen_cluster_create, dash_screen_cluster_refresh, NULL},
    {"track",   dash_screen_track_create,   dash_screen_track_refresh,   NULL},
    {"data",    dash_screen_data_create,    dash_screen_data_refresh,    NULL},
};
#define NUM_SCREENS ((int)(sizeof(s_screens) / sizeof(s_screens[0])))

static int s_active;

/*
 * Screen changes are animated, but kept brief. A full-screen transition is the
 * single most expensive thing this UI does - it composites two 1600x720 screens
 * - so a long one risks starving the DSI bridge.
 */
#define SWITCH_MS  220

static void gesture_cb(lv_event_t *e);

/*
 * Screens are built the first time they are shown, not up front.
 *
 * Every widget costs LVGL pool memory and object-tree walking whether or not it
 * is on screen, and building all three at boot was enough to exhaust lv_malloc
 * and wedge the draw dispatcher. Most sessions only ever look at one screen.
 */
static lv_obj_t *screen_get(int idx)
{
    if (s_screens[idx].screen == NULL) {
        s_screens[idx].screen = s_screens[idx].create();
        if (s_screens[idx].screen == NULL) {
            ESP_LOGE(TAG, "failed to build the %s screen", s_screens[idx].name);
            return NULL;
        }
        /* Gestures are delivered to the screen under the finger, so every
           screen needs its own handler. */
        lv_obj_add_event_cb(s_screens[idx].screen, gesture_cb, LV_EVENT_GESTURE, NULL);
        ESP_LOGI(TAG, "built the %s screen", s_screens[idx].name);
    }
    return s_screens[idx].screen;
}

static void switch_to(int idx, bool upward)
{
    if (idx < 0) {
        idx = NUM_SCREENS - 1;
    } else if (idx >= NUM_SCREENS) {
        idx = 0;
    }
    if (idx == s_active) {
        return;
    }

    lv_obj_t *scr = screen_get(idx);
    if (scr == NULL) {
        return;
    }

    s_active = idx;
    lv_screen_load_anim(scr,
                        upward ? LV_SCR_LOAD_ANIM_MOVE_TOP : LV_SCR_LOAD_ANIM_MOVE_BOTTOM,
                        SWITCH_MS, 0, false);
    ESP_LOGI(TAG, "screen -> %s", s_screens[idx].name);
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) {
        return;
    }

    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_TOP:      /* swipe up   - next screen  */
        switch_to(s_active + 1, true);
        break;
    case LV_DIR_BOTTOM:   /* swipe down - previous     */
        switch_to(s_active - 1, false);
        break;
    default:
        break;
    }
}

/*
 * Screen-change requests from other tasks.
 *
 * The button and CAN tasks do not touch LVGL themselves. They leave a request
 * here and the refresh timer, which already runs in the LVGL task with the
 * lock held, carries it out. That keeps a first-time screen build - a hundred
 * or more objects - off those tasks' small stacks, and means a press is never
 * lost because the display lock happened to be busy.
 */
static atomic_int s_req_step;        /* net next (+) / previous (-) not yet applied */
static atomic_int s_req_abs = -1;    /* screen selected outright, or -1 for none */

static void apply_requests(void)
{
    const int abs  = atomic_exchange(&s_req_abs, -1);
    const int step = atomic_exchange(&s_req_step, 0);

    if (abs >= 0 && abs < NUM_SCREENS) {
        /* Animate in whichever direction the index moved. */
        switch_to(abs, abs > s_active);
    }
    if (step != 0) {
        int idx = (s_active + step) % NUM_SCREENS;
        if (idx < 0) {
            idx += NUM_SCREENS;
        }
        switch_to(idx, step > 0);
    }
}

/*
 * Source chip. Lives on the top layer so it sits over every screen and rides
 * through screen-change animations without being rebuilt.
 */
#define CHIP_X       10
#define CHIP_Y       8

static atomic_int s_chip_req = DASH_UI_CHIP_SIM;
static atomic_bool s_gps_warn;
static int        s_chip_shown = -1;
static lv_obj_t  *s_chip;
static void     (*s_chip_cb)(void);

static void chip_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_chip_cb != NULL) {
        s_chip_cb();
    }
}

static void chip_create(void)
{
    s_chip = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_font(s_chip, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_hor(s_chip, 12, 0);
    lv_obj_set_style_pad_ver(s_chip, 4, 0);
    lv_obj_set_style_radius(s_chip, 6, 0);
    lv_obj_set_style_border_width(s_chip, 2, 0);
    /* Top left is the one corner all three screens leave empty. */
    lv_obj_align(s_chip, LV_ALIGN_TOP_LEFT, CHIP_X, CHIP_Y);
    /* A fingertip is far bigger than the label. */
    lv_obj_set_ext_click_area(s_chip, 28);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_chip, chip_event_cb, LV_EVENT_LONG_PRESSED, NULL);
}

static void chip_refresh(void)
{
    const int  state = atomic_load(&s_chip_req);
    const bool no_gps = state != DASH_UI_CHIP_SIM && atomic_load(&s_gps_warn);
    const int  want = state | (no_gps ? 0x100 : 0);
    if (want == s_chip_shown || s_chip == NULL) {
        return;
    }
    s_chip_shown = want;

    const char *text;
    lv_color_t  fg, bg, border;
    lv_opa_t    opa = LV_OPA_COVER;

    switch (state) {
    case DASH_UI_CHIP_LIVE:
        if (no_gps) {
            text = "NO GPS";
            fg = COL_WHITE;
            bg = lv_color_hex(0xC00000);
            border = bg;
            break;
        }
        /* Nothing to say when the feed is real and healthy. The label keeps
           its size and stays touchable, just invisible, so holding the same
           corner still steps the source. */
        text = "LIVE";
        fg = bg = border = lv_color_black();
        opa = LV_OPA_TRANSP;
        break;
    case DASH_UI_CHIP_NO_ECU:
        text = no_gps ? "NO ECU / GPS" : "NO ECU";
        fg = COL_WHITE;
        bg = lv_color_hex(0xC00000);
        border = bg;
        break;
    default:
        text = "SIM";
        fg = lv_color_black();
        bg = COL_AMBER;
        border = bg;
        break;
    }
    lv_label_set_text(s_chip, text);
    lv_obj_set_style_text_color(s_chip, fg, 0);
    lv_obj_set_style_bg_color(s_chip, bg, 0);
    lv_obj_set_style_bg_opa(s_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_chip, opa, 0);
    lv_obj_set_style_border_color(s_chip, border, 0);
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    apply_requests();
    chip_refresh();

    dash_signals_t s;
    dash_signals_get(&s);
    s_screens[s_active].refresh(&s);
}

esp_err_t dash_ui_create(lv_display_t *disp)
{
    if (disp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    lv_obj_t *first = screen_get(0);
    if (first == NULL) {
        return ESP_FAIL;
    }

    s_active = 0;
    lv_screen_load(first);
    chip_create();
    chip_refresh();

    lv_timer_create(refresh_cb, 50, NULL);

    ESP_LOGI(TAG, "%d screens available, swipe up/down to change", NUM_SCREENS);
    return ESP_OK;
}

void dash_ui_next_screen(void)
{
    atomic_fetch_add(&s_req_step, 1);
}

void dash_ui_prev_screen(void)
{
    atomic_fetch_sub(&s_req_step, 1);
}

void dash_ui_set_screen(int index)
{
    if (index < 0 || index >= NUM_SCREENS) {
        return;
    }
    atomic_store(&s_req_abs, index);
}

void dash_ui_set_source_chip(dash_ui_source_chip_t state)
{
    atomic_store(&s_chip_req, (int)state);
}

void dash_ui_set_gps_warning(bool no_gps)
{
    atomic_store(&s_gps_warn, no_gps);
}

void dash_ui_set_source_chip_cb(void (*on_hold)(void))
{
    s_chip_cb = on_hold;
}

int dash_ui_screen_count(void)
{
    return NUM_SCREENS;
}
