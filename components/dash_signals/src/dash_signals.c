#include "dash_signals.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* A mutex rather than atomics: readers want a whole consistent frame, not
   individually-fresh fields, and the contention here is trivial. */
static SemaphoreHandle_t s_lock;
static dash_signals_t    s_signals;

#define WITH_LOCK(body)                                    \
    do {                                                   \
        if (s_lock == NULL) {                              \
            break;                                         \
        }                                                  \
        xSemaphoreTake(s_lock, portMAX_DELAY);             \
        body;                                              \
        xSemaphoreGive(s_lock);                            \
    } while (0)

esp_err_t dash_signals_init(void)
{
    if (s_lock != NULL) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_signals = (dash_signals_t) {0};
    return ESP_OK;
}

void dash_signals_get(dash_signals_t *out)
{
    if (out == NULL) {
        return;
    }
    WITH_LOCK(*out = s_signals);
}

void dash_signals_publish(const dash_signals_t *in)
{
    if (in == NULL) {
        return;
    }
    WITH_LOCK(s_signals = *in);
}

#define DEFINE_SETTER(fn, field, type)      \
    void fn(type v)                         \
    {                                       \
        WITH_LOCK(s_signals.field = v);     \
    }

DEFINE_SETTER(dash_signals_set_speed_kmh,        speed_kmh,        float)
DEFINE_SETTER(dash_signals_set_rpm,              rpm,              float)
DEFINE_SETTER(dash_signals_set_coolant_temp_c,   coolant_temp_c,   float)
DEFINE_SETTER(dash_signals_set_fuel_level_pct,   fuel_level_pct,   float)
DEFINE_SETTER(dash_signals_set_oil_temp_c,       oil_temp_c,       float)
DEFINE_SETTER(dash_signals_set_ambient_temp_c,   ambient_temp_c,   float)
DEFINE_SETTER(dash_signals_set_oil_pressure_psi, oil_pressure_psi, float)
DEFINE_SETTER(dash_signals_set_turbo_psi,        turbo_psi,        float)
DEFINE_SETTER(dash_signals_set_odometer_km,      odometer_km,      float)
DEFINE_SETTER(dash_signals_set_trip_km,          trip_km,          float)
DEFINE_SETTER(dash_signals_set_gear,             gear,             int)

void dash_signals_set_telltale(uint32_t mask, bool on)
{
    WITH_LOCK(
        if (on) {
            s_signals.telltales |= mask;
        } else {
            s_signals.telltales &= ~mask;
        }
    );
}
