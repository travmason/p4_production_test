/*
 * The odometer's arithmetic, apart from where it is kept. Pure C, tested on
 * the host (tests/test_odo.c).
 *
 * Distance is speed integrated over the time between samples. Whoever supplies
 * speed - the GPS node, or the ECU when there is no GPS - supplies the samples.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A gap between samples longer than this earns nothing: the feed stopped, and
   guessing what happened in the gap is how odometers gain phantom distance. */
#define DASH_ODO_MAX_GAP_MS      1000u
/* Below this the car is treated as stopped. A stationary GPS receiver reports
   a few tenths of a km/h of noise, which would otherwise creep the odometer
   along while parked. */
#define DASH_ODO_MIN_SPEED_KMH   1.5f

typedef struct {
    double   total_m;        /*!< the odometer */
    double   trip_start_m;   /*!< odometer reading when the trip was reset */
    uint32_t last_ms;
    float    last_speed_kmh;
    bool     have_last;
} dash_odo_core_t;

/** Start from kept values. */
void dash_odo_core_init(dash_odo_core_t *o, double total_m, double trip_start_m);

/** Add a speed sample taken at @p now_ms (any forward-only millisecond clock). */
void dash_odo_core_sample(dash_odo_core_t *o, float speed_kmh, uint32_t now_ms);

/** The speed feed stopped or changed hands: do not bridge to the next sample. */
void dash_odo_core_break(dash_odo_core_t *o);

double dash_odo_core_trip_m(const dash_odo_core_t *o);
