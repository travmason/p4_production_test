#include "dash_source_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "dash_source";

#define NVS_NAMESPACE  "dash"
#define NVS_KEY        "source_mode"

static bool nvs_ready(void)
{
    static bool tried, ok;
    if (tried) {
        return ok;
    }
    tried = true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Only settings live here, and every one has a default. */
        ESP_LOGW(TAG, "NVS needs erasing (%s)", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    ok = (err == ESP_OK);
    if (!ok) {
        ESP_LOGW(TAG, "NVS unavailable (%s); the source mode will not be kept",
                 esp_err_to_name(err));
    }
    return ok;
}

int dash_source_store_load(int fallback)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return fallback;
    }
    uint8_t v;
    const esp_err_t err = nvs_get_u8(h, NVS_KEY, &v);
    nvs_close(h);
    return err == ESP_OK ? (int)v : fallback;
}

void dash_source_store_save(int value)
{
    nvs_handle_t h;
    if (!nvs_ready() || nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u8(h, NVS_KEY, (uint8_t)value) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "could not save the source mode");
    }
    nvs_close(h);
}
