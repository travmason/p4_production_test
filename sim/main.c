/*
 * Dashboard simulator for the Mac.
 *
 * Runs the firmware's own UI code - dash_ui, dash_assets, dash_signals and the
 * dash_source_sim drive cycle, all compiled unmodified - in an SDL window, so
 * the graphics can be seen and changed without the board.
 *
 * What it does not reproduce: the board's timing. The DSI bridge, PSRAM
 * bandwidth and CPU speed are what decide whether the real panel underruns or
 * drops frames, and none of that exists here. Use this for layout, colours and
 * behaviour; use the board for performance.
 *
 *   dash_sim                          open a window
 *   dash_sim --zoom 0.5               ... at half size
 *   dash_sim --screen 1               ... starting on the second screen
 *   dash_sim --screenshot out.bmp     no window: run for a while, save, exit
 *            --after 20               ... after this many seconds (default 5)
 *
 * Keys: Space / Right / Down  next screen      1 2 3   pick a screen
 *       Left / Up             previous screen  Esc Q   quit
 * Dragging up or down with the mouse swipes, as a finger would on the panel.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <SDL.h>

#include "dash_signals.h"
#include "dash_source_sim.h"
#include "dash_ui.h"
#include "lvgl.h"

#define PANEL_W 1600
#define PANEL_H 720

typedef struct {
    float       zoom;          /* 0 = choose one that fits the desktop */
    int         screen;
    const char *screenshot;    /* NULL = interactive */
    float       after_s;
} options_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [--zoom F] [--screen N] [--screenshot FILE.bmp [--after SECONDS]]\n"
            "\n"
            "  --zoom F            window scale, e.g. 0.5 (default: largest that fits)\n"
            "  --screen N          start on screen N (0 cluster, 1 track, 2 data)\n"
            "  --screenshot FILE   run without a window, save a BMP, then exit\n"
            "  --after SECONDS     how long to run before the screenshot (default 5)\n"
            "\n"
            "keys: Space/Right/Down next, Left/Up previous, 1-3 select, Esc/Q quit\n",
            argv0);
}

static bool parse_options(int argc, char **argv, options_t *o)
{
    *o = (options_t) { .zoom = 0.0f, .screen = 0, .screenshot = NULL, .after_s = 5.0f };

    for (int i = 1; i < argc; i++) {
        const bool has_value = i + 1 < argc;
        if (strcmp(argv[i], "--zoom") == 0 && has_value) {
            o->zoom = strtof(argv[++i], NULL);
        } else if (strcmp(argv[i], "--screen") == 0 && has_value) {
            o->screen = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot") == 0 && has_value) {
            o->screenshot = argv[++i];
        } else if (strcmp(argv[i], "--after") == 0 && has_value) {
            o->after_s = strtof(argv[++i], NULL);
        } else {
            return false;
        }
    }
    return o->zoom >= 0.0f && o->after_s >= 0.0f;
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ------------------------------------------------------------------------- *
 * Headless display, for --screenshot
 * ------------------------------------------------------------------------- */

static void headless_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static lv_display_t *headless_display_create(void)
{
    const size_t size = (size_t)PANEL_W * PANEL_H * 3;
    uint8_t *fb = malloc(size);
    if (fb == NULL) {
        return NULL;
    }
    memset(fb, 0, size);

    lv_tick_set_cb(now_ms);
    lv_display_t *disp = lv_display_create(PANEL_W, PANEL_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(disp, fb, NULL, (uint32_t)size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, headless_flush);
    return disp;
}

/* 24-bit BMP. LVGL's RGB888 is stored blue-first, which is what BMP wants; the
   only differences are that BMP rows run bottom-up and are padded to 4 bytes. */
static bool save_bmp(const char *path, const lv_draw_buf_t *img)
{
    const uint32_t w = img->header.w;
    const uint32_t h = img->header.h;
    const uint32_t row = (w * 3 + 3) & ~3u;
    const uint32_t data_size = row * h;

    uint8_t hdr[54] = {0};
    hdr[0] = 'B';
    hdr[1] = 'M';
    const uint32_t file_size = sizeof(hdr) + data_size;
    memcpy(&hdr[2], &file_size, 4);
    hdr[10] = sizeof(hdr);
    hdr[14] = 40;
    memcpy(&hdr[18], &w, 4);
    memcpy(&hdr[22], &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    memcpy(&hdr[34], &data_size, 4);

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);

    static const uint8_t pad[3] = {0};
    for (uint32_t y = h; ok && y-- > 0;) {
        const uint8_t *src = img->data + (size_t)y * img->header.stride;
        ok = fwrite(src, 1, w * 3, f) == w * 3 &&
             fwrite(pad, 1, row - w * 3, f) == row - w * 3;
    }
    return fclose(f) == 0 && ok;
}

static int run_screenshot(const options_t *o)
{
    const uint32_t end = now_ms() + (uint32_t)(o->after_s * 1000.0f);
    while ((int32_t)(end - now_ms()) > 0) {
        lv_timer_handler();
        usleep(5000);
    }

    lv_draw_buf_t *shot = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (shot == NULL) {
        fprintf(stderr, "snapshot failed\n");
        return 1;
    }
    const bool ok = save_bmp(o->screenshot, shot);
    lv_draw_buf_destroy(shot);
    if (!ok) {
        fprintf(stderr, "could not write %s\n", o->screenshot);
        return 1;
    }
    printf("wrote %s (%dx%d) after %.1f s\n", o->screenshot, PANEL_W, PANEL_H, o->after_s);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Interactive window
 * ------------------------------------------------------------------------- */

/* The panel is 1600 wide, which is wider than many laptop desktops. */
static float fit_zoom(void)
{
    SDL_Rect r;
    if (SDL_GetDisplayUsableBounds(0, &r) != 0) {
        return 1.0f;
    }
    for (int tenths = 10; tenths > 3; tenths--) {
        const float z = tenths / 10.0f;
        if (PANEL_W * z <= r.w - 16 && PANEL_H * z <= r.h - 48) {
            return z;
        }
    }
    return 0.3f;
}

/*
 * Keyboard.
 *
 * LVGL's SDL driver drains the SDL event queue itself, so key presses are read
 * here from SDL's keyboard state and turned into edges by hand.
 */
static bool handle_keys(void)
{
    static const struct {
        SDL_Scancode key;
        int          action;   /* -1 previous, -2 next, -3 quit, >= 0 select */
    } kMap[] = {
        {SDL_SCANCODE_SPACE, -2}, {SDL_SCANCODE_RIGHT, -2}, {SDL_SCANCODE_DOWN, -2},
        {SDL_SCANCODE_LEFT, -1},  {SDL_SCANCODE_UP, -1},
        {SDL_SCANCODE_1, 0},      {SDL_SCANCODE_2, 1},      {SDL_SCANCODE_3, 2},
        {SDL_SCANCODE_ESCAPE, -3}, {SDL_SCANCODE_Q, -3},
    };
    enum { N = sizeof(kMap) / sizeof(kMap[0]) };
    static bool was_down[N];

    const Uint8 *state = SDL_GetKeyboardState(NULL);
    for (int i = 0; i < N; i++) {
        const bool down = state[kMap[i].key] != 0;
        const bool pressed = down && !was_down[i];
        was_down[i] = down;
        if (!pressed) {
            continue;
        }
        switch (kMap[i].action) {
        case -1: dash_ui_prev_screen(); break;
        case -2: dash_ui_next_screen(); break;
        case -3: return false;
        default: dash_ui_set_screen(kMap[i].action); break;
        }
    }
    return true;
}

static int run_window(void)
{
    while (handle_keys()) {
        uint32_t wait = lv_timer_handler();
        if (wait > 10) {
            wait = 10;
        }
        usleep(wait * 1000u);
    }
    lv_sdl_quit();
    return 0;
}

int main(int argc, char **argv)
{
    options_t o;
    if (!parse_options(argc, argv, &o)) {
        usage(argv[0]);
        return 2;
    }

    lv_init();

    lv_display_t *disp;
    if (o.screenshot != NULL) {
        disp = headless_display_create();
    } else {
        disp = lv_sdl_window_create(PANEL_W, PANEL_H);
        if (disp != NULL) {
            lv_sdl_window_set_title(disp, "Dashboard simulator  -  Space: next   1-3: select   Esc: quit");
            lv_sdl_window_set_zoom(disp, o.zoom > 0.0f ? o.zoom : fit_zoom());
            lv_sdl_mouse_create();
        }
    }
    if (disp == NULL) {
        fprintf(stderr, "could not create the display\n");
        return 1;
    }
    lv_display_set_default(disp);

    /* From here on this is the firmware's own start-up sequence. */
    if (dash_signals_init() != ESP_OK || dash_ui_create(disp) != ESP_OK ||
        dash_source_sim_start() != ESP_OK) {
        fprintf(stderr, "dashboard start-up failed\n");
        return 1;
    }
    if (o.screen != 0) {
        dash_ui_set_screen(o.screen);
    }

    return o.screenshot != NULL ? run_screenshot(&o) : run_window();
}
