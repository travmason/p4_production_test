/*
 * Host test for the rusEFI verbose CAN decoder.
 *
 *     cc -Wall -Wextra -Icomponents/dash_can/include \
 *        tests/test_rusefi.c components/dash_can/src/dash_rusefi.c -lm \
 *        -o /tmp/test_rusefi && /tmp/test_rusefi
 *
 * The simulator build runs it too: `ctest --test-dir sim/build`.
 *
 * Frames are built by hand from rusEFI_CAN_verbose.dbc (start bit | length,
 * factor, offset), independently of the decoder's own constants.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_rusefi.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

#define NEAR(a, b, tol)  CHECK(fabsf((float)(a) - (float)(b)) <= (tol))

#define BASE 0x200u

static void put16(uint8_t *d, int byte, unsigned v)
{
    d[byte]     = (uint8_t)(v & 0xFF);
    d[byte + 1] = (uint8_t)(v >> 8);
}

int main(void)
{
    dash_rusefi_t s;
    uint8_t d[8];

    /* ---- base+0 status ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    put16(d, 0, 3);            /* WarningCounter */
    put16(d, 2, 6042);         /* LastError */
    d[4] = 0x08 | 0x04 | 0x40; /* CEL, fuel pump, fan */
    d[5] = 4;                  /* CurrentGear */
    put16(d, 6, 1234);         /* DistanceTraveled, 0.1 km */
    CHECK(dash_rusefi_decode(BASE, BASE + 0, d, 8, &s));
    CHECK(s.has == DASH_RUSEFI_HAS_STATUS);
    CHECK(s.check_engine && s.fuel_pump_on && s.fan_on);
    CHECK(!s.rev_limit_active && !s.main_relay_on);
    CHECK(s.gear == 4);
    CHECK(s.warning_count == 3 && s.last_error == 6042);
    NEAR(s.distance_km, 123.4f, 0.01f);

    d[4] = 0x01 | 0x02;        /* rev limit, main relay */
    CHECK(dash_rusefi_decode(BASE, BASE + 0, d, 8, &s));
    CHECK(s.rev_limit_active && s.main_relay_on && !s.check_engine);

    /* ---- base+1 speeds ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    put16(d, 0, 6850);                 /* RPM */
    put16(d, 2, (unsigned)(-525 & 0xFFFF)); /* IgnitionTiming -10.5 deg / 0.02 */
    d[4] = 151;                        /* InjDuty 75.5 % / 0.5 */
    d[6] = 217;                        /* VehicleSpeed kph */
    CHECK(dash_rusefi_decode(BASE, BASE + 1, d, 8, &s));
    CHECK(s.has == (DASH_RUSEFI_HAS_RPM | DASH_RUSEFI_HAS_SPEED));
    NEAR(s.rpm, 6850.0f, 0.01f);
    NEAR(s.ignition_deg, -10.5f, 0.001f);
    NEAR(s.injector_duty_pct, 75.5f, 0.001f);
    NEAR(s.speed_kmh, 217.0f, 0.001f);

    /* ---- base+2 pedal and throttle ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    put16(d, 0, 10000);                /* PPS 100.00 % */
    put16(d, 2, 4321);                 /* TPS1 43.21 % */
    CHECK(dash_rusefi_decode(BASE, BASE + 2, d, 8, &s));
    NEAR(s.pedal_pct, 100.0f, 0.001f);
    NEAR(s.tps_pct, 43.21f, 0.001f);

    /* ---- base+3 sensors 1 ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    put16(d, 0, 6000);                 /* MAP 200 kPa * 30 */
    d[2] = 132;                        /* CoolantTemp 92 C + 40 */
    d[3] = 0;                          /* IntakeTemp: no sensor */
    d[7] = 97;                         /* FuelLevel 48.5 % / 0.5 */
    CHECK(dash_rusefi_decode(BASE, BASE + 3, d, 8, &s));
    CHECK(s.has == (DASH_RUSEFI_HAS_MAP | DASH_RUSEFI_HAS_COOLANT |
                    DASH_RUSEFI_HAS_FUEL_LEVEL));
    NEAR(s.map_kpa, 200.0f, 0.01f);
    NEAR(s.coolant_c, 92.0f, 0.001f);
    NEAR(s.fuel_level_pct, 48.5f, 0.001f);
    /* 200 kPa absolute is 98.675 kPa of boost, 14.31 psi. */
    NEAR(dash_rusefi_boost_psi(s.map_kpa), 14.31f, 0.01f);
    NEAR(dash_rusefi_boost_psi(35.0f), -9.62f, 0.01f);

    /* ---- base+4 sensors 2 ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    d[0] = 0xAA; d[1] = 0x55;          /* padding must be ignored */
    put16(d, 2, 12411);                /* OilPress 413.7 kPa * 30 (60 psi) */
    d[4] = 148;                        /* OilTemperature 108 C + 40 */
    put16(d, 6, 13850);                /* BattVolt 13.850 V */
    CHECK(dash_rusefi_decode(BASE, BASE + 4, d, 8, &s));
    CHECK(s.has == (DASH_RUSEFI_HAS_OIL_PRESS | DASH_RUSEFI_HAS_OIL_TEMP |
                    DASH_RUSEFI_HAS_BATTERY));
    NEAR(dash_rusefi_kpa_to_psi(s.oil_press_kpa), 60.0f, 0.01f);
    NEAR(s.oil_temp_c, 108.0f, 0.001f);
    NEAR(s.battery_v, 13.85f, 0.001f);

    /* ---- base+7 lambda, base+11 brake ---- */
    memset(&s, 0, sizeof(s));
    memset(d, 0, sizeof(d));
    put16(d, 0, 8800);                 /* Lam1 0.88 */
    CHECK(dash_rusefi_decode(BASE, BASE + 7, d, 8, &s));
    NEAR(s.lambda, 0.88f, 0.0001f);
    d[0] = 1;
    CHECK(dash_rusefi_decode(BASE, BASE + 11, d, 8, &s));
    CHECK(s.brake_pedal);

    /* ---- IDs: frames it has no use for, and frames that are not its own ---- */
    memset(&s, 0, sizeof(s));
    CHECK(dash_rusefi_decode(BASE, BASE + 8, d, 8, &s));   /* cams */
    CHECK(s.has == 0);
    CHECK(!dash_rusefi_decode(BASE, BASE - 1, d, 8, &s));
    CHECK(!dash_rusefi_decode(BASE, BASE + 12, d, 8, &s));
    CHECK(!dash_rusefi_decode(BASE, 0x600, d, 8, &s));
    CHECK(s.has == 0);

    /* A moved base address. */
    memset(d, 0, sizeof(d));
    put16(d, 0, 900);
    CHECK(dash_rusefi_decode(0x500, 0x501, d, 8, &s));
    NEAR(s.rpm, 900.0f, 0.01f);
    CHECK(!dash_rusefi_decode(0x500, BASE + 1, d, 8, &s));

    /* A short frame must not be read past its end. */
    memset(&s, 0, sizeof(s));
    CHECK(!dash_rusefi_decode(BASE, BASE + 1, d, 4, &s));
    CHECK(s.has == 0);

    if (s_failed == 0) {
        printf("rusEFI decoder: all checks passed\n");
        return 0;
    }
    printf("rusEFI decoder: %d check(s) FAILED\n", s_failed);
    return 1;
}
