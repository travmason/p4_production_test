/*
 * dash_touch - the panel's capacitive touch, over USB.
 *
 * The Waveshare 10.4HP-CAPQLED reports touch as a USB HID digitizer on its
 * Type-C port, not over I2C, so this brings up USB host + HID and presents the
 * result to LVGL as a pointer input device.
 */

#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start USB host, watch for a HID digitizer, and register an LVGL pointer.
 *
 * Safe to call before the panel is plugged in; the device is picked up
 * whenever it appears. Returns once the USB task is running, not once a
 * device is found.
 */
esp_err_t dash_touch_start(lv_display_t *disp);

/** True once a digitizer has been found and its report layout decoded. */
bool dash_touch_is_connected(void);

#ifdef __cplusplus
}
#endif
