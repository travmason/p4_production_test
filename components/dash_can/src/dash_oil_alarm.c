#include "dash_oil_alarm.h"

bool dash_oil_alarm_step(dash_oil_alarm_t *a, float rpm, float oil_psi, uint32_t now_ms)
{
    if (rpm < DASH_OIL_RUNNING_RPM) {
        *a = (dash_oil_alarm_t) {0};
        return false;
    }
    if (!a->running) {
        a->running = true;
        a->running_since_ms = now_ms;
    }

    const float limit = rpm >= DASH_OIL_HIGH_RPM ? DASH_OIL_LIMIT_HIGH_PSI
                                                 : DASH_OIL_LIMIT_IDLE_PSI;

    if (a->active) {
        /* Stays on until pressure is clearly back, so it cannot chatter. */
        if (oil_psi >= limit + DASH_OIL_CLEAR_MARGIN_PSI) {
            a->active = false;
            a->low = false;
        }
        return a->active;
    }

    if (oil_psi >= limit) {
        a->low = false;
        return false;
    }
    if (!a->low) {
        a->low = true;
        a->low_since_ms = now_ms;
    }
    if ((uint32_t)(now_ms - a->running_since_ms) >= DASH_OIL_START_GRACE_MS &&
        (uint32_t)(now_ms - a->low_since_ms) >= DASH_OIL_LOW_HOLD_MS) {
        a->active = true;
    }
    return a->active;
}
