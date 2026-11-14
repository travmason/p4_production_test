#include "dash_rusefi.h"

#include <stddef.h>

#define KPA_PER_PSI        6.894757f
#define STANDARD_ATM_KPA   101.325f

/* rusEFI packs pressures as kPa * 30 in 16 bits. */
#define PRESSURE_SCALE     (1.0f / 30.0f)

static uint16_t u16le(const uint8_t *d, int byte)
{
    return (uint16_t)(d[byte] | ((uint16_t)d[byte + 1] << 8));
}

static int16_t s16le(const uint8_t *d, int byte)
{
    return (int16_t)u16le(d, byte);
}

/* Temperatures are one byte, offset by 40. Raw 0 is "no sensor". */
static bool temp_c(uint8_t raw, float *out)
{
    if (raw == 0) {
        return false;
    }
    *out = (float)raw - 40.0f;
    return true;
}

float dash_rusefi_kpa_to_psi(float kpa)
{
    return kpa / KPA_PER_PSI;
}

float dash_rusefi_boost_psi(float map_kpa)
{
    return (map_kpa - STANDARD_ATM_KPA) / KPA_PER_PSI;
}

bool dash_rusefi_decode(uint32_t base_id, uint32_t id, const uint8_t *d,
                        uint8_t dlc, dash_rusefi_t *s)
{
    if (d == NULL || s == NULL || id < base_id ||
        id >= base_id + DASH_RUSEFI_NUM_FRAMES) {
        return false;
    }

    switch (id - base_id) {
    case 0:   /* status */
        if (dlc < 8) {
            return false;
        }
        s->warning_count    = u16le(d, 0);
        s->last_error       = u16le(d, 2);
        s->rev_limit_active = (d[4] & 0x01) != 0;
        s->main_relay_on    = (d[4] & 0x02) != 0;
        s->fuel_pump_on     = (d[4] & 0x04) != 0;
        s->check_engine     = (d[4] & 0x08) != 0;
        s->fan_on           = (d[4] & 0x40) != 0;
        s->gear             = d[5];
        s->distance_km      = (float)u16le(d, 6) * 0.1f;
        s->has |= DASH_RUSEFI_HAS_STATUS;
        return true;

    case 1:   /* speeds */
        if (dlc < 8) {
            return false;
        }
        s->rpm               = (float)u16le(d, 0);
        s->ignition_deg      = (float)s16le(d, 2) * 0.02f;
        s->injector_duty_pct = (float)d[4] * 0.5f;
        s->speed_kmh         = (float)d[6];
        s->has |= DASH_RUSEFI_HAS_RPM | DASH_RUSEFI_HAS_SPEED;
        return true;

    case 2:   /* pedal and throttle */
        if (dlc < 4) {
            return false;
        }
        s->pedal_pct = (float)s16le(d, 0) * 0.01f;
        s->tps_pct   = (float)s16le(d, 2) * 0.01f;
        s->has |= DASH_RUSEFI_HAS_PEDALS;
        return true;

    case 3:   /* sensors 1 */
        if (dlc < 8) {
            return false;
        }
        s->map_kpa = (float)u16le(d, 0) * PRESSURE_SCALE;
        s->has |= DASH_RUSEFI_HAS_MAP;
        if (temp_c(d[2], &s->coolant_c)) {
            s->has |= DASH_RUSEFI_HAS_COOLANT;
        }
        if (temp_c(d[3], &s->intake_c)) {
            s->has |= DASH_RUSEFI_HAS_INTAKE;
        }
        s->fuel_level_pct = (float)d[7] * 0.5f;
        s->has |= DASH_RUSEFI_HAS_FUEL_LEVEL;
        return true;

    case 4:   /* sensors 2; bytes 0-1 are padding */
        if (dlc < 8) {
            return false;
        }
        s->oil_press_kpa = (float)u16le(d, 2) * PRESSURE_SCALE;
        s->has |= DASH_RUSEFI_HAS_OIL_PRESS;
        if (temp_c(d[4], &s->oil_temp_c)) {
            s->has |= DASH_RUSEFI_HAS_OIL_TEMP;
        }
        s->battery_v = (float)u16le(d, 6) * 0.001f;
        s->has |= DASH_RUSEFI_HAS_BATTERY;
        return true;

    case 7:   /* fueling 3 */
        if (dlc < 2) {
            return false;
        }
        s->lambda = (float)u16le(d, 0) * 0.0001f;
        s->has |= DASH_RUSEFI_HAS_LAMBDA;
        return true;

    case 11:  /* status 11 */
        if (dlc < 1) {
            return false;
        }
        s->brake_pedal = (d[0] & 0x01) != 0;
        s->has |= DASH_RUSEFI_HAS_BRAKE;
        return true;

    default:  /* fueling, cams, EGTs, knock: recognised, not used */
        return true;
    }
}
