/*
 * dash_signals - the vehicle signal model.
 *
 * This is the seam between where data comes from and how it is drawn. Producers
 * (the simulator today, a CAN/OBD-II reader later) push values in; the UI pulls
 * a consistent snapshot out. Neither side knows about the other.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Telltale lamps, as a bitmask. */
typedef enum {
    DASH_TT_TURN_LEFT    = 1u << 0,
    DASH_TT_TURN_RIGHT   = 1u << 1,
    DASH_TT_HEADLIGHT    = 1u << 2,
    DASH_TT_HIGH_BEAM    = 1u << 3,
    DASH_TT_LOW_TEMP     = 1u << 4,  /*!< ice / low outside temperature */
    DASH_TT_BRAKE        = 1u << 5,
    DASH_TT_WARNING      = 1u << 6,
    DASH_TT_CHECK_ENGINE = 1u << 7,
    DASH_TT_BATTERY      = 1u << 8,
    DASH_TT_OIL          = 1u << 9,
} dash_telltale_t;

/** A complete instrument reading. */
typedef struct {
    float    speed_kmh;
    float    rpm;
    float    coolant_temp_c;
    float    fuel_level_pct;      /*!< 0..100 */
    float    oil_temp_c;
    float    ambient_temp_c;   /*!< outside air */
    float    oil_pressure_psi;
    float    turbo_psi;           /*!< negative = vacuum */
    float    trip_km;
    float    odometer_km;
    uint32_t telltales;           /*!< bitmask of dash_telltale_t */
    int      gear;                /*!< 0 = neutral */
} dash_signals_t;

/** Create the lock and zero the model. Call once before anything else. */
esp_err_t dash_signals_init(void);

/** Copy a torn-free snapshot of every signal. Safe from any task. */
void dash_signals_get(dash_signals_t *out);

/** Replace every signal at once. For producers that compute a whole frame. */
void dash_signals_publish(const dash_signals_t *in);

/*
 * Per-field setters, for producers that receive one signal at a time - a CAN
 * reader decoding frames as they arrive, for instance.
 */
void dash_signals_set_speed_kmh(float v);
void dash_signals_set_rpm(float v);
void dash_signals_set_coolant_temp_c(float v);
void dash_signals_set_fuel_level_pct(float v);
void dash_signals_set_oil_temp_c(float v);
void dash_signals_set_ambient_temp_c(float v);
void dash_signals_set_oil_pressure_psi(float v);
void dash_signals_set_turbo_psi(float v);
void dash_signals_set_odometer_km(float v);
void dash_signals_set_trip_km(float v);
void dash_signals_set_gear(int gear);

/** Turn one or more telltales on or off without disturbing the others. */
void dash_signals_set_telltale(uint32_t mask, bool on);

#ifdef __cplusplus
}
#endif
