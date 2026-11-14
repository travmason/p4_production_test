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

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    apply_requests();

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

int dash_ui_screen_count(void)
{
    return NUM_SCREENS;
}
