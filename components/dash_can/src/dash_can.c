#include "dash_can.h"

#include "bsp/esp-bsp.h"
#include "dash_ui.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash_can";

static dash_can_cfg_t s_cfg;
static volatile uint32_t s_rx_count;

static bool timing_for(uint32_t bps, twai_timing_config_t *out)
{
    switch (bps) {
    case 1000000: *out = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();   return true;
    case 500000:  *out = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); return true;
    case 250000:  *out = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); return true;
    case 125000:  *out = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); return true;
    default:      return false;
    }
}

/* Screen commands touch LVGL objects, so they run under the display lock. */
static void handle_screen_cmd(const twai_message_t *msg)
{
    if (msg->data_length_code < 1) {
        return;
    }
    const uint8_t cmd = msg->data[0];

    if (!bsp_display_lock(200)) {
        ESP_LOGW(TAG, "dropped screen command 0x%02x: display busy", cmd);
        return;
    }

    if (cmd == DASH_CAN_SCREEN_NEXT) {
        dash_ui_next_screen();
    } else if (cmd == DASH_CAN_SCREEN_PREV) {
        dash_ui_prev_screen();
    } else if (cmd >= DASH_CAN_SCREEN_ABS_BASE) {
        dash_ui_set_screen(cmd - DASH_CAN_SCREEN_ABS_BASE);
    }

    bsp_display_unlock();
}

static void can_task(void *arg)
{
    (void)arg;

    while (true) {
        twai_message_t msg;
        /* Long timeout rather than portMAX_DELAY so a bus-off state still gets
           noticed and reported below. */
        const esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(1000));
        if (err == ESP_OK) {
            s_rx_count++;
            if (msg.identifier == s_cfg.screen_cmd_id && !msg.rtr) {
                handle_screen_cmd(&msg);
            }
            /* Vehicle signal decoding belongs here: match msg.identifier
               against the frame map and push values into dash_signals. */
            continue;
        }
        if (err != ESP_ERR_TIMEOUT) {
            continue;
        }

        /* Recover from a bus-off, which happens if the bus is unterminated or
           the transceiver is missing while we are not in listen-only mode. */
        twai_status_info_t st;
        if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF) {
            ESP_LOGW(TAG, "bus-off, recovering");
            twai_initiate_recovery();
            vTaskDelay(pdMS_TO_TICKS(100));
            twai_start();
        }
    }
}

esp_err_t dash_can_start(const dash_can_cfg_t *cfg)
{
    if (cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;

    twai_timing_config_t t_cfg;
    if (!timing_for(s_cfg.bitrate_bps, &t_cfg)) {
        ESP_LOGE(TAG, "unsupported bitrate %" PRIu32, s_cfg.bitrate_bps);
        return ESP_ERR_INVALID_ARG;
    }

    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
                                      (gpio_num_t)s_cfg.tx_gpio,
                                      (gpio_num_t)s_cfg.rx_gpio,
                                      s_cfg.listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    g_cfg.rx_queue_len = 32;

    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t err = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    err = twai_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_start failed: %s", esp_err_to_name(err));
        twai_driver_uninstall();
        return err;
    }

    if (xTaskCreate(can_task, "dash_can", 4096, NULL, 5, NULL) != pdPASS) {
        twai_stop();
        twai_driver_uninstall();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CAN up: TX=GPIO%d RX=GPIO%d %" PRIu32 " bit/s%s, screen commands on ID 0x%03" PRIx32,
             s_cfg.tx_gpio, s_cfg.rx_gpio, s_cfg.bitrate_bps,
             s_cfg.listen_only ? " (listen-only)" : "", s_cfg.screen_cmd_id);
    ESP_LOGI(TAG, "an external 3.3V transceiver is required on these pins");
    return ESP_OK;
}

uint32_t dash_can_rx_count(void)
{
    return s_rx_count;
}
