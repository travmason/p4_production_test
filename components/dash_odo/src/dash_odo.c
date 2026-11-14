#include "dash_odo.h"

#include <math.h>
#include <stdbool.h>

#include "dash_odo_core.h"
#include "dash_store.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "dash_odo";

/* Kept as whole metres in 32 bits: good for 4.29 million km. */
#define KEY_TOTAL_M   "odo_m"
#define KEY_TRIP_M    "trip0_m"

#define SAVE_EVERY_M        500.0
#define SAVE_STOPPED_MS     3000u
#define SAVE_STOPPED_MIN_M  10.0
#define SAVE_POLL_MS        500

static SemaphoreHandle_t s_lock;
static dash_odo_core_t   s_core;
static double            s_saved_total_m;
static double            s_saved_trip_m;
static uint32_t          s_last_moving_tick;

static void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

static void save_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAVE_POLL_MS));

        lock();
        const double total = s_core.total_m;
        const double trip0 = s_core.trip_start_m;
        const bool moving = s_core.have_last && s_core.last_speed_kmh > 0.0f;
        if (moving) {
            s_last_moving_tick = xTaskGetTickCount();
        }
        const uint32_t stopped_ms =
            (xTaskGetTickCount() - s_last_moving_tick) * portTICK_PERIOD_MS;
        unlock();

        const double unsaved = total - s_saved_total_m;
        const bool save_total = unsaved >= SAVE_EVERY_M ||
                                (unsaved >= SAVE_STOPPED_MIN_M && stopped_ms >= SAVE_STOPPED_MS) ||
                                unsaved < 0.0;   /* set outright */
        const bool save_trip = trip0 != s_saved_trip_m;

        /* The flash writes happen here, outside the lock. */
        if (save_total && dash_store_set_u32(KEY_TOTAL_M, (uint32_t)llround(total))) {
            s_saved_total_m = total;
        }
        if (save_trip && dash_store_set_u32(KEY_TRIP_M, (uint32_t)llround(trip0))) {
            s_saved_trip_m = trip0;
        }
    }
}

esp_err_t dash_odo_start(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint32_t total = dash_store_get_u32(KEY_TOTAL_M, DASH_ODO_INITIAL_KM * 1000u);
    const uint32_t trip0 = dash_store_get_u32(KEY_TRIP_M, total);
    dash_odo_core_init(&s_core, (double)total, (double)trip0);
    s_saved_total_m = s_core.total_m;
    s_saved_trip_m = s_core.trip_start_m;
    s_last_moving_tick = xTaskGetTickCount();

    ESP_LOGI(TAG, "odometer %.1f km, trip %.1f km", s_core.total_m / 1000.0,
             dash_odo_core_trip_m(&s_core) / 1000.0);

    /* Low priority: saving is never urgent. */
    if (xTaskCreate(save_task, "dash_odo", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dash_odo_sample(float speed_kmh, uint32_t now_ms)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    dash_odo_core_sample(&s_core, speed_kmh, now_ms);
    unlock();
}

void dash_odo_break(void)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    dash_odo_core_break(&s_core);
    unlock();
}

void dash_odo_get(float *odometer_km, float *trip_km)
{
    double total = 0.0, trip = 0.0;
    if (s_lock != NULL) {
        lock();
        total = s_core.total_m;
        trip = dash_odo_core_trip_m(&s_core);
        unlock();
    }
    if (odometer_km != NULL) {
        *odometer_km = (float)(total / 1000.0);
    }
    if (trip_km != NULL) {
        *trip_km = (float)(trip / 1000.0);
    }
}

void dash_odo_reset_trip(void)
{
    if (s_lock == NULL) {
        return;
    }
    lock();
    s_core.trip_start_m = s_core.total_m;
    unlock();
    ESP_LOGI(TAG, "trip reset");
}

void dash_odo_set_total_km(uint32_t km)
{
    if (s_lock == NULL || km > 4000000u) {
        return;
    }
    lock();
    s_core.total_m = (double)km * 1000.0;
    s_core.trip_start_m = s_core.total_m;
    /* Force a save whichever way it moved. */
    s_saved_total_m = s_core.total_m + SAVE_EVERY_M;
    unlock();
    ESP_LOGW(TAG, "odometer set to %u km", (unsigned)km);
}
