/*
 * GPS node for the car dashboard.
 *
 *   receiver (Dragy Pro over Bluetooth, or a u-blox module on a UART)
 *        |  UBX NAV-PVT
 *        v
 *   dash_ubx  ->  dash_gps_fix_t  ->  gps_can  ->  CAN 0x610-0x613  ->  dashboard
 *
 * The node has no display and keeps no state. Power it from the ignition; it
 * finds the Dragy by itself and starts sending within a few seconds.
 */

#include "esp_log.h"
#include "gps_node.h"
#include "nvs_flash.h"

static const char *TAG = "gps_node";

void app_main(void)
{
    /* The Bluetooth controller keeps its calibration in NVS. Nothing of ours
       lives there, so erasing a partition it cannot use loses nothing. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* CAN first: the dashboard should see the node's heartbeat, and so know
       it is alive, before a receiver has been found. */
    ESP_ERROR_CHECK(gps_can_start());

    err = gps_source_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receiver failed to start: %s", esp_err_to_name(err));
    }
}
