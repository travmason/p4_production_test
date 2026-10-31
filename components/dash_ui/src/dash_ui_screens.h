/*
 * One create + refresh pair per screen. Each builds its own LVGL screen object;
 * the manager in dash_ui.c owns them and refreshes only the visible one.
 */

#pragma once

#include "dash_signals.h"
#include "lvgl.h"

lv_obj_t *dash_screen_cluster_create(void);
void      dash_screen_cluster_refresh(const dash_signals_t *s);

lv_obj_t *dash_screen_track_create(void);
void      dash_screen_track_refresh(const dash_signals_t *s);

lv_obj_t *dash_screen_data_create(void);
void      dash_screen_data_refresh(const dash_signals_t *s);
