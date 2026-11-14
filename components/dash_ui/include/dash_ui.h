/*
 * dash_ui - the dashboard screens.
 *
 * Builds the first screen and starts refreshing whichever is on show; the other
 * screens are built the first time they are selected. Screens are navigated by
 * swiping up and down; dash_ui_next_screen() does the same thing without touch,
 * which is useful when no digitizer is attached.
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

/*
 * The source chip: a small label in the top-left corner of every screen
 * that appears whenever the numbers are not a healthy live feed, so simulated
 * data can never be mistaken for the car's. With the ECU feeding the gauges it
 * shows nothing, but the corner still responds to a press and hold.
 */
typedef enum {
    DASH_UI_CHIP_SIM = 0,   /*!< amber "SIM" */
    DASH_UI_CHIP_LIVE,      /*!< nothing shown */
    DASH_UI_CHIP_NO_ECU,    /*!< red "NO ECU" */
} dash_ui_source_chip_t;

/** Set what the chip shows. Safe from any task; do not hold the LVGL lock. */
void dash_ui_set_source_chip(dash_ui_source_chip_t state);

/**
 * Called, in the LVGL task, when the chip is pressed and held. A hold rather
 * than a tap so that a brushed finger changes nothing. The callback must not
 * block; posting a request to another task is the intended use.
 */
void dash_ui_set_source_chip_cb(void (*on_hold)(void));

#ifdef __cplusplus
}
#endif
