/*
 * Host stand-in for FreeRTOS, covering only what the dashboard code calls.
 * Tasks are pthreads and mutexes are pthread mutexes; see ../shim.c.
 *
 * One tick is one millisecond, matching the firmware's CONFIG_FREERTOS_HZ=1000.
 */
#pragma once

#include <stddef.h>   /* the real header brings NULL in too, and code relies on it */
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;

#define pdFALSE        0
#define pdTRUE         1
#define pdFAIL         0
#define pdPASS         1
#define portMAX_DELAY  ((TickType_t)0xFFFFFFFFu)

#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
