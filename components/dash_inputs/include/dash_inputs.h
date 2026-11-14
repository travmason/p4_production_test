/*
 * dash_inputs - the car's lamp circuits, read directly.
 *
 * Indicators, headlights and a few warning switches are 12 V circuits that have
 * nothing to do with the ECU. Each one drives an optocoupler whose output pulls
 * a GPIO on the EXT1 header to ground, and this component turns those pins into
 * telltales. Because nothing else is involved, the lamps on the dash keep
 * working with the ECU, the CAN bus or the GPS node down.
 *
 * WIRING, per input
 * -----------------
 *
 *   car side                             dash side
 *
 *   lamp feed (+12 V when on) --[R]--+        +---- GPIO (internal pull-up)
 *                                    |        |
 *                                 anode    collector
 *                                   LED  >>  transistor     (PC817 or similar)
 *                                 cathode  emitter
 *                                    |        |
 *   chassis ground ------------------+        +---- board GND (EXT1 pin 3)
 *
 *   R = 1 kOhm, 0.5 W or more. That is about 11 mA at 14 V. Ready-made
 *   "12 V optocoupler isolation" boards already have it fitted.
 *
 * A lit lamp turns the LED on, the transistor pulls the pin low, and the dash
 * reads "on". Nothing on the car side connects to the board electrically.
 *
 * For a circuit that is switched on the ground side instead - an oil pressure
 * switch or a handbrake switch, which connect the lamp to ground - put the LED
 * in series the other way round: anode to switched +12 V (ignition) through R,
 * cathode to the switch wire. The dash side is the same.
 *
 * The pin assignments are the table at the top of dash_inputs.c.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configure the pins and start reading them. dash_signals_init() first. */
esp_err_t dash_inputs_start(void);

/** The debounced lamps that are on right now, as dash_telltale_t bits. */
uint32_t dash_inputs_lamps(void);

#ifdef __cplusplus
}
#endif
