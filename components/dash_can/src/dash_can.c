#include "dash_can.h"

#include <inttypes.h>

#include "dash_rusefi.h"
#include "dash_signals.h"
#include "dash_ui.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash_can";

static dash_can_cfg_t s_cfg;
static volatile uint32_t s_rx_count;

/* ECU link. Written by the CAN task only; s_ecu_online is read elsewhere. */
static dash_rusefi_t  s_ecu;
static TickType_t     s_ecu_last_rx;
static volatile bool  s_ecu_online;

/* No verbose frame for this long means the ECU is off or unplugged. The ECU
   sends the whole set many times a second, so this is generous. */
#define ECU_TIMEOUT_MS        1000

/* Charge warning: engine turning but the system voltage is at battery level.
   Two thresholds so it does not flicker around one. */
#define BATT_WARN_ON_V        12.0f
#define BATT_WARN_OFF_V       12.6f
#define BATT_WARN_MIN_RPM     500.0f

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

/* Push what the ECU last said into the signal model, converting to the units
   the dash shows. Only fields the ECU has actually sent are touched. */
static void apply_ecu(const dash_rusefi_t *e)
{
    if (e->has & DASH_RUSEFI_HAS_RPM) {
        dash_signals_set_rpm(e->rpm);
    }
    if (e->has & DASH_RUSEFI_HAS_SPEED) {
        dash_signals_set_speed_kmh(e->speed_kmh);
    }
    if (e->has & DASH_RUSEFI_HAS_COOLANT) {
        dash_signals_set_coolant_temp_c(e->coolant_c);
    }
    if (e->has & DASH_RUSEFI_HAS_FUEL_LEVEL) {
        dash_signals_set_fuel_level_pct(e->fuel_level_pct);
    }
    if (e->has & DASH_RUSEFI_HAS_OIL_TEMP) {
        dash_signals_set_oil_temp_c(e->oil_temp_c);
    }
    if (e->has & DASH_RUSEFI_HAS_OIL_PRESS) {
        dash_signals_set_oil_pressure_psi(dash_rusefi_kpa_to_psi(e->oil_press_kpa));
    }
    if (e->has & DASH_RUSEFI_HAS_MAP) {
        dash_signals_set_turbo_psi(dash_rusefi_boost_psi(e->map_kpa));
    }
    if (e->has & DASH_RUSEFI_HAS_STATUS) {
        dash_signals_set_gear(e->gear);
        /* Distance since the ECU powered up. It stands in for the trip meter
           until the GPS node provides a kept odometer. */
        dash_signals_set_trip_km(e->distance_km);
        dash_signals_set_telltale(DASH_TT_CHECK_ENGINE, e->check_engine);
    }
    if ((e->has & DASH_RUSEFI_HAS_BATTERY) && (e->has & DASH_RUSEFI_HAS_RPM)) {
        if (e->rpm < BATT_WARN_MIN_RPM || e->battery_v >= BATT_WARN_OFF_V) {
            dash_signals_set_telltale(DASH_TT_BATTERY, false);
        } else if (e->battery_v < BATT_WARN_ON_V) {
            dash_signals_set_telltale(DASH_TT_BATTERY, true);
        }
    }
}

/* The ECU went quiet. A frozen needle is worse than an empty one, so drop
   everything it was supplying to zero and light the general warning lamp. */
static void ecu_lost(void)
{
    s_ecu_online = false;
    s_ecu = (dash_rusefi_t) {0};

    dash_signals_set_rpm(0.0f);
    dash_signals_set_speed_kmh(0.0f);
    dash_signals_set_coolant_temp_c(0.0f);
    dash_signals_set_fuel_level_pct(0.0f);
    dash_signals_set_oil_temp_c(0.0f);
    dash_signals_set_oil_pressure_psi(0.0f);
    dash_signals_set_turbo_psi(0.0f);
    dash_signals_set_gear(0);
    dash_signals_set_telltale(DASH_TT_CHECK_ENGINE | DASH_TT_BATTERY, false);
    dash_signals_set_telltale(DASH_TT_WARNING, true);
    ESP_LOGW(TAG, "ECU silent for %d ms; gauges cleared", ECU_TIMEOUT_MS);
}

static void handle_ecu_frame(const twai_message_t *msg)
{
    if (!dash_rusefi_decode(s_cfg.ecu_base_id, msg->identifier, msg->data,
                            msg->data_length_code, &s_ecu)) {
        return;
    }
    s_ecu_last_rx = xTaskGetTickCount();
    if (!s_ecu_online) {
        s_ecu_online = true;
        dash_signals_set_telltale(DASH_TT_WARNING, false);
        ESP_LOGI(TAG, "ECU online");
    }
    apply_ecu(&s_ecu);
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
        /* Checked every pass, not only on a receive timeout: other nodes may
           keep the bus busy after the ECU has gone. */
        if (s_ecu_online &&
            (xTaskGetTickCount() - s_ecu_last_rx) > pdMS_TO_TICKS(ECU_TIMEOUT_MS)) {
            ecu_lost();
        }

        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, rx_timeout);
        if (err == ESP_OK) {
            s_rx_count++;
            if (msg.rtr || msg.extd) {
                continue;
            }
            if (msg.identifier == s_cfg.screen_cmd_id) {
                handle_screen_cmd(&msg);
            } else {
                handle_ecu_frame(&msg);
            }
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

    ESP_LOGI(TAG, "CAN up: TX=GPIO%d RX=GPIO%d %" PRIu32 " bit/s%s",
             s_cfg.tx_gpio, s_cfg.rx_gpio, s_cfg.bitrate_bps,
             s_cfg.listen_only ? " (listen-only)" : "");
    ESP_LOGI(TAG, "rusEFI verbose frames at 0x%03" PRIx32 "-0x%03" PRIx32
             ", screen commands on 0x%03" PRIx32,
             s_cfg.ecu_base_id, s_cfg.ecu_base_id + DASH_RUSEFI_NUM_FRAMES - 1,
             s_cfg.screen_cmd_id);
    ESP_LOGI(TAG, "an external 3.3V transceiver is required on these pins");
    return ESP_OK;
}

bool dash_can_ecu_online(void)
{
    return s_ecu_online;
}

uint32_t dash_can_rx_count(void)
{
    return s_rx_count;
}
