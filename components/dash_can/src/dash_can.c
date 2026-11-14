#include "dash_can.h"

#include <inttypes.h>

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

/* The dash_ui screen calls only post a request, which the LVGL task acts on, so
   nothing here needs the display lock and the receive loop never waits on it. */
static void handle_screen_cmd(const twai_message_t *msg)
{
    if (msg->data_length_code < 1) {
        return;
    }
    const uint8_t cmd = msg->data[0];

    if (cmd == DASH_CAN_SCREEN_NEXT) {
        dash_ui_next_screen();
    } else if (cmd == DASH_CAN_SCREEN_PREV) {
        dash_ui_prev_screen();
    } else if (cmd >= DASH_CAN_SCREEN_ABS_BASE) {
        dash_ui_set_screen(cmd - DASH_CAN_SCREEN_ABS_BASE);
    }
}

/* Receive timeout while the bus is healthy. Finite rather than portMAX_DELAY so
   a bus-off still gets noticed. */
#define RX_IDLE_TIMEOUT_MS  1000
/* How often to look at the driver state while it is not running. */
#define RX_DOWN_POLL_MS     100

static void can_task(void *arg)
{
    (void)arg;

    TickType_t rx_timeout = pdMS_TO_TICKS(RX_IDLE_TIMEOUT_MS);

    while (true) {
        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, rx_timeout);
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
            /* Not expected, but never spin on an error that returns at once. */
            vTaskDelay(pdMS_TO_TICKS(RX_DOWN_POLL_MS));
        }

        /*
         * Bus-off recovery. A bus-off happens when transmits keep failing - an
         * unterminated bus, or a missing transceiver, while not in listen-only
         * mode. Recovery is three driver states, and each has to be seen:
         *
         *   BUS_OFF     ask the controller to start recovering
         *   RECOVERING  it is counting 128 x 11 recessive bits; on a noisy or
         *               stuck bus that takes arbitrarily long, so just wait
         *   STOPPED     recovery finished and left the driver stopped; only
         *               now will twai_start() succeed
         *
         * Nothing else in this component stops the driver, so STOPPED always
         * means "recovered, restart me".
         */
        twai_status_info_t st;
        if (twai_get_status_info(&st) != ESP_OK) {
            continue;
        }
        switch (st.state) {
        case TWAI_STATE_BUS_OFF:
            ESP_LOGW(TAG, "bus-off, recovering");
            twai_initiate_recovery();
            rx_timeout = pdMS_TO_TICKS(RX_DOWN_POLL_MS);
            break;
        case TWAI_STATE_RECOVERING:
            rx_timeout = pdMS_TO_TICKS(RX_DOWN_POLL_MS);
            break;
        case TWAI_STATE_STOPPED:
            if (twai_start() == ESP_OK) {
                ESP_LOGI(TAG, "bus recovered, receiving again");
                rx_timeout = pdMS_TO_TICKS(RX_IDLE_TIMEOUT_MS);
            } else {
                rx_timeout = pdMS_TO_TICKS(RX_DOWN_POLL_MS);
            }
            break;
        default:
            rx_timeout = pdMS_TO_TICKS(RX_IDLE_TIMEOUT_MS);
            break;
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
