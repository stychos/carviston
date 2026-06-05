#include "heater_control.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"
#include "benchmark.h"
#include "buttons.h"
#include "event_log.h"
#include "leds.h"
#include "relays.h"
#include "safety.h"
#include "temperature.h"

static const char *TAG = "heater";

#define TICK_MS  1000   /* heavy control cadence (read temp, evaluate, command) */
#define INPUT_POLL_MS  50   /* button/combo servicing cadence — keeps +/- snappy */

static SemaphoreHandle_t s_lock;
static heater_state_t s_state;

/* Mode-private state.
 *
 * `initialized` distinguishes "just entered this mode, load the opening
 * phase" from "ongoing — decrement and roll over on boundary". Without it
 * the boundary block (phase_seconds_left == 0) would fire on the very
 * first tick after reset_mode_state(), making FAST/ECO start in REST and
 * making OPTIMAL fire heater 2 first.
 *
 * `mode` is the mode this state belongs to — when the user changes mode
 * we detect it here and re-initialise. */
typedef struct {
    bool     initialized;
    uint8_t  mode;
    uint32_t phase_seconds_left;
    uint8_t  phase;        /* index within mode-specific phase list */
    uint8_t  current_relay;/* for optimal/eco: which heater is currently energised */
} mode_state_t;
static mode_state_t s_mode_state;

static const uint8_t TEMP_STEPS[] = { 40, 50, 60, 70, 80 };

static void reset_mode_state(void)
{
    memset(&s_mode_state, 0, sizeof(s_mode_state));
}

/* --- external setters --- */

void heater_set_target(uint8_t celsius)
{
    /* clamp to allowed steps */
    uint8_t chosen = 60;
    for (size_t i = 0; i < sizeof(TEMP_STEPS); ++i) {
        if (TEMP_STEPS[i] == celsius) { chosen = celsius; break; }
    }
    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.target_temp_c != chosen) {
        cfg.target_temp_c = chosen;
        /* Deferred — rapid setpoint changes (e.g. dragging the web UI slider)
         * would each trigger an inline nvs_commit and stall TCP keepalives
         * during a sector erase. */
        app_config_save_deferred(&cfg);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.target_c = chosen;
    xSemaphoreGive(s_lock);
    benchmark_note_target_change(chosen);
    event_log_emit(EV_TARGET_CHANGE, chosen, 0, NULL);
    /* The +/- press handler also calls leds_target_preview_press so that the
     * preview survives across multiple taps. From the web API path the
     * preview LED briefly shows the new target as well. */
    leds_target_preview_press(chosen);
    leds_target_preview_release();
}

void heater_set_mode(heating_mode_t mode)
{
    heater_atomic_swap_mode(mode, NULL);
}

void heater_atomic_swap_mode(heating_mode_t new_mode, heating_mode_t *prev_out)
{
    if (new_mode >= HEATING_MODE_COUNT) {
        if (prev_out) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            *prev_out = s_state.mode;
            xSemaphoreGive(s_lock);
        }
        return;
    }

    heating_mode_t prev;
    bool changed;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    prev = s_state.mode;
    changed = (prev != new_mode);
    if (changed) {
        s_state.mode = new_mode;
        reset_mode_state();
    }
    xSemaphoreGive(s_lock);

    if (prev_out) *prev_out = prev;

    if (changed) {
        /* Persist + telemetry outside the lock. Deferred save → rapid mode
         * cycling isn't stalled by a sector erase. */
        app_config_t cfg;
        app_config_get(&cfg);
        if (cfg.heating_mode != new_mode) {
            cfg.heating_mode = new_mode;
            app_config_save_deferred(&cfg);
        }
        benchmark_note_mode_change((uint8_t)new_mode);
        event_log_emit(EV_MODE_CHANGE, (int16_t)new_mode, 0, NULL);
    }
}

/* Persist the latest master on/off so restore_power_on_boot can bring it back
 * after a reboot. Always recorded (cheap: coalesced by app_config_save_deferred
 * and only at human cadence) so enabling the option later restores the real
 * current state. Must be called WITHOUT s_lock held — app_config has its own
 * mutex. */
static void persist_power_state(bool on)
{
    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.last_power_state != on) {
        cfg.last_power_state = on;
        app_config_save_deferred(&cfg);
    }
}

void heater_set_master_enabled(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool prev_any = s_state.element_enabled[0] || s_state.element_enabled[1];
    s_state.element_enabled[0] = on;
    s_state.element_enabled[1] = on;
    s_state.master_enabled     = on;
    if (!on) reset_mode_state();
    xSemaphoreGive(s_lock);
    if (!on) relays_all_off();
    if (prev_any != on) event_log_emit(on ? EV_MASTER_ON : EV_MASTER_OFF, 0, 0, NULL);
    persist_power_state(on);
}

void heater_toggle_master(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool any = s_state.element_enabled[0] || s_state.element_enabled[1];
    xSemaphoreGive(s_lock);
    heater_set_master_enabled(!any);
}

void heater_set_element_enabled(uint8_t channel, bool on)
{
    if (channel >= 2) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.element_enabled[channel] == on) {
        xSemaphoreGive(s_lock);
        return;
    }
    bool prev_any = s_state.element_enabled[0] || s_state.element_enabled[1];
    s_state.element_enabled[channel] = on;
    bool now_any = s_state.element_enabled[0] || s_state.element_enabled[1];
    s_state.master_enabled = now_any;
    if (!now_any) reset_mode_state();
    xSemaphoreGive(s_lock);
    if (!now_any) relays_all_off();
    /* Don't emit EV_HEATER_ON/OFF here: that event means "a relay actually
     * energised", which only the control-loop tick can decide (enabling an
     * element while in standby closes no relay). Emitting it on the user's
     * enable-toggle too would give the log two producers of the same event
     * with the same args. We log only the master on/off transition. */
    if (prev_any != now_any) {
        event_log_emit(now_any ? EV_MASTER_ON : EV_MASTER_OFF, 0, 0, NULL);
        persist_power_state(now_any);
    }
}

void heater_toggle_element(uint8_t channel)
{
    if (channel >= 2) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_state.element_enabled[channel];
    xSemaphoreGive(s_lock);
    heater_set_element_enabled(channel, !was);
}

void heater_clear_safety_fault(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_on = s_state.element_enabled[0] || s_state.element_enabled[1];
    s_state.element_enabled[0] = false;
    s_state.element_enabled[1] = false;
    s_state.master_enabled     = false;
    reset_mode_state();
    xSemaphoreGive(s_lock);
    relays_all_off();
    if (was_on) { event_log_emit(EV_MASTER_OFF, 0, 0, NULL); persist_power_state(false); }
    /* Drop the latch only AFTER we've forced master off and dropped relays.
     * If the underlying condition still holds, the next safety_evaluate()
     * tick will re-latch before any heating decision is made. */
    safety_clear_fault();
}

void heater_get_state(heater_state_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

/* --- control logic --- */

/* Returns true if a tank at `temp_c` should be in a "heating needed" cycle.
 * Implements hysteresis off the tank's own current relay state. */
static bool update_heating_phase(bool currently_heating, float temp_c,
                                 uint8_t target, uint8_t hyst)
{
    if (isnan(temp_c)) return false;
    if (currently_heating) {
        return temp_c < (float)target;
    }
    return temp_c <= (float)target - (float)hyst;
}

/* Each relay is bound to its own tank: relay/tank 0 = inlet, 1 = outlet.
 * `wants[i]` is set by the caller iff tank i is enabled, healthy, and below
 * target (its own hysteresis). The mode here is purely a POWER POLICY layered
 * on top — it never forces an element on whose tank doesn't want heat, so a
 * tank that has reached target stops while the colder tank keeps heating.
 *
 *   SUPER_FAST / FAST : both elements may run at once (FAST adds an on/rest
 *                       duty cycle); each still gated by its own tank.
 *   OPTIMAL / ECO     : at most ONE element at a time (caps current draw),
 *                       alternating between tanks; ECO adds a rest phase.
 *                       If the scheduled tank is already at target, the slot
 *                       is handed to the other tank that still wants heat.
 *
 * Runs the mode state machine for one tick. Caller holds s_lock. */
static void step_mode(const app_config_t *cfg, const bool wants[RELAY_COUNT],
                      bool out_relays[RELAY_COUNT], const char **phase_name)
{
    out_relays[0] = false;
    out_relays[1] = false;
    *phase_name = "idle";

    bool any_want = wants[0] || wants[1];
    if (!any_want) {
        /* Both tanks at target: pause but PRESERVE the mode state. When a tank
         * cools and heating resumes, OPTIMAL continues the current slot, ECO
         * continues its phase, FAST resumes its on/rest window. Resetting here
         * would restart the alternation pattern each cycle and (for ECO) lose
         * any mandated rest period. */
        *phase_name = "target reached";
        return;
    }

    /* Re-init when entering a fresh mode (heater_set_mode resets s_mode_state,
     * so initialized is false). Loading the opening phase HERE (rather than
     * in reset_mode_state) gives us access to cfg without plumbing it through. */
    if (!s_mode_state.initialized || s_mode_state.mode != (uint8_t)s_state.mode) {
        s_mode_state.initialized = true;
        s_mode_state.mode = (uint8_t)s_state.mode;
        s_mode_state.phase = 0;
        s_mode_state.current_relay = 0;
        switch (s_state.mode) {
        case HEATING_MODE_FAST:
            s_mode_state.phase_seconds_left = (uint32_t)cfg->fast_on_min * 60u; break;
        case HEATING_MODE_OPTIMAL:
            s_mode_state.phase_seconds_left = (uint32_t)cfg->optimal_swap_min * 60u; break;
        case HEATING_MODE_ECO:
            s_mode_state.phase_seconds_left = (uint32_t)cfg->eco_on_min * 60u; break;
        default:
            s_mode_state.phase_seconds_left = 0; break;
        }
    } else if (s_mode_state.phase_seconds_left > 0) {
        s_mode_state.phase_seconds_left--;
    }

    switch (s_state.mode) {
    case HEATING_MODE_SUPER_FAST:
        out_relays[0] = wants[0];
        out_relays[1] = wants[1];
        *phase_name = "heating";
        break;

    case HEATING_MODE_FAST: {
        /* phase 0: both eligible for N min. phase 1: rest R min.
         * Boundary fires only when an actual phase runs out — the entry
         * block above pre-loaded fast_on_min for the opening tick. */
        if (s_mode_state.phase_seconds_left == 0) {
            s_mode_state.phase ^= 1;
            uint32_t mins = (s_mode_state.phase == 0)
                            ? cfg->fast_on_min : cfg->fast_rest_min;
            s_mode_state.phase_seconds_left = (uint32_t)mins * 60u;
        }
        if (s_mode_state.phase == 0) {
            out_relays[0] = wants[0];
            out_relays[1] = wants[1];
            *phase_name = "heating";
        } else {
            *phase_name = "rest";
        }
        break;
    }

    case HEATING_MODE_OPTIMAL: {
        if (s_mode_state.phase_seconds_left == 0) {
            s_mode_state.current_relay ^= 1;
            s_mode_state.phase_seconds_left = (uint32_t)cfg->optimal_swap_min * 60u;
        }
        /* Serve the scheduled tank; if it's already satisfied, give its slot
         * to the other tank rather than idling. */
        uint8_t r = s_mode_state.current_relay;
        if (!wants[r]) r ^= 1;
        if (wants[r]) {
            out_relays[r] = true;
            *phase_name = (r == 0) ? "inlet tank" : "outlet tank";
        } else {
            *phase_name = "target reached";
        }
        break;
    }

    case HEATING_MODE_ECO: {
        /* phase 0: one tank on for N min. phase 1: rest R min, then swap. */
        if (s_mode_state.phase_seconds_left == 0) {
            if (s_mode_state.phase == 0) {
                s_mode_state.phase = 1;
                s_mode_state.phase_seconds_left = (uint32_t)cfg->eco_rest_min * 60u;
            } else {
                s_mode_state.phase = 0;
                s_mode_state.current_relay ^= 1;
                s_mode_state.phase_seconds_left = (uint32_t)cfg->eco_on_min * 60u;
            }
        }
        if (s_mode_state.phase == 0) {
            uint8_t r = s_mode_state.current_relay;
            if (!wants[r]) r ^= 1;
            if (wants[r]) {
                out_relays[r] = true;
                *phase_name = (r == 0) ? "inlet tank" : "outlet tank";
            } else {
                *phase_name = "target reached";
            }
        } else {
            *phase_name = "rest";
        }
        break;
    }

    case HEATING_MODE_COUNT:
    default:
        break;
    }
}

/* Helpers for the +/- preview + step. */
static uint8_t next_temp_up(uint8_t cur)
{
    for (size_t i = 0; i < sizeof(TEMP_STEPS); ++i) {
        if (TEMP_STEPS[i] > cur) return TEMP_STEPS[i];
    }
    return cur;
}
static uint8_t next_temp_down(uint8_t cur)
{
    for (int i = (int)sizeof(TEMP_STEPS) - 1; i >= 0; --i) {
        if (TEMP_STEPS[i] < cur) return (uint8_t)TEMP_STEPS[i];
    }
    return cur;
}

#include "wifi_mgr.h"     /* wifi_mgr_force_ap_mode */

static void process_button(const button_event_t *ev)
{
    button_id_t id = (button_id_t)ev->id;
    button_kind_t k = (button_kind_t)ev->kind;

    if (k == BTN_KIND_PRESS) event_log_emit(EV_BUTTON_PRESS, (int16_t)id, 0, NULL);
    else if (k == BTN_KIND_LONG) event_log_emit(EV_BUTTON_LONG, (int16_t)id, 0, NULL);

    switch (id) {
    case BTN_POWER:
        if (k == BTN_KIND_PRESS) {
            heater_toggle_master();
            ESP_LOGI(TAG, "BTN power → master=%d", (int)s_state.master_enabled);
        }
        break;

    case BTN_ECO:
        if (k == BTN_KIND_LONG) {
            ESP_LOGI(TAG, "BTN eco long-press → force AP mode");
            leds_animate_ap_mode();
            wifi_mgr_force_ap_mode();
        }
        break;

    case BTN_SHOWER:
        if (k == BTN_KIND_LONG) {
            heating_mode_t next = (heating_mode_t)((s_state.mode + 1) % HEATING_MODE_COUNT);
            heater_set_mode(next);
            leds_animate_mode((uint8_t)next);
            ESP_LOGI(TAG, "BTN shower long-press → mode=%d", (int)next);
        }
        break;

    case BTN_PLUS:
        if (k == BTN_KIND_PRESS) {
            uint8_t cur = s_state.target_c;
            uint8_t next = next_temp_up(cur);
            if (next != cur) heater_set_target(next);
            leds_target_preview_press(s_state.target_c);
        } else if (k == BTN_KIND_RELEASE) {
            /* only release the linger timer when the *other* +/- isn't held */
            if (!buttons_is_held(BTN_MINUS)) leds_target_preview_release();
        }
        break;

    case BTN_MINUS:
        if (k == BTN_KIND_PRESS) {
            uint8_t cur = s_state.target_c;
            uint8_t next = next_temp_down(cur);
            if (next != cur) heater_set_target(next);
            leds_target_preview_press(s_state.target_c);
        } else if (k == BTN_KIND_RELEASE) {
            if (!buttons_is_held(BTN_PLUS)) leds_target_preview_release();
        }
        break;

    case BTN_COUNT:
    default:
        break;
    }
}

static void control_task(void *arg)
{
    (void)arg;
    QueueHandle_t btn_q = buttons_event_queue();
    led_panel_state_t led;

    int64_t next_control_us = 0;

    for (;;) {
        /* drain button events (non-blocking) */
        button_event_t ev;
        while (btn_q && xQueueReceive(btn_q, &ev, 0) == pdTRUE) {
            process_button(&ev);
        }

        /* Buttons are serviced every INPUT_POLL_MS so
         * +/- and the panel stay responsive. The heavy control block below
         * (temperature read alone takes a few hundred ms) runs only once per
         * TICK_MS — draining buttons at that 1 Hz cadence is what made rapid
         * +/- presses feel laggy. */
        int64_t loop_now_us = esp_timer_get_time();
        if (loop_now_us < next_control_us) {
            vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
            continue;
        }
        next_control_us = loop_now_us + (int64_t)TICK_MS * 1000;

        app_config_t cfg;
        app_config_get(&cfg);

        temperature_reading_t temp;
        temperature_read(&temp);
        safety_status_t safety = safety_evaluate(&temp);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.temp     = temp;
        s_state.safety   = safety;
        s_state.target_c = cfg.target_temp_c;
        s_state.mode     = cfg.heating_mode;
        /* Shower-ready tracks the OUTLET tank (the delivered water) alone, not
         * the whole-heater average — a hot outlet means a usable shower even if
         * the inlet tank is still recovering. NaN (faulted outlet) → not ready. */
        s_state.shower_ready =
            !isnan(temp.outlet_c) && temp.outlet_c >= (float)cfg.shower_ready_c;

        bool relays_cmd[RELAY_COUNT] = { false, false };
        bool any_enabled = s_state.element_enabled[0] || s_state.element_enabled[1];
        s_state.master_enabled = any_enabled;
        bool ok_to_heat = any_enabled && safety == SAFETY_OK;

        /* Per-tank demand: each tank runs its own thermostat against the shared
         * target using its own regulation NTC and its own hysteresis state
         * (the relay it's currently driving). A disabled or faulted tank never
         * demands heat — so a sensor fault on one tank stops that element while
         * the healthy tank keeps heating. step_mode() then layers the mode's
         * power policy on top without ever forcing a satisfied tank on. */
        bool wants[RELAY_COUNT] = { false, false };
        for (int i = 0; i < RELAY_COUNT; ++i) {
            wants[i] = ok_to_heat && s_state.element_enabled[i] &&
                       !temp.tank[i].fault &&
                       update_heating_phase(s_state.heater_active[i],
                                            temp.tank[i].regulation_c,
                                            cfg.target_temp_c, cfg.hysteresis_c);
        }
        bool heating_needed = wants[0] || wants[1];

        const char *phase_name = "idle";
        if (ok_to_heat) {
            step_mode(&cfg, wants, relays_cmd, &phase_name);
        } else {
            reset_mode_state();
        }

        s_state.heater_active[0]    = relays_cmd[0];
        s_state.heater_active[1]    = relays_cmd[1];
        s_state.phase_name          = phase_name;
        s_state.phase_seconds_left  = s_mode_state.phase_seconds_left;
        s_state.heating_phase       = heating_needed;

        /* Drive the relays while still holding s_lock so a concurrent
         * heater_set_master_enabled(false) on the chip / web thread
         * (which calls relays_all_off internally) can't be silently
         * undone by us re-applying a stale relays_cmd. relays.c uses its
         * own mutex so there's no nested-lock deadlock risk. */
        if (ok_to_heat) {
            relays_set(relays_cmd);
        } else {
            relays_all_off();
        }
        xSemaphoreGive(s_lock);

        /* Benchmark tracking — driven from the same tick as control. */
        benchmark_tick(heating_needed, (uint8_t)s_state.mode, cfg.target_temp_c,
                       temp.water_c, s_state.master_enabled, (int)safety);

        /* Heater on/off transitions → event log. */
        static bool last_h[2];
        for (int i = 0; i < 2; ++i) {
            if (last_h[i] != relays_cmd[i]) {
                event_log_emit(relays_cmd[i] ? EV_HEATER_ON : EV_HEATER_OFF, i, 0, NULL);
                last_h[i] = relays_cmd[i];
            }
        }
        /* Safety transitions → event log. */
        static int last_safety = -1;
        if (last_safety != (int)safety) {
            if (safety == SAFETY_OK && last_safety > 0) {
                event_log_emit(EV_SAFETY_CLEARED, 0, 0, NULL);
            } else if (safety != SAFETY_OK) {
                event_log_emit(EV_SAFETY_FAULT, (int16_t)safety, 0, NULL);
            }
            last_safety = (int)safety;
        }

        /* publish panel state to leds (the LED task renders at 10 Hz) */
        led.master_enabled    = s_state.master_enabled;
        led.current_temp_c    = temp.water_c;
        led.any_heater_active = relays_cmd[0] || relays_cmd[1];
        led.one_heater_active = relays_cmd[0] ^ relays_cmd[1];
        led.wifi_connected    = wifi_mgr_state() == WIFI_STATE_STA_CONNECTED;
        led.shower_ready      = s_state.shower_ready;
        leds_publish_state(&led);

        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
}

esp_err_t heater_control_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    app_config_t cfg;
    app_config_get(&cfg);

    /* Default is OFF: don't run heaters before the user acks (see CLAUDE.md).
     * With restore_power_on_boot enabled we instead re-apply the last known
     * master state, so the heater survives a reboot / power cut. Safety still
     * gates the relays in the control tick, so restoring ON can't bypass a
     * fault — it just means heating resumes once the probes read OK. */
    bool restore = cfg.restore_power_on_boot && cfg.last_power_state;
    memset(&s_state, 0, sizeof(s_state));
    s_state.master_enabled     = restore;
    s_state.element_enabled[0] = restore;
    s_state.element_enabled[1] = restore;
    s_state.target_c       = cfg.target_temp_c;
    s_state.mode           = cfg.heating_mode;
    s_state.phase_name     = "idle";
    /* Temperature reading is "unknown" until temperature.c produces one — NaN
     * propagates correctly through the UI and the LED panel, where 0 °C would
     * incorrectly enable the 40 °C LED. */
    s_state.temp.water_c                    = NAN;
    s_state.temp.outlet_c                   = NAN;
    s_state.temp.max_c                      = NAN;
    s_state.temp.tank[TANK_INLET].regulation_c  = NAN;
    s_state.temp.tank[TANK_INLET].safety_c      = NAN;
    s_state.temp.tank[TANK_OUTLET].regulation_c = NAN;
    s_state.temp.tank[TANK_OUTLET].safety_c     = NAN;
    reset_mode_state();
    if (restore) event_log_emit(EV_MASTER_ON, 0, 0, "restored");

    if (xTaskCreate(control_task, "heater", 4096, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
