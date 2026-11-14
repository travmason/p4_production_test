#include "dash_inputs.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "dash_debounce.h"
#include "dash_signals.h"
#include "dash_source.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash_inputs";

/*
 * Pin assignments. All on the EXT1 header, all otherwise unused, none of them
 * strapping pins. Change the wiring here and nowhere else.
 *
 * active_low is true for the optocoupler wiring in dash_inputs.h: the pin sits
 * high on its pull-up and goes low when the lamp is on.
 */
typedef struct {
    gpio_num_t  gpio;
    uint32_t    telltale;
    bool        active_low;
    const char *name;
} input_def_t;

static const input_def_t kInputs[] = {
    {GPIO_NUM_17, DASH_TT_TURN_LEFT,  true, "left indicator"},    /* EXT1 pin 11 */
    {GPIO_NUM_18, DASH_TT_TURN_RIGHT, true, "right indicator"},   /* EXT1 pin 12 */
    {GPIO_NUM_19, DASH_TT_HEADLIGHT,  true, "headlights"},        /* EXT1 pin 13 */
    {GPIO_NUM_33, DASH_TT_HIGH_BEAM,  true, "high beam"},         /* EXT1 pin 16 */
    {GPIO_NUM_46, DASH_TT_BRAKE,      true, "handbrake / brake"}, /* EXT1 pin 18 */
    /* GPIO47 and GPIO48 (EXT1 pins 19 and 20) are free for two more. Oil
       pressure is not here: it comes from the ECU's sensor over CAN. */
};
#define NUM_INPUTS ((int)(sizeof(kInputs) / sizeof(kInputs[0])))

/*
 * Sampled every 10 ms; a change has to hold for three samples. That is quick
 * enough to follow a flasher exactly - an indicator on the dash should blink
 * with the lamp, not on a timer of its own - and slow enough to ignore the
 * spikes a car's wiring is full of.
 */
#define POLL_MS         10
#define DEBOUNCE_COUNT  3

static atomic_uint s_lamps;

static void inputs_task(void *arg)
{
    (void)arg;

    dash_debounce_t db[NUM_INPUTS] = {0};
    uint32_t all = 0;
    for (int i = 0; i < NUM_INPUTS; i++) {
        all |= kInputs[i].telltale;
    }

    while (true) {
        uint32_t on = 0;
        for (int i = 0; i < NUM_INPUTS; i++) {
            const bool level = gpio_get_level(kInputs[i].gpio) != 0;
            const bool raw = kInputs[i].active_low ? !level : level;
            if (dash_debounce_step(&db[i], raw, DEBOUNCE_COUNT)) {
                on |= kInputs[i].telltale;
            }
        }
        atomic_store(&s_lamps, on);

        /*
         * Written every pass rather than on change: switching from simulated
         * to live wipes the signal model, and this puts the real lamps back
         * within one sample. While the simulator has the gauges its made-up
         * lamps are left alone.
         */
        if (dash_source_live_writes_allowed()) {
            dash_signals_set_telltale(on, true);
            dash_signals_set_telltale(all & ~on, false);
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t dash_inputs_start(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < NUM_INPUTS; i++) {
        mask |= 1ULL << kInputs[i].gpio;
    }
    const gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        /* Pulled to the "off" level, so an input with nothing wired to it
           reads off. The internal pull-up is weak (tens of kOhm); on a long
           lead add an external 4.7 kOhm to 3.3 V. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(inputs_task, "dash_inputs", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < NUM_INPUTS; i++) {
        ESP_LOGI(TAG, "GPIO%d: %s", kInputs[i].gpio, kInputs[i].name);
    }
    return ESP_OK;
}

uint32_t dash_inputs_lamps(void)
{
    return atomic_load(&s_lamps);
}
