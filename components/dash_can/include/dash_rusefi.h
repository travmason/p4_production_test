/*
 * dash_rusefi - decoder for rusEFI's native "verbose" CAN broadcast.
 *
 * The ECU sends twelve 8-byte frames on consecutive IDs starting at a base
 * address (0x200 unless "verbose CAN base address" is changed in the tune).
 * Every signal is little-endian. The layout follows rusEFI_CAN_verbose.dbc in
 * firmware/controllers/can of the rusEFI source, which is the reference to
 * check against if a firmware update moves something.
 *
 * ECU settings this expects (TunerStudio, CAN section):
 *
 *   CAN baud rate              500 kbit/s
 *   Enable rusEFI CAN broadcast  yes   ("verbose" broadcast)
 *   Verbose CAN base address   0x200 (512)
 *   IDs                        11 bit
 *
 * This file has no ESP-IDF dependencies so that it can be tested on the host;
 * see tests/test_rusefi.c.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DASH_RUSEFI_DEFAULT_BASE_ID  0x200u
#define DASH_RUSEFI_NUM_FRAMES       12u

/** Which members of dash_rusefi_t have been received at least once. */
typedef enum {
    DASH_RUSEFI_HAS_STATUS      = 1u << 0,  /*!< flags, gear, distance */
    DASH_RUSEFI_HAS_RPM         = 1u << 1,
    DASH_RUSEFI_HAS_SPEED       = 1u << 2,
    DASH_RUSEFI_HAS_PEDALS      = 1u << 3,  /*!< tps_pct, pedal_pct */
    DASH_RUSEFI_HAS_MAP         = 1u << 4,
    DASH_RUSEFI_HAS_COOLANT     = 1u << 5,
    DASH_RUSEFI_HAS_INTAKE      = 1u << 6,
    DASH_RUSEFI_HAS_FUEL_LEVEL  = 1u << 7,
    DASH_RUSEFI_HAS_OIL_PRESS   = 1u << 8,
    DASH_RUSEFI_HAS_OIL_TEMP    = 1u << 9,
    DASH_RUSEFI_HAS_BATTERY     = 1u << 10,
    DASH_RUSEFI_HAS_LAMBDA      = 1u << 11,
    DASH_RUSEFI_HAS_BRAKE       = 1u << 12,
} dash_rusefi_field_t;

/** The latest value of everything the dash takes from the ECU, in ECU units. */
typedef struct {
    uint32_t has;             /*!< bitmask of dash_rusefi_field_t */

    /* base+0  status */
    bool     rev_limit_active;
    bool     main_relay_on;
    bool     fuel_pump_on;
    bool     check_engine;
    bool     fan_on;
    int      gear;            /*!< 0 = neutral */
    float    distance_km;     /*!< since the ECU powered up; wraps at 6553.5 */
    uint16_t warning_count;
    uint16_t last_error;

    /* base+1  speeds */
    float    rpm;
    float    ignition_deg;
    float    injector_duty_pct;
    float    speed_kmh;       /*!< whole km/h, 255 at most */

    /* base+2  pedal and throttle */
    float    pedal_pct;
    float    tps_pct;

    /* base+3  sensors 1 */
    float    map_kpa;         /*!< absolute */
    float    coolant_c;
    float    intake_c;
    float    fuel_level_pct;

    /* base+4  sensors 2 */
    float    oil_press_kpa;
    float    oil_temp_c;
    float    battery_v;

    /* base+7  fueling 3 */
    float    lambda;

    /* base+11 */
    bool     brake_pedal;
} dash_rusefi_t;

/**
 * Decode one frame into @p state.
 *
 * Returns true if @p id is one of the verbose frames (base .. base+11) and the
 * frame was long enough, whether or not the dash uses anything in it. Frames
 * the dash has no use for (cams, EGTs, per-cylinder knock) are recognised and
 * otherwise ignored.
 *
 * A temperature the ECU has no sensor for arrives as raw 0, which would read
 * as -40 C. Those are left out: the matching DASH_RUSEFI_HAS_ bit stays clear.
 */
bool dash_rusefi_decode(uint32_t base_id, uint32_t id, const uint8_t *data,
                        uint8_t dlc, dash_rusefi_t *state);

/** Boost in psi relative to a standard atmosphere; negative is vacuum. */
float dash_rusefi_boost_psi(float map_kpa);

/** kPa to psi. */
float dash_rusefi_kpa_to_psi(float kpa);

#ifdef __cplusplus
}
#endif
