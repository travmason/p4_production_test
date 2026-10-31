/*
 * dash_source_sim - a synthetic vehicle, so the cluster has something to show
 * before it is wired to a real bus.
 *
 * Implements the producer half of dash_signals. Replacing this with a CAN or
 * OBD-II reader should require no changes to the UI.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the simulator task. dash_signals_init() must have run first. */
esp_err_t dash_source_sim_start(void);

/** Stop the simulator task, leaving the last values in place. */
void dash_source_sim_stop(void);

#ifdef __cplusplus
}
#endif
