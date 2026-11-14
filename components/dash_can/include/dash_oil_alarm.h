/*
 * Low oil pressure alarm, from the ECU's oil pressure and engine speed.
 * Pure C, tested on the host (tests/test_oil_alarm.c).
 *
 * The alarm is for a running engine only: with the engine stopped there is no
 * pressure and nothing wrong. It trips when pressure has been under the limit
 * for DASH_OIL_LOW_HOLD_MS, so a single noisy sample does nothing, and not
 * until the engine has been running for DASH_OIL_START_GRACE_MS, so the second
 * or so the pump takes to build pressure after a start does not flash it.
 *
 * The limits are deliberately low. They are a "stop now" warning, not a
 * judgement on whether the pressure is healthy; set them for the engine.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DASH_OIL_RUNNING_RPM      500.0f   /*!< below this the engine is not running */
#define DASH_OIL_HIGH_RPM         3000.0f  /*!< at and above this, the higher limit applies */
#define DASH_OIL_LIMIT_IDLE_PSI   10.0f    /*!< alarm below this under DASH_OIL_HIGH_RPM */
#define DASH_OIL_LIMIT_HIGH_PSI   25.0f    /*!< alarm below this at DASH_OIL_HIGH_RPM and up */
#define DASH_OIL_CLEAR_MARGIN_PSI 4.0f     /*!< must rise this far over the limit to clear */
#define DASH_OIL_START_GRACE_MS   2000u
#define DASH_OIL_LOW_HOLD_MS      500u

typedef struct {
    bool     active;
    bool     running;
    bool     low;
    uint32_t running_since_ms;
    uint32_t low_since_ms;
} dash_oil_alarm_t;

/**
 * Feed the latest reading; returns whether the alarm is on.
 *
 * @param now_ms  any millisecond clock that only moves forward (wrap is fine)
 */
bool dash_oil_alarm_step(dash_oil_alarm_t *a, float rpm, float oil_psi, uint32_t now_ms);
