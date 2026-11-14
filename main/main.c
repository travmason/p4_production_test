/*
 * ESP32-P4-PC -> HDMI (LT8912B) -> 1600x720 bar panel
 *
 * Car instrument cluster. Three pieces, deliberately kept apart:
 *
 *   dash_signals     the vehicle signal model - the seam between data and pixels
 *   dash_source_sim  a synthetic vehicle feeding that model (swap for CAN later)
 *   dash_ui          the LVGL cluster, which only ever reads dash_signals
 *
 * The original Olimex production test is preserved in production_test_main.c.ref
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "dash_can.h"
#include "dash_signals.h"
#include "dash_source_sim.h"
#include "dash_touch.h"
#include "dash_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

static const char *TAG = "dash";

/*
 * Give the clock a plausible starting point.
 *
 * The board has no battery-backed RTC and nothing is syncing time, so the
 * system clock would otherwise start at the epoch and the corner would read
 * 00:00 in 1970. Seeding from the build timestamp at least starts it near the
 * right time of day. A real installation should take the time from GPS, CAN,
 * or SNTP over the onboard Ethernet, at which point this can go.
 */
static void seed_clock_from_build_time(void)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";

    char month[4] = {0};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0;

    if (sscanf(__DATE__, "%3s %d %d", month, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &min, &sec) != 3) {
        return;
    }

    const char *pos = strstr(months, month);
    if (pos == NULL) {
        return;
    }

    struct tm tm = {
        .tm_year = year - 1900,
        .tm_mon  = (int)(pos - months) / 3,
        .tm_mday = day,
        .tm_hour = hour,
        .tm_min  = min,
        .tm_sec  = sec,
        .tm_isdst = -1,
    };

    const struct timeval tv = { .tv_sec = mktime(&tm), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "clock seeded from build time: %s %s", __DATE__, __TIME__);
}

/*
 * Buttons that change screen.
 *
 * Two inputs, both active low with an internal pull-up, so a button simply
 * shorts the pin to GND:
 *
 *   BOOT (GPIO35)  the on-board button
 *   GPIO14         EXT1 pin 8, for a panel-mounted button. GND is EXT1 pin 3
 *                  and 3.3 V is pin 1.
 *
 * GPIO14 is one of eleven unassigned pins on the EXT1 header and is not a
 * strapping pin - the P4's strapping pins are GPIO34-38, and GPIO35 is already
 * the BOOT button. Nothing else on the board claims it.
 */
#define BOOT_BTN_GPIO  GPIO_NUM_35
#define EXT_BTN_GPIO   GPIO_NUM_14

#define BTN_DEBOUNCE_MS  40

static void button_task(void *arg)
{
    (void)arg;

    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOOT_BTN_GPIO) | (1ULL << EXT_BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    ESP_LOGI(TAG, "screen buttons: BOOT (GPIO%d) and GPIO%d (EXT1 pin 8)",
             BOOT_BTN_GPIO, EXT_BTN_GPIO);

    bool prev_boot = false, prev_ext = false;
    while (true) {
        const bool boot = gpio_get_level(BOOT_BTN_GPIO) == 0;   /* active low */
        const bool ext  = gpio_get_level(EXT_BTN_GPIO) == 0;

        /* Either button advances, on the press edge only. This only posts a
           request; the LVGL task makes the change, so no display lock here. */
        if ((boot && !prev_boot) || (ext && !prev_ext)) {
            dash_ui_next_screen();
        }
        prev_boot = boot;
        prev_ext  = ext;
        vTaskDelay(pdMS_TO_TICKS(BTN_DEBOUNCE_MS));
    }
}

void app_main(void)
{
    seed_clock_from_build_time();

    ESP_LOGI(TAG, "PSRAM free: %u KB, internal free: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    ESP_ERROR_CHECK(dash_signals_init());

    /*
     * Explicit display config rather than bsp_display_start()'s defaults.
     * Those enable software rotation, which makes esp_lvgl_port allocate a
     * third full draw buffer - and with an RGB565 frame buffer it asks for that
     * one to be DMA-capable internal RAM, which does not fit here. The cluster
     * never rotates, so the whole buffer is waste.
     */
    bsp_display_cfg_t disp_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        /* Rendering is the heaviest thing on the board. Pin it to core 1 so it
           does not compete with the DSI interrupt, the USB host stack and the
           signal source, which all sit on core 0. Set below, since the macro
           above initialises the whole struct. */
        /* One 1600x40 RGB565 draw buffer is 128 KB of DMA-capable internal RAM,
           out of roughly 337 KB free. A second one does not fit. */
        .buffer_size   = BSP_LCD_H_RES * 40,
        .double_buffer = false,
        .hw_cfg = {
            .hdmi_resolution = BSP_HDMI_RES_1600x720,
            .dsi_bus = {
                .phy_clk_src        = 0,
                .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
            },
        },
        .flags = {
            /* esp_lvgl_port only accepts DMA draw buffers for RGB565; with an
               RGB888 buffer its copy path is not alignment-safe. */
#if CONFIG_BSP_LCD_COLOR_FORMAT_RGB888
            .buff_dma    = false,
#else
            .buff_dma    = true,
#endif
            .buff_spiram = false,
            .sw_rotate   = false,
        },
    };

    disp_cfg.lvgl_port_cfg.task_affinity = 1;

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start() failed - check HDMI cable and LT8912B I2C");
        return;
    }

    /* No bsp_display_backlight_on() here: on HDMI the panel runs its own
       backlight, and BSP_LCD_BACKLIGHT (GPIO23) is not the panel's. */

    if (bsp_display_lock(portMAX_DELAY)) {
        lv_display_set_default(disp);
        dash_ui_create(disp);
        bsp_display_unlock();
    }

    /* Touch is optional: the cluster runs fine without a digitizer attached,
       it just cannot be swiped. */
    if (dash_touch_start(disp) != ESP_OK) {
        ESP_LOGW(TAG, "touch input unavailable");
    }

    xTaskCreate(button_task, "screen_btn", 3072, NULL, 3, NULL);

    /* CAN. Needs an external 3.3V transceiver on the EXT1 header - see
       dash_can.h. Failing to start is not fatal; the dash runs regardless. */
    const dash_can_cfg_t can_cfg = DASH_CAN_DEFAULT_CFG();
    if (dash_can_start(&can_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "CAN unavailable; screens still change on the buttons");
    }

    ESP_ERROR_CHECK(dash_source_sim_start());

    ESP_LOGI(TAG, "cluster running at %" LV_PRId32 "x%" LV_PRId32,
             lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));

    /* Periodic health report; the UI refreshes itself on an LVGL timer. */
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "internal free: %u KB, largest block: %u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    }
}
