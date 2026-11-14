/*
 * dash_source - decides where the gauges get their data.
 *
 * Two producers can feed dash_signals: the simulator (dash_source_sim) and the
 * ECU (dash_can). Only one may write at a time, and this component is the one
 * place that decides which. It starts and stops the simulator, tells dash_can
 * whether its values may go through, keeps the chosen mode in flash, and tells
 * the UI what to show in the source chip.
 *
 * Modes:
 *
 *   AUTO  simulated until the ECU is first heard, then live for the rest of
 *         the power cycle. It never falls back to simulated data by itself:
 *         made-up numbers must not replace real ones on a moving car.
 *   SIM   always simulated, even with the ECU talking.
 *   LIVE  never simulated. With no ECU the gauges read zero and the chip says
 *         NO ECU. This is the mode to leave the car in.
 *
 * Requests can come from any task (touch, the button, CAN). They are applied by
 * this component's own task within about a tenth of a second.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DASH_SOURCE_AUTO = 0,
    DASH_SOURCE_SIM,
    DASH_SOURCE_LIVE,
    DASH_SOURCE_NUM_MODES,
} dash_source_mode_t;

/** What is actually driving the gauges right now. */
typedef enum {
    DASH_SHOWING_SIM = 0,
    DASH_SHOWING_LIVE,
    DASH_SHOWING_NO_ECU,   /*!< live selected, but the ECU is silent */
} dash_source_showing_t;

/**
 * Load the saved mode and start managing the sources.
 *
 * @param live_online  returns true while the ECU is being heard; called from
 *                     this component's task. NULL means "never".
 *
 * dash_signals_init() must have run. Starts the simulator itself if the mode
 * calls for it, so nothing else should call dash_source_sim_start().
 */
esp_err_t dash_source_start(bool (*live_online)(void));

/** Select a mode. Saved to flash. Safe from any task; out-of-range is ignored. */
void dash_source_set_mode(dash_source_mode_t mode);

/** AUTO -> SIM -> LIVE -> AUTO. Safe from any task. */
void dash_source_cycle_mode(void);

dash_source_mode_t dash_source_get_mode(void);

dash_source_showing_t dash_source_showing(void);

/** For the live producer: may it write to dash_signals at the moment? */
bool dash_source_live_writes_allowed(void);

const char *dash_source_mode_name(dash_source_mode_t mode);

#ifdef __cplusplus
}
#endif
