/* pthread-backed implementations of the few FreeRTOS calls the dashboard uses. */

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

struct sim_mutex {
    pthread_mutex_t m;
};

typedef struct {
    TaskFunction_t fn;
    void          *arg;
} task_start_t;

static void *task_trampoline(void *p)
{
    task_start_t start = *(task_start_t *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_depth,
                       void *arg, int priority, TaskHandle_t *out_handle)
{
    (void)name;
    (void)stack_depth;
    (void)priority;

    task_start_t *start = malloc(sizeof(*start));
    if (start == NULL) {
        return pdFAIL;
    }
    start->fn = fn;
    start->arg = arg;

    pthread_t th;
    if (pthread_create(&th, NULL, task_trampoline, start) != 0) {
        free(start);
        return pdFAIL;
    }
    pthread_detach(th);
    if (out_handle != NULL) {
        *out_handle = (TaskHandle_t)th;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    usleep((useconds_t)ticks * 1000u);
}

void vTaskDelete(TaskHandle_t task)
{
    if (task == NULL) {
        pthread_exit(NULL);
    }
}

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct sim_mutex *m = malloc(sizeof(*m));
    if (m != NULL) {
        pthread_mutex_init(&m->m, NULL);
    }
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t ticks)
{
    (void)ticks;
    return pthread_mutex_lock(&m->m) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m)
{
    return pthread_mutex_unlock(&m->m) == 0 ? pdTRUE : pdFALSE;
}
