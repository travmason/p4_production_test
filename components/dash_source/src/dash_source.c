#include "dash_source.h"

#include <stdatomic.h>

#include "dash_signals.h"
#include "dash_source_sim.h"
#include "dash_source_store.h"
#include "dash_ui.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash_source";

#define TICK_MS          100
/* The simulator publishes every 50 ms and checks its stop flag once per
   publish, so this is comfortably after its last frame. */
#define SIM_SETTLE_MS    150

static bool (*s_live_online)(void);

static atomic_int  s_mode_req = DASH_SOURCE_AUTO;   /* what was last asked for */
static atomic_int  s_showing = DASH_SHOWING_SIM;
static atomic_bool s_live_writes;
static bool        s_started;

const char *dash_source_mode_name(dash_source_mode_t mode)
{
    switch (mode) {
    case DASH_SOURCE_AUTO: return "auto";
    case DASH_SOURCE_SIM:  return "simulated";
    case DASH_SOURCE_LIVE: return "live";
    default:               return "?";
    }
}

static void wipe_signals(void)
{
    const dash_signals_t blank = {0};
    dash_signals_publish(&blank);
}

static void source_task(void *arg)
{
    (void)arg;

    int  mode = -1;            /* the mode in force; -1 forces the first pass */
    bool heard_live = false;   /* AUTO: latched once the ECU has spoken */
    bool sim_on = false;       /* the simulator is running */
    bool sim_wanted_prev = false;
    bool first = true;

    while (true) {
        const int  req = atomic_load(&s_mode_req);
        const bool online = s_live_online != NULL && s_live_online();

        if (req != mode) {
            if (mode != -1) {
                dash_source_store_save(req);
            }
            mode = req;
            /* Choosing AUTO afresh starts its latch again. */
            heard_live = false;
            ESP_LOGI(TAG, "source mode: %s", dash_source_mode_name((dash_source_mode_t)mode));
        }
        if (online) {
            heard_live = true;
        }

        const bool sim_wanted = mode == DASH_SOURCE_SIM ||
                                (mode == DASH_SOURCE_AUTO && !heard_live);

        if (sim_wanted) {
            /* Shut the live side out before the simulator starts writing. */
            atomic_store(&s_live_writes, false);
            if (!sim_on) {
                /* Fails for a moment if a previous run is still winding down;
                   the next tick tries again. */
                sim_on = dash_source_sim_start() == ESP_OK;
                if (sim_on) {
                    ESP_LOGI(TAG, "showing simulated data");
                }
            }
        } else if (first || sim_wanted_prev) {
            if (sim_on) {
                dash_source_sim_stop();
                sim_on = false;
                vTaskDelay(pdMS_TO_TICKS(SIM_SETTLE_MS));
            }
            /* Nothing the simulator made up may outlive it: lamps, odometer
               and outside temperature have no live source to overwrite them. */
            wipe_signals();
            atomic_store(&s_live_writes, true);
            ESP_LOGI(TAG, "showing live data");
        }
        sim_wanted_prev = sim_wanted;
        first = false;

        const int showing = sim_wanted ? DASH_SHOWING_SIM
                            : online   ? DASH_SHOWING_LIVE
                            :            DASH_SHOWING_NO_ECU;
        atomic_store(&s_showing, showing);
        dash_ui_set_source_chip((dash_ui_source_chip_t)showing);

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t dash_source_start(bool (*live_online)(void))
{
    if (s_started) {
        return ESP_OK;
    }
    s_live_online = live_online;

    int saved = dash_source_store_load(DASH_SOURCE_AUTO);
    if (saved < 0 || saved >= DASH_SOURCE_NUM_MODES) {
        saved = DASH_SOURCE_AUTO;
    }
    atomic_store(&s_mode_req, saved);

    if (xTaskCreate(source_task, "dash_source", 4096, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

void dash_source_set_mode(dash_source_mode_t mode)
{
    if ((int)mode < 0 || mode >= DASH_SOURCE_NUM_MODES) {
        return;
    }
    atomic_store(&s_mode_req, (int)mode);
}

void dash_source_cycle_mode(void)
{
    int cur = atomic_load(&s_mode_req);
    while (!atomic_compare_exchange_weak(&s_mode_req, &cur,
                                         (cur + 1) % DASH_SOURCE_NUM_MODES)) {
    }
}

dash_source_mode_t dash_source_get_mode(void)
{
    return (dash_source_mode_t)atomic_load(&s_mode_req);
}

dash_source_showing_t dash_source_showing(void)
{
    return (dash_source_showing_t)atomic_load(&s_showing);
}

bool dash_source_live_writes_allowed(void)
{
    return atomic_load(&s_live_writes);
}
