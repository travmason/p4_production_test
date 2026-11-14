#include "dash_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dash_store";

#define NVS_NAMESPACE  "dash"

static bool nvs_ready(void)
{
    static bool tried, ok;
    if (tried) {
        return ok;
    }
    tried = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition cannot be used as it is. Erasing loses the odometer,
           but so does leaving it unusable - and says so loudly. */
        ESP_LOGE(TAG, "NVS unusable (%s): erasing. THE ODOMETER AND SETTINGS ARE RESET.",
                 esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    ok = (err == ESP_OK);
    if (!ok) {
        ESP_LOGE(TAG, "NVS unavailable (%s); nothing will be kept", esp_err_to_name(err));
    }
    return ok;
}

uint32_t dash_store_get_u32(const char *key, uint32_t fallback)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    uint32_t v;
    const esp_err_t err = nvs_get_u32(h, key, &v);
    nvs_close(h);
    return err == ESP_OK ? v : fallback;
}

bool dash_store_set_u32(const char *key, uint32_t value)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    const bool ok = nvs_set_u32(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    if (!ok) {
        ESP_LOGW(TAG, "could not save %s", key);
    }
    return ok;
}
