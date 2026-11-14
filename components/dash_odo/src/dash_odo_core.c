#include "dash_odo_core.h"

void dash_odo_core_init(dash_odo_core_t *o, double total_m, double trip_start_m)
{
    *o = (dash_odo_core_t) {
        .total_m = total_m,
        /* A trip start beyond the odometer can only be corruption. */
        .trip_start_m = trip_start_m <= total_m ? trip_start_m : total_m,
    };
}

void dash_odo_core_sample(dash_odo_core_t *o, float speed_kmh, uint32_t now_ms)
{
    if (!(speed_kmh >= DASH_ODO_MIN_SPEED_KMH)) {   /* also catches NaN */
        speed_kmh = 0.0f;
    }
    if (o->have_last) {
        const uint32_t dt_ms = now_ms - o->last_ms;
        if (dt_ms <= DASH_ODO_MAX_GAP_MS) {
            /* Trapezoid between the two samples. km/h * ms / 3600 = metres. */
            const double mean = ((double)o->last_speed_kmh + (double)speed_kmh) * 0.5;
            o->total_m += mean * (double)dt_ms / 3600.0;
        }
    }
    o->last_ms = now_ms;
    o->last_speed_kmh = speed_kmh;
    o->have_last = true;
}

void dash_odo_core_break(dash_odo_core_t *o)
{
    o->have_last = false;
}

double dash_odo_core_trip_m(const dash_odo_core_t *o)
{
    return o->total_m - o->trip_start_m;
}
