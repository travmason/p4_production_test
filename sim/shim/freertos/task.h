#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

/* Stack depth and priority are accepted and ignored. */
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, int priority, TaskHandle_t *out_handle);

void vTaskDelay(TickType_t ticks);

/* Only vTaskDelete(NULL) - a task ending itself - is supported. */
void vTaskDelete(TaskHandle_t task);
