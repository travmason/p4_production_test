#pragma once

#include "freertos/FreeRTOS.h"

typedef struct sim_mutex *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);

/* The timeout is ignored: the dashboard only ever waits forever. */
BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t m);
