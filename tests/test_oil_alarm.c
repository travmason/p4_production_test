/* Host test for the oil pressure alarm. Run with `ctest --test-dir sim/build`. */

#include <stdio.h>

#include "dash_oil_alarm.h"

static int s_failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            s_failed++;                                                 \
        }                                                               \
    } while (0)

static dash_oil_alarm_t a;
static uint32_t t;

/* Hold a reading for ms, in 50 ms steps; returns the final alarm state. */
static bool hold(float rpm, float psi, uint32_t ms)
{
    bool on = a.active;
    for (uint32_t i = 0; i < ms; i += 50) {
        t += 50;
        on = dash_oil_alarm_step(&a, rpm, psi, t);
    }
    return on;
}

static void reset(uint32_t start)
{
    a = (dash_oil_alarm_t) {0};
    t = start;
}

int main(void)
{
    /* Engine stopped: no pressure, no alarm, however long. */
    reset(0);
    CHECK(!hold(0, 0, 10000));
    CHECK(!hold(200, 0, 5000));     /* cranking */

    /* A normal start: pressure takes a second to arrive. */
    CHECK(!hold(900, 2, 1000));
    CHECK(!hold(900, 45, 5000));

    /* Pressure lost at idle: trips after the hold time, not before. */
    CHECK(!hold(900, 5, 400));
    CHECK(hold(900, 5, 200));
    /* Just over the limit is not enough to clear it... */
    CHECK(hold(900, 12, 1000));
    /* ...clearly over is. */
    CHECK(!hold(900, 15, 100));

    /* A short dip is ignored. */
    CHECK(!hold(900, 3, 300));
    CHECK(!hold(900, 40, 300));
    CHECK(!hold(900, 3, 300));
    CHECK(!hold(900, 40, 300));

    /* Fine at idle, not fine at speed: 18 psi. */
    CHECK(!hold(900, 18, 2000));
    CHECK(hold(4000, 18, 600));
    CHECK(hold(4000, 27, 600));     /* inside the clear margin */
    CHECK(!hold(4000, 30, 100));

    /* Stopping the engine clears it and re-arms the start grace. */
    CHECK(hold(900, 0, 1000));
    CHECK(!hold(0, 0, 100));
    CHECK(!hold(900, 0, 1900));     /* never built pressure: still in grace */
    CHECK(hold(900, 0, 200));       /* grace over */

    /* The millisecond clock wrapping changes nothing. */
    reset(0xFFFFFFFFu - 2500u);
    CHECK(!hold(900, 45, 2400));
    CHECK(!hold(900, 5, 400));      /* crosses the wrap */
    CHECK(hold(900, 5, 200));

    if (s_failed == 0) {
        printf("oil alarm: all checks passed\n");
        return 0;
    }
    printf("oil alarm: %d check(s) FAILED\n", s_failed);
    return 1;
}
