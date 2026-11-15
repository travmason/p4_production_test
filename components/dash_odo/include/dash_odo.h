/*
 * dash_odo - the odometer and trip meter, kept across power cycles.
 *
 * The dashboard is the instrument, so the distance lives here rather than in
 * the GPS node or the ECU: either can be replaced without losing it. It is
 * built up from speed samples (see dash_odo_core.h) and saved to flash by this
 * component's own task, so the task feeding it never waits on a flash write.
 *
 * Saved every 500 m while moving and a few seconds after stopping, which keeps
 * flash wear trivial (a few thousand page erases over 200,000 km) and loses at
 * most 500 m if power is cut at speed.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The car's odometer reading when this dashboard was fitted.
 *
 * Written to flash the first time the firmware runs on a board that has no
 * odometer kept yet, and never again: from then on the kept value is the
 * odometer, and re-flashing the firmware does not bring this number back. To
 * change the reading on a board that already has one, use
 * dash_odo_set_total_km() (CAN command 0x31, see dash_can.h) or erase the
 * flash (`idf.py erase-flash`) before flashing.
 */
#ifndef DASH_ODO_INITIAL_KM
#define DASH_ODO_INITIAL_KM  83483u
#endif

/** Load the kept values and start the saving task. */
esp_err_t dash_odo_start(void);

/** Add a speed sample. Safe from any task; never blocks on flash. */
void dash_odo_sample(float speed_kmh, uint32_t now_ms);

/** The speed feed stopped or changed source. */
void dash_odo_break(void);

void dash_odo_get(float *odometer_km, float *trip_km);

/** Zero the trip meter. Safe from any task. */
void dash_odo_reset_trip(void);

/** Set the odometer outright, for commissioning. The trip is zeroed too. */
void dash_odo_set_total_km(uint32_t km);

#ifdef __cplusplus
}
#endif
