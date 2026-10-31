#include "dash_source_sim.h"

#include <math.h>

#include "dash_signals.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dash_sim";

#define SIM_HZ        20
#define SIM_DT        (1.0f / SIM_HZ)

/* Road speed in km/h at 1000 rpm, per gear. Index 0 is unused (neutral). */
static const float kGearKmhPer1000Rpm[] = {0.0f, 7.5f, 13.5f, 20.0f, 27.0f, 34.0f, 42.0f};
#define NUM_GEARS ((int)(sizeof(kGearKmhPer1000Rpm) / sizeof(kGearKmhPer1000Rpm[0])) - 1)

#define IDLE_RPM      820.0f
/* Held to the limiter so the demo exercises the whole tachometer, including
   the warning and redline colours above 6500. */
#define SHIFT_UP_RPM  8000.0f
#define SHIFT_DN_RPM  1600.0f

/* Warning lamps the demo cycles through. Turn signals and headlamps are driven
   by the driving model instead, so they are not in here. */
static const uint32_t kWarnLamps[] = {
    DASH_TT_CHECK_ENGINE,
    DASH_TT_BATTERY,
    DASH_TT_WARNING,
    DASH_TT_BRAKE,
};
#define NUM_WARN_LAMPS ((int)(sizeof(kWarnLamps) / sizeof(kWarnLamps[0])))

#define WARN_ON_MIN_S   3.0f
#define WARN_ON_MAX_S   6.0f
#define WARN_GAP_MIN_S  10.0f
#define WARN_GAP_MAX_S  30.0f

static float rand_range(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(esp_random() >> 8) / (float)(1u << 24));
}

static TaskHandle_t s_task;
static volatile bool s_run;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Pick the gear whose ratio keeps engine speed in a sane band for this road
   speed - a stand-in for a real gearbox model. */
static int select_gear(int gear, float speed_kmh)
{
    if (speed_kmh < 3.0f) {
        return 1;
    }
    float rpm = (speed_kmh / kGearKmhPer1000Rpm[gear]) * 1000.0f;
    if (rpm > SHIFT_UP_RPM && gear < NUM_GEARS) {
        return gear + 1;
    }
    if (rpm < SHIFT_DN_RPM && gear > 1) {
        return gear - 1;
    }
    return gear;
}

static void sim_task(void *arg)
{
    (void)arg;

    float t         = 0.0f;
    float speed     = 0.0f;
    float coolant   = 18.0f;   /* cold start */
    float oil_temp  = 18.0f;
    float fuel      = 78.0f;
    float trip      = 12.4f;
    float odo       = 124503.0f;
    int   gear      = 1;

    /* Warning lamp demo: idle for a gap, light one or two lamps, repeat. */
    uint32_t warn_mask = 0;
    float    warn_until = rand_range(WARN_GAP_MIN_S, WARN_GAP_MAX_S);
    bool     warn_on = false;

    ESP_LOGI(TAG, "simulated drive cycle running at %d Hz", SIM_HZ);

    while (s_run) {
        /* A slow drive cycle: roughly a 100 s loop from a standstill up to
           motorway speed and back down again. */
        const float cycle  = 0.5f - 0.5f * cosf(t * 0.063f);          /* 0..1 */
        const float target = 3.0f + 155.0f * cycle;

        const float error = target - speed;
        /* Brakes bite harder than the engine pulls. */
        const float accel = clampf(error * 0.55f, -22.0f, 9.0f);      /* km/h/s */
        speed = clampf(speed + accel * SIM_DT, 0.0f, 165.0f);

        gear = select_gear(gear, speed);

        float rpm = (speed / kGearKmhPer1000Rpm[gear]) * 1000.0f;
        if (rpm < IDLE_RPM) {
            rpm = IDLE_RPM;
        }
        /* Engine never sits perfectly still. */
        rpm += 25.0f * sinf(t * 9.1f);
        rpm = clampf(rpm, 0.0f, 8000.0f);

        /* Boost tracks how hard the engine is being asked to work; off throttle
           the turbo falls into vacuum. */
        const float demand = clampf(accel / 9.0f, -1.0f, 1.0f);
        const float turbo  = (demand > 0.0f)
                             ? demand * 14.0f * clampf(rpm / 4000.0f, 0.0f, 1.0f)
                             : demand * 6.0f;

        /* Warm up towards thermostat temperature, then hold with a slow cycle
           as the fan comes in and out. */
        const float coolant_target = 89.0f + 3.0f * sinf(t * 0.05f);
        coolant += (coolant_target - coolant) * 0.004f;

        /* Oil runs hotter than coolant and lags it, more so when working hard. */
        const float oil_target = coolant + 12.0f + 18.0f * clampf(rpm / 6000.0f, 0.0f, 1.0f);
        oil_temp += (oil_target - oil_temp) * 0.0025f;

        const float oil = (rpm < 1000.0f) ? 18.0f + rpm * 0.012f
                          : clampf(28.0f + rpm * 0.008f, 0.0f, 80.0f);

        /* Stand-in for an outside-air sensor the board does not have yet. Swept
           across the 4 C ice threshold, roughly every 70 s, so the snowflake
           above the temperature readout demonstrates itself. */
        const float ambient = 5.0f + 7.0f * sinf(t * 0.045f);

        const float km_this_tick = speed * SIM_DT / 3600.0f;
        trip += km_this_tick;
        odo  += km_this_tick;
        fuel  = clampf(fuel - km_this_tick * 0.09f, 0.0f, 100.0f);

        uint32_t tt = 0;
        /* Indicate for a few seconds every half minute, alternating sides,
           blinking at a realistic ~1.4 Hz. */
        const float in_cycle = fmodf(t, 30.0f);
        if (in_cycle < 4.0f && fmodf(t, 0.7f) < 0.35f) {
            tt |= (fmodf(t, 60.0f) < 30.0f) ? DASH_TT_TURN_LEFT : DASH_TT_TURN_RIGHT;
        }
        if (fuel < 10.0f) {
            tt |= DASH_TT_WARNING;
        }

        /*
         * Warning lamps, so the bottom row does something. One or two at a
         * time for a few seconds, then dark for a random 10-30 s. Headlamps
         * stay off unless something switches them on.
         */
        if (t >= warn_until) {
            if (warn_on) {
                warn_mask = 0;
                warn_on = false;
                warn_until = t + rand_range(WARN_GAP_MIN_S, WARN_GAP_MAX_S);
            } else {
                warn_mask = kWarnLamps[esp_random() % NUM_WARN_LAMPS];
                if ((esp_random() & 1u) != 0) {
                    warn_mask |= kWarnLamps[esp_random() % NUM_WARN_LAMPS];
                }
                warn_on = true;
                warn_until = t + rand_range(WARN_ON_MIN_S, WARN_ON_MAX_S);
            }
        }
        tt |= warn_mask;

        const dash_signals_t frame = {
            .speed_kmh        = speed,
            .rpm              = rpm,
            .coolant_temp_c   = coolant,
            .fuel_level_pct   = fuel,
            .oil_temp_c       = oil_temp,
            .ambient_temp_c   = ambient,
            .oil_pressure_psi = oil,
            .turbo_psi        = turbo,
            .trip_km          = trip,
            .odometer_km      = odo,
            .telltales        = tt,
            .gear             = gear,
        };
        dash_signals_publish(&frame);

        t += SIM_DT;
        vTaskDelay(pdMS_TO_TICKS(1000 / SIM_HZ));
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dash_source_sim_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    s_run = true;
    if (xTaskCreate(sim_task, "dash_sim", 4096, NULL, 4, &s_task) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void dash_source_sim_stop(void)
{
    s_run = false;
}
