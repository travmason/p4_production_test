/*
 * Debounce for a slow on/off input. Pure C, tested on the host.
 *
 * The output changes only after the raw input has disagreed with it for
 * `need` samples in a row, so a spike shorter than that never gets through.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool    state;   /*!< the debounced value */
    uint8_t run;     /*!< consecutive samples that disagreed with it */
} dash_debounce_t;

/** Feed one raw sample; returns the debounced value. */
bool dash_debounce_step(dash_debounce_t *d, bool raw, uint8_t need);
