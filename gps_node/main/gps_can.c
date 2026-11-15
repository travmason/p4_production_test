/*
 * CAN side of the GPS node: sends the frames in dash_gps_proto.h.
 *
 *   motion    on every fix (the receiver's own rate, typically 10-25 Hz),
 *             and twice a second as a heartbeat when there is none
 *   position  5 Hz
 *   time      1 Hz
 *   quality   1 Hz
 *
 * The node only ever transmits. It needs an external 3.3 V CAN transceiver
 * (SN65HVD230 or similar) between the two GPIOs and the bus, as the dashboard
 * does.
 */

#include <inttypes.h>
#include <string.h>

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gps_node.h"
#include "sdkconfig.h"

static const char *TAG = "gps_can";

#define HEARTBEAT_MS   500
#define POSITION_MS    200
#define SLOW_MS        1000
#define TX_TIMEOUT_MS  10

static SemaphoreHandle_t s_lock;
static dash_gps_fix_t    s_fix = { .battery_pct = DASH_GPS_BATTERY_UNKNOWN };
static bool              s_linked;
static TaskHandle_t      s_task;
static uint32_t          s_tx_fail;

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

void gps_can_publish(const dash_gps_fix_t *fix)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint8_t battery = s_fix.battery_pct;
    const uint8_t counter = s_fix.counter;
    s_fix = *fix;
    s_fix.battery_pct = battery;
    s_fix.counter = counter;
    s_linked = true;
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);
}

void gps_can_set_battery(uint8_t pct)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fix.battery_pct = pct <= 100 ? pct : DASH_GPS_BATTERY_UNKNOWN;
    xSemaphoreGive(s_lock);
}

void gps_can_link_lost(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_linked = false;
    /* Whatever the last fix said, it is not a fix any more. */
    s_fix.flags = 0;
    s_fix.fix_type = 0;
    s_fix.speed_kmh = 0.0f;
    s_fix.satellites = 0;
    s_fix.battery_pct = DASH_GPS_BATTERY_UNKNOWN;
    xSemaphoreGive(s_lock);
}

static void send(uint32_t id, const uint8_t data[8])
{
    twai_message_t msg = { .identifier = id, .data_length_code = 8 };
    memcpy(msg.data, data, 8);
    if (twai_transmit(&msg, pdMS_TO_TICKS(TX_TIMEOUT_MS)) != ESP_OK) {
        s_tx_fail++;
    }
}

/* With nobody acknowledging - the dashboard off, or the bus unplugged - the
   controller goes bus-off. Walk it back, as the dashboard's receiver does. */
static void recover_if_needed(void)
{
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) {
        return;
    }
    if (st.state == TWAI_STATE_BUS_OFF) {
        twai_initiate_recovery();
    } else if (st.state == TWAI_STATE_STOPPED) {
        twai_start();
    }
}

static void can_task(void *arg)
{
    (void)arg;

    TickType_t last_position = 0, last_slow = 0, last_report = 0;

    while (true) {
        /* Woken by a new fix; otherwise the timeout is the heartbeat. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(HEARTBEAT_MS));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_fix.counter++;
        dash_gps_fix_t fix = s_fix;
        const bool linked = s_linked;
        xSemaphoreGive(s_lock);

        fix.flags &= (uint8_t)~(DASH_GPS_FLAG_LINKED | DASH_GPS_FLAG_SRC_UBLOX);
        fix.flags |= gps_source_flag();
        if (linked) {
            fix.flags |= DASH_GPS_FLAG_LINKED;
        }

        recover_if_needed();

        uint8_t d[8];
        dash_gps_pack_motion(&fix, d);
        send(DASH_GPS_ID_MOTION, d);

        const TickType_t now = xTaskGetTickCount();
        if (linked && (fix.flags & DASH_GPS_FLAG_FIX_OK) &&
            now - last_position >= pdMS_TO_TICKS(POSITION_MS)) {
            last_position = now;
            dash_gps_pack_position(&fix, d);
            send(DASH_GPS_ID_POSITION, d);
        }
        if (now - last_slow >= pdMS_TO_TICKS(SLOW_MS)) {
            last_slow = now;
            if (linked) {
                dash_gps_pack_time(&fix, d);
                send(DASH_GPS_ID_TIME, d);
            }
            dash_gps_pack_quality(&fix, d);
            send(DASH_GPS_ID_QUALITY, d);
        }

        if (now - last_report >= pdMS_TO_TICKS(10000)) {
            last_report = now;
            ESP_LOGI(TAG, "%s  fix %u  sats %u  %.1f km/h  battery %u%%  tx failures %" PRIu32,
                     linked ? "receiver linked" : "no receiver", fix.fix_type, fix.satellites,
                     (double)fix.speed_kmh,
                     fix.battery_pct == DASH_GPS_BATTERY_UNKNOWN ? 0 : fix.battery_pct, s_tx_fail);
        }
    }
}

esp_err_t gps_can_start(void)
{
    twai_timing_config_t t_cfg;
    if (!timing_for(CONFIG_GPS_CAN_BITRATE, &t_cfg)) {
        ESP_LOGE(TAG, "unsupported bit rate %d", CONFIG_GPS_CAN_BITRATE);
        return ESP_ERR_INVALID_ARG;
    }
    twai_general_config_t g_cfg = TWAI_GENERAL_CONFIG_DEFAULT(
                                      (gpio_num_t)CONFIG_GPS_CAN_TX_GPIO,
                                      (gpio_num_t)CONFIG_GPS_CAN_RX_GPIO, TWAI_MODE_NORMAL);
    g_cfg.tx_queue_len = 16;
    /* The node acts on nothing it hears. */
    twai_filter_config_t f_cfg = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    g_cfg.rx_queue_len = 1;

    esp_err_t err = twai_driver_install(&g_cfg, &t_cfg, &f_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_start();
    if (err != ESP_OK) {
        twai_driver_uninstall();
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL ||
        xTaskCreate(can_task, "gps_can", 4096, NULL, 6, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "CAN up: TX=GPIO%d RX=GPIO%d %d bit/s, frames 0x%03X-0x%03X",
             CONFIG_GPS_CAN_TX_GPIO, CONFIG_GPS_CAN_RX_GPIO, CONFIG_GPS_CAN_BITRATE,
             (unsigned)DASH_GPS_ID_MOTION, (unsigned)DASH_GPS_ID_QUALITY);
    return ESP_OK;
}
