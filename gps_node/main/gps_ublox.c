/*
 * A u-blox module wired to a UART, as the receiver instead of a Dragy.
 *
 * NOT YET TRIED ON HARDWARE. It is here so that fitting a dedicated module
 * later is a build option and a few wires, not new firmware.
 *
 * The module has to be sending UBX-NAV-PVT, which u-blox modules do not do
 * out of the box (they send NMEA at 1 Hz). Configure it once with u-center,
 * saved to the module's flash:
 *
 *   - UART1 output protocol: UBX            (CFG-UART1OUTPROT-UBX = 1)
 *   - NAV-PVT on UART1, every solution      (CFG-MSGOUT-UBX_NAV_PVT_UART1 = 1)
 *   - measurement rate 100 ms for 10 Hz     (CFG-RATE-MEAS = 100)
 *   - baud rate to match GPS_UBLOX_BAUD     (CFG-UART1-BAUDRATE = 115200)
 *
 * This file deliberately sends the module nothing: configuration written from
 * here would have to be right for every module generation.
 *
 * Wiring: module TX -> GPS_UBLOX_RX_GPIO, module GND -> GND, module VCC -> 3V3.
 */

#include "dash_ubx.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gps_node.h"
#include "sdkconfig.h"

static const char *TAG = "gps_ublox";

#define UBLOX_UART        UART_NUM_1
#define STALL_MS          2000

uint8_t gps_source_flag(void)
{
    return DASH_GPS_FLAG_SRC_UBLOX;
}

static void ublox_task(void *arg)
{
    (void)arg;

    dash_ubx_parser_t parser;
    dash_ubx_init(&parser);
    dash_gps_fix_t fix = {0};
    uint8_t buf[128];
    TickType_t last_fix = xTaskGetTickCount();
    bool linked = false;

    while (true) {
        const int n = uart_read_bytes(UBLOX_UART, buf, sizeof(buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            if (dash_ubx_feed(&parser, buf[i], &fix)) {
                last_fix = xTaskGetTickCount();
                if (!linked) {
                    linked = true;
                    ESP_LOGI(TAG, "receiving NAV-PVT");
                }
                gps_can_publish(&fix);
            }
        }
        if (linked && xTaskGetTickCount() - last_fix > pdMS_TO_TICKS(STALL_MS)) {
            linked = false;
            ESP_LOGW(TAG, "no NAV-PVT for %d ms", STALL_MS);
            gps_can_link_lost();
        }
    }
}

esp_err_t gps_source_start(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_GPS_UBLOX_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UBLOX_UART, 2048, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(UBLOX_UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(UBLOX_UART,
                           CONFIG_GPS_UBLOX_TX_GPIO >= 0 ? CONFIG_GPS_UBLOX_TX_GPIO
                                                         : UART_PIN_NO_CHANGE,
                           CONFIG_GPS_UBLOX_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        return err;
    }
    if (xTaskCreate(ublox_task, "gps_ublox", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "u-blox on UART%d, RX=GPIO%d, %d baud", UBLOX_UART,
             CONFIG_GPS_UBLOX_RX_GPIO, CONFIG_GPS_UBLOX_BAUD);
    return ESP_OK;
}
