/*
 * dash_ui - the dashboard screens.
 *
 * Builds every screen and starts refreshing whichever is on show. Screens are
 * navigated by swiping up and down; dash_ui_next_screen() does the same thing
 * without touch, which is useful when no digitizer is attached.
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build the first screen and start the refresh timer.
 *
 * Must be called with the LVGL lock held (bsp_display_lock).
 */
esp_err_t dash_ui_create(lv_display_t *disp);

/*
 * The three screen-change calls below are safe from any task and must NOT be
 * wrapped in the LVGL lock: they only post a request, which the UI's refresh
 * timer carries out in the LVGL task within one tick (50 ms).
 */

/** Advance to the next screen, as a swipe up would. Wraps around. */
void dash_ui_next_screen(void);

/** Go back one screen, as a swipe down would. Wraps around. */
void dash_ui_prev_screen(void);

/** Select a screen outright. Out-of-range indices are ignored. */
void dash_ui_set_screen(int index);

/** How many screens exist, for validating an external selector. */
int dash_ui_screen_count(void);

#ifdef __cplusplus
}
#endif
