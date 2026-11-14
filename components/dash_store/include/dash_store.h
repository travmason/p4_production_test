/*
 * dash_store - the dashboard's few kept values, in NVS.
 *
 * One namespace, small integers, every one with a default: the data source
 * mode, the odometer, the trip start, and whether a GPS node has been seen.
 * Safe from any task. Writes take milliseconds and now and then much longer
 * (a flash page erase), so never write from a task that must not stall.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Returns the stored value, or @p fallback if there is none or NVS is down. */
uint32_t dash_store_get_u32(const char *key, uint32_t fallback);

/** Returns false if it could not be written. */
bool dash_store_set_u32(const char *key, uint32_t value);

#ifdef __cplusplus
}
#endif
