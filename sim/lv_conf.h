/*
 * LVGL configuration for the Mac simulator.
 *
 * Only what differs from LVGL's defaults is set here. The rendering settings
 * mirror the firmware's (sdkconfig.defaults plus the LVGL Kconfig defaults) so
 * the picture matches the board: 24-bit colour, the same built-in fonts, the
 * same refresh period, no image or shadow caches.
 */
#ifndef LV_CONF_H
#define LV_CONF_H

/* ---- same as the firmware ---- */
#define LV_COLOR_DEPTH                24
#define LV_USE_STDLIB_MALLOC          LV_STDLIB_CLIB
#define LV_DEF_REFR_PERIOD            33
#define LV_DPI_DEF                    130
#define LV_USE_OS                     LV_OS_NONE
#define LV_DRAW_SW_SHADOW_CACHE_SIZE  0
#define LV_CACHE_DEF_SIZE             0
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 0

#define LV_FONT_MONTSERRAT_14         1
#define LV_FONT_MONTSERRAT_20         1
#define LV_FONT_MONTSERRAT_28         1
#define LV_FONT_MONTSERRAT_48         1
#define LV_USE_FONT_PLACEHOLDER       1

/* ---- simulator only ---- */
#define LV_USE_SDL                    1
#define LV_SDL_INCLUDE_PATH           <SDL.h>
#define LV_SDL_RENDER_MODE            LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT              1
#define LV_SDL_ACCELERATED            1
#define LV_SDL_FULLSCREEN             0
#define LV_SDL_DIRECT_EXIT            1
#define LV_SDL_MOUSEWHEEL_MODE        LV_SDL_MOUSEWHEEL_MODE_ENCODER

/* For --screenshot. */
#define LV_USE_SNAPSHOT               1

#define LV_USE_LOG                    1
#define LV_LOG_LEVEL                  LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF                 1

#endif /* LV_CONF_H */
