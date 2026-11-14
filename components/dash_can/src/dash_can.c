#include "dash_can.h"

#include <inttypes.h>
#include <sys/time.h>
#include <time.h>

#include "dash_gps_proto.h"
#include "dash_odo.h"
#include "dash_oil_alarm.h"
#include "dash_rusefi.h"
#include "dash_signals.h"
#include "dash_source.h"
#include "dash_store.h"
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
static dash_oil_alarm_t s_oil_alarm;

/* GPS node link. Written by the CAN task only. */
static dash_gps_fix_t s_gps;
static TickType_t     s_gps_last_rx;
static volatile bool  s_gps_node_online;   /* the node is being heard */
static bool           s_gps_expected;      /* a node has been seen on this car */
static bool           s_speed_from_gps;    /* who fed the odometer last */

#define GPS_TIMEOUT_MS        1000
#define KEY_GPS_SEEN          "gps_seen"

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/*
 * Speed and distance are not the ECU's alone. The GPS node puts them on the
 * same bus and is the better source, so whenever it has a fix it owns them and
 * the ECU's copies are ignored - and losing the ECU then leaves them alone.
 * The ECU's vehicle speed is the fallback: with no node, or a node without a
 * fix (a tunnel, a cold start), speed and distance come from the ECU.
 */
static bool gps_online(void)
{
    return s_gps_node_online && (s_gps.flags & DASH_GPS_FLAG_FIX_OK) != 0;
}

/* Feed the odometer from whichever source owns speed. When ownership changes
   hands the two clocks of samples are not joined up. */
static void odo_sample(bool from_gps, float speed_kmh)
{
    if (from_gps != s_speed_from_gps) {
        s_speed_from_gps = from_gps;
        dash_odo_break();
    }
    dash_odo_sample(speed_kmh, now_ms());
}

/* The odometer is the dash's own, so it is shown whoever is or is not on the
   bus - but not over the simulator's made-up one. */
static void publish_odo(void)
{
    if (!dash_source_live_writes_allowed()) {
        return;
    }
    float odo, trip;
    dash_odo_get(&odo, &trip);
    dash_signals_set_odometer_km(odo);
    dash_signals_set_trip_km(trip);
}

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

    if (cmd >= DASH_CAN_SOURCE_BASE && cmd < DASH_CAN_SOURCE_BASE + DASH_SOURCE_NUM_MODES) {
        dash_source_set_mode((dash_source_mode_t)(cmd - DASH_CAN_SOURCE_BASE));
        return;
    }
    if (cmd == DASH_CAN_TRIP_RESET) {
        dash_odo_reset_trip();
        return;
    }
    if (cmd == DASH_CAN_ODO_SET && msg->data_length_code >= 5) {
        dash_odo_set_total_km((uint32_t)msg->data[1] | ((uint32_t)msg->data[2] << 8) |
                              ((uint32_t)msg->data[3] << 16) | ((uint32_t)msg->data[4] << 24));
        return;
    }
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
    if ((e->has & DASH_RUSEFI_HAS_SPEED) && !gps_online()) {
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
        dash_signals_set_telltale(DASH_TT_CHECK_ENGINE, e->check_engine);
    }
    /* Low oil pressure, judged here from the ECU's sensor: the gauge goes red
       and the warning lamp lights. See dash_oil_alarm.h for the limits. */
    if ((e->has & DASH_RUSEFI_HAS_OIL_PRESS) && (e->has & DASH_RUSEFI_HAS_RPM)) {
        const bool low = dash_oil_alarm_step(&s_oil_alarm, e->rpm,
                                             dash_rusefi_kpa_to_psi(e->oil_press_kpa),
                                             (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        dash_signals_set_telltale(DASH_TT_OIL, low);
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
   everything it was supplying to zero. dash_source sees s_ecu_online go false
   and has the UI say NO ECU. */
static void ecu_lost(void)
{
    s_ecu_online = false;
    s_ecu = (dash_rusefi_t) {0};
    s_oil_alarm = (dash_oil_alarm_t) {0};
    if (!gps_online()) {
        dash_odo_break();   /* the ECU was feeding the odometer */
    }
    ESP_LOGW(TAG, "ECU silent for %d ms", ECU_TIMEOUT_MS);

    if (!dash_source_live_writes_allowed()) {
        return;   /* the simulator has the gauges */
    }

    /* Only what the ECU was actually supplying. Speed stays as it is when
       the GPS node has it, and the odometer is nobody's to zero. */
    if (!gps_online()) {
        dash_signals_set_speed_kmh(0.0f);
    }
    dash_signals_set_rpm(0.0f);
    dash_signals_set_coolant_temp_c(0.0f);
    dash_signals_set_fuel_level_pct(0.0f);
    dash_signals_set_oil_temp_c(0.0f);
    dash_signals_set_oil_pressure_psi(0.0f);
    dash_signals_set_turbo_psi(0.0f);
    dash_signals_set_gear(0);
    dash_signals_set_telltale(DASH_TT_CHECK_ENGINE | DASH_TT_BATTERY | DASH_TT_OIL, false);
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
        ESP_LOGI(TAG, "ECU online");
    }
    /* Distance is counted whatever the screen is showing. The speed frame is
       the one that carries a fresh sample. */
    if (msg->identifier == s_cfg.ecu_base_id + 1 && !gps_online()) {
        odo_sample(false, s_ecu.speed_kmh);
    }
    /* Decoding carries on regardless, so the values are current the moment
       the source is switched to live. */
    if (dash_source_live_writes_allowed()) {
        apply_ecu(&s_ecu);
    }
}

/* Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
   days_from_civil), so UTC needs no help from the C library's time zone. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* Keep the system clock on GPS time. Local time is the TZ variable's job. */
static void set_clock_from_gps(const dash_gps_fix_t *g)
{
    if (g->year < 2024 || g->month < 1 || g->month > 12 || g->day < 1 || g->day > 31 ||
        g->hour > 23 || g->minute > 59 || g->second > 60) {
        return;
    }
    const time_t utc = (time_t)(days_from_civil(g->year, g->month, g->day) * 86400 +
                                g->hour * 3600 + g->minute * 60 + g->second);
    const time_t have = time(NULL);
    const int64_t off = (int64_t)utc - (int64_t)have;
    if (off > 1 || off < -1) {
        const struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock set from GPS: %04u-%02u-%02u %02u:%02u:%02u UTC",
                 g->year, g->month, g->day, g->hour, g->minute, g->second);
    }
}

static void gps_lost(void)
{
    s_gps_node_online = false;
    s_gps.flags = 0;
    ESP_LOGW(TAG, "GPS node silent for %d ms", GPS_TIMEOUT_MS);
    dash_odo_break();

    /* Speed falls back to the ECU, whose next frame supplies it. With no ECU
       either there is no speed to show. */
    if (!s_ecu_online && dash_source_live_writes_allowed()) {
        dash_signals_set_speed_kmh(0.0f);
    }
}

static bool handle_gps_frame(const twai_message_t *msg)
{
    if (!dash_gps_unpack(msg->identifier, msg->data, msg->data_length_code, &s_gps)) {
        return false;
    }
    s_gps_last_rx = xTaskGetTickCount();
    if (!s_gps_node_online) {
        s_gps_node_online = true;
        ESP_LOGI(TAG, "GPS node online");
        if (!s_gps_expected) {
            /* From now on this car is expected to have GPS, and its absence
               is worth a warning. Written once in the board's life. */
            s_gps_expected = true;
            dash_store_set_u32(KEY_GPS_SEEN, 1);
        }
    }

    if (msg->identifier == DASH_GPS_ID_MOTION && gps_online()) {
        odo_sample(true, s_gps.speed_kmh);
        if (dash_source_live_writes_allowed()) {
            dash_signals_set_speed_kmh(s_gps.speed_kmh);
        }
    } else if (msg->identifier == DASH_GPS_ID_TIME &&
               (s_gps.flags & DASH_GPS_FLAG_TIME_VALID)) {
        set_clock_from_gps(&s_gps);
    }
    return true;
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
        if (s_gps_node_online &&
            (xTaskGetTickCount() - s_gps_last_rx) > pdMS_TO_TICKS(GPS_TIMEOUT_MS)) {
            gps_lost();
        }
        /* Warn about GPS only on a car that has had it. */
        dash_ui_set_gps_warning(s_gps_expected && !gps_online());
        publish_odo();

        twai_message_t msg;
        const esp_err_t err = twai_receive(&msg, rx_timeout);
        if (err == ESP_OK) {
            s_rx_count++;
            if (msg.rtr || msg.extd) {
                continue;
            }
            if (msg.identifier == s_cfg.screen_cmd_id) {
                handle_screen_cmd(&msg);
            } else if (!handle_gps_frame(&msg)) {
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
    s_gps_expected = dash_store_get_u32(KEY_GPS_SEEN, 0) != 0;

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

bool dash_can_gps_online(void)
{
    return s_gps_node_online;
}

bool dash_can_ecu_online(void)
{
    return s_ecu_online;
}

uint32_t dash_can_rx_count(void)
{
    return s_rx_count;
}
