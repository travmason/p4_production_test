/* Host test for the odometer arithmetic. Run with `ctest --test-dir sim/build`. */

#include <math.h>
#include <stdio.h>

#include "dash_odo_core.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

#define NEAR(a, b, tol)  CHECK(fabs((double)(a) - (double)(b)) <= (tol))

int main(void)
{
    dash_odo_core_t o;
    uint32_t t = 5000;

    /* An hour at a steady 100 km/h, sampled at 10 Hz, is 100 km. */
    dash_odo_core_init(&o, 124503000.0, 124490600.0);
    NEAR(dash_odo_core_trip_m(&o), 12400.0, 1e-6);
    for (int i = 0; i <= 36000; i++, t += 100) {
        dash_odo_core_sample(&o, 100.0f, t);
    }
    NEAR(o.total_m, 124603000.0, 0.5);
    NEAR(dash_odo_core_trip_m(&o), 112400.0, 0.5);

    /* Irregular sample spacing gives the same answer. */
    dash_odo_core_init(&o, 0, 0);
    t = 0;
    static const uint32_t gaps[] = {40, 95, 100, 130, 62, 250, 8, 115};
    for (int i = 0; t <= 600000; i++) {
        dash_odo_core_sample(&o, 60.0f, t);
        t += gaps[i % 8];
    }
    NEAR(o.total_m, 10000.0, 5.0);       /* ten minutes at 60 km/h */

    /* Accelerating 0 to 100 km/h in 10 s covers 138.9 m. */
    dash_odo_core_init(&o, 0, 0);
    for (int i = 0; i <= 100; i++) {
        dash_odo_core_sample(&o, (float)i, (uint32_t)i * 100u);
    }
    NEAR(o.total_m, 138.9, 0.5);

    /* Parked with GPS noise for a day: nothing. */
    dash_odo_core_init(&o, 1000.0, 1000.0);
    for (uint32_t i = 0; i < 864000; i++) {
        dash_odo_core_sample(&o, (i % 3) * 0.6f, i * 100u);   /* 0, 0.6, 1.2 km/h */
    }
    NEAR(o.total_m, 1000.0, 1e-9);

    /* A gap in the feed earns nothing, however fast the car was going. */
    dash_odo_core_init(&o, 0, 0);
    dash_odo_core_sample(&o, 100.0f, 0);
    dash_odo_core_sample(&o, 100.0f, 100);
    dash_odo_core_sample(&o, 100.0f, 30100);    /* 30 s later */
    dash_odo_core_sample(&o, 100.0f, 30200);
    NEAR(o.total_m, 2.0 * 100.0 * 0.1 / 3.6, 0.01);

    /* Changing source: the first sample from the new one is not bridged. */
    dash_odo_core_init(&o, 0, 0);
    dash_odo_core_sample(&o, 80.0f, 0);
    dash_odo_core_break(&o);
    dash_odo_core_sample(&o, 80.0f, 500);
    NEAR(o.total_m, 0.0, 1e-9);
    dash_odo_core_sample(&o, 80.0f, 600);
    NEAR(o.total_m, 80.0 * 0.1 / 3.6, 0.01);

    /* The millisecond clock wrapping. */
    dash_odo_core_init(&o, 0, 0);
    dash_odo_core_sample(&o, 36.0f, 0xFFFFFFFFu - 49u);
    dash_odo_core_sample(&o, 36.0f, 50u);       /* 100 ms later */
    NEAR(o.total_m, 1.0, 0.001);

    /* A kept trip start beyond the odometer is pulled back. */
    dash_odo_core_init(&o, 500.0, 900.0);
    NEAR(dash_odo_core_trip_m(&o), 0.0, 1e-9);

    /* Nonsense speeds add nothing. */
    dash_odo_core_init(&o, 0, 0);
    dash_odo_core_sample(&o, NAN, 0);
    dash_odo_core_sample(&o, -50.0f, 100);
    dash_odo_core_sample(&o, NAN, 200);
    NEAR(o.total_m, 0.0, 1e-9);

    if (s_failed == 0) {
        printf("odometer: all checks passed\n");
        return 0;
    }
    printf("odometer: %d check(s) FAILED\n", s_failed);
    return 1;
}
