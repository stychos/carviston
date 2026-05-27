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

#define TICK_MS  1000

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
        /* Deferred — Matter chip thread calls this on every setpoint change
         * a controller pushes; an inline nvs_commit would stall BLE adv and
         * TCP keepalives during a sector erase. */
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
        /* Persist + telemetry outside the lock. Deferred save → the chip
         * thread isn't stalled by a sector erase when a Matter controller
         * cycles modes. */
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

void heater_set_master_enabled(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool prev = s_state.master_enabled;
    s_state.master_enabled = on;
    if (!on) reset_mode_state();
    xSemaphoreGive(s_lock);
    if (!on) relays_all_off();
    if (prev != on) event_log_emit(on ? EV_MASTER_ON : EV_MASTER_OFF, 0, 0, NULL);
}

void heater_toggle_master(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool now = !s_state.master_enabled;
    s_state.master_enabled = now;
    if (!now) reset_mode_state();
    xSemaphoreGive(s_lock);
    if (!now) relays_all_off();
    event_log_emit(now ? EV_MASTER_ON : EV_MASTER_OFF, 0, 0, NULL);
}

void heater_clear_safety_fault(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_on = s_state.master_enabled;
    s_state.master_enabled = false;
    reset_mode_state();
    xSemaphoreGive(s_lock);
    relays_all_off();
    if (was_on) event_log_emit(EV_MASTER_OFF, 0, 0, NULL);
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

/* Returns true if we should be in "heating needed" cycle. Implements hysteresis. */
static bool update_heating_phase(bool currently_heating, float water_c,
                                 uint8_t target, uint8_t hyst)
{
    if (isnan(water_c)) return false;
    if (currently_heating) {
        return water_c < (float)target;
    }
    return water_c <= (float)target - (float)hyst;
}

/* Runs the mode state machine for one tick. Caller holds s_lock. */
static void step_mode(const app_config_t *cfg, bool heating_needed,
                      bool out_relays[RELAY_COUNT], const char **phase_name)
{
    out_relays[0] = false;
    out_relays[1] = false;
    *phase_name = "idle";

    if (!heating_needed) {
        /* Target reached / outside hysteresis: pause but PRESERVE the mode
         * state. When water cools and heating resumes, OPTIMAL continues the
         * current heater's remaining slot, ECO continues its current phase,
         * FAST resumes its current on/rest window. Resetting here would
         * restart the alternation pattern each cycle and (for ECO) lose any
         * mandated rest period. */
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
        out_relays[0] = true;
        out_relays[1] = true;
        *phase_name = "heating";
        break;

    case HEATING_MODE_FAST: {
        /* phase 0: both on for N min. phase 1: rest R min.
         * Boundary fires only when an actual phase runs out — the entry
         * block above pre-loaded fast_on_min for the opening tick. */
        if (s_mode_state.phase_seconds_left == 0) {
            s_mode_state.phase ^= 1;
            uint32_t mins = (s_mode_state.phase == 0)
                            ? cfg->fast_on_min : cfg->fast_rest_min;
            s_mode_state.phase_seconds_left = (uint32_t)mins * 60u;
        }
        if (s_mode_state.phase == 0) {
            out_relays[0] = true;
            out_relays[1] = true;
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
        out_relays[s_mode_state.current_relay] = true;
        *phase_name = (s_mode_state.current_relay == 0) ? "heater 1" : "heater 2";
        break;
    }

    case HEATING_MODE_ECO: {
        /* phase 0: heater current_relay on for N min. phase 1: rest R min, then swap. */
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
            out_relays[s_mode_state.current_relay] = true;
            *phase_name = (s_mode_state.current_relay == 0) ? "heater 1" : "heater 2";
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
#include "matter_node.h" /* matter_node_open_pairing / pairing info */

/* Combo: holding POWER + ECO together for COMBO_TRIGGER_MS opens a Matter
 * commissioning window. Both press orderings are supported — see the BTN
 * handlers and the tick-loop combo detector for the precise rules. */
#define COMBO_TRIGGER_MS          3000
#define COMBO_PAIR_WINDOW_S        180
#define COMBO_PRESS_PROXIMITY_US  (500 * 1000)   /* 500 ms — power+eco "near-simultaneous" */
#define COMBO_PRESS_RECENT_US     (2000 * 1000)  /* 2 s — power pressed recently enough that
                                                    a subsequent ECO long-press still belongs
                                                    to the combo */

/* Press / toggle timestamps used by the combo logic. Updated only from the
 * heater control task, so no lock required. */
static int64_t s_last_power_press_us = 0;
static int64_t s_master_toggle_us    = 0;

static void process_button(const button_event_t *ev)
{
    button_id_t id = (button_id_t)ev->id;
    button_kind_t k = (button_kind_t)ev->kind;

    if (k == BTN_KIND_PRESS) event_log_emit(EV_BUTTON_PRESS, (int16_t)id, 0, NULL);
    else if (k == BTN_KIND_LONG) event_log_emit(EV_BUTTON_LONG, (int16_t)id, 0, NULL);

    switch (id) {
    case BTN_POWER:
        if (k == BTN_KIND_PRESS) {
            int64_t now = esp_timer_get_time();
            s_last_power_press_us = now;
            /* If ECO is already down, the user is starting (or completing)
             * the Matter pairing combo — don't fire the standalone master
             * toggle. The reverse ordering (POWER first, then ECO) is
             * handled by the retroactive undo in the tick-loop fire path. */
            if (buttons_is_held(BTN_ECO)) {
                ESP_LOGI(TAG, "BTN power press while ECO held — combo, suppress toggle");
                break;
            }
            heater_toggle_master();
            s_master_toggle_us = now;
            ESP_LOGI(TAG, "BTN power → master=%d", (int)s_state.master_enabled);
        }
        break;

    case BTN_ECO:
        if (k == BTN_KIND_LONG) {
            int64_t now = esp_timer_get_time();
            /* Suppress AP-mode if POWER is held right now OR was pressed
             * recently — covers both orderings of the Matter pairing combo:
             *   - POWER held already (caught by buttons_is_held)
             *   - POWER pressed within the last COMBO_PRESS_RECENT_US,
             *     even if briefly released or not yet held when LONG fires. */
            if (buttons_is_held(BTN_POWER) ||
                (s_last_power_press_us != 0 &&
                 now - s_last_power_press_us < COMBO_PRESS_RECENT_US)) {
                ESP_LOGI(TAG, "BTN eco long-press ignored (combo in progress)");
                break;
            }
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

    /* Tracks the time at which BOTH POWER+ECO first went down together.
     * 0 = not currently held. Sub-tick precision via esp_timer so the trigger
     * lands within ~one buttons-poll period (20 ms) of COMBO_TRIGGER_MS,
     * rather than jittering across a 1 s window. */
    static int64_t combo_both_held_since_us = 0;
    static bool    combo_fired              = false;

    for (;;) {
        /* drain button events (non-blocking) */
        button_event_t ev;
        while (btn_q && xQueueReceive(btn_q, &ev, 0) == pdTRUE) {
            process_button(&ev);
        }

        /* POWER + ECO held → Matter commissioning combo */
        bool combo_held = buttons_is_held(BTN_POWER) && buttons_is_held(BTN_ECO);
        if (combo_held) {
            if (combo_both_held_since_us == 0) {
                combo_both_held_since_us = esp_timer_get_time();
            }
            int64_t held_ms = (esp_timer_get_time() - combo_both_held_since_us) / 1000;
            if (!combo_fired && held_ms >= COMBO_TRIGGER_MS) {
                combo_fired = true;
                ESP_LOGI(TAG, "POWER+ECO held %lld ms → open Matter pairing window",
                         (long long)held_ms);

                /* If POWER PRESS toggled master right around when ECO joined
                 * (POWER-first ordering, the toggle fired before ECO had
                 * arrived), retroactively undo it — the user's intent was
                 * the combo, not a power toggle. */
                if (s_master_toggle_us != 0 &&
                    s_master_toggle_us >= combo_both_held_since_us - COMBO_PRESS_PROXIMITY_US) {
                    ESP_LOGI(TAG, "combo undoing the POWER PRESS toggle that started it");
                    heater_toggle_master();
                    s_master_toggle_us = 0;
                }

                if (matter_node_open_pairing(COMBO_PAIR_WINDOW_S) == ESP_OK) {
                    leds_set_matter_pairing(true);
                    event_log_emit(EV_BUTTON_LONG, 99 /* combo */, 0, NULL);
                }
            }
        } else {
            combo_both_held_since_us = 0;
            combo_fired              = false;
        }

        /* Keep the SHOWER LED's commissioning blink in sync with the actual
         * pairing window — it auto-clears when the window times out. */
        {
            matter_pairing_info_t pi;
            if (matter_node_get_pairing_info(&pi) == ESP_OK) {
                leds_set_matter_pairing(pi.active);
            }
        }

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
        s_state.shower_ready =
            !temp.all_fault && temp.water_c >= (float)cfg.shower_ready_c;

        bool relays_cmd[RELAY_COUNT] = { false, false };
        bool ok_to_heat = s_state.master_enabled && safety == SAFETY_OK;
        bool currently_heating = s_state.heater_active[0] || s_state.heater_active[1];
        bool heating_needed = ok_to_heat &&
            update_heating_phase(currently_heating, temp.water_c,
                                 cfg.target_temp_c, cfg.hysteresis_c);

        const char *phase_name = "idle";
        if (ok_to_heat) {
            step_mode(&cfg, heating_needed, relays_cmd, &phase_name);
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

        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t heater_control_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    app_config_t cfg;
    app_config_get(&cfg);

    memset(&s_state, 0, sizeof(s_state));
    s_state.master_enabled = false;       /* require explicit user power-on */
    s_state.target_c       = cfg.target_temp_c;
    s_state.mode           = cfg.heating_mode;
    s_state.phase_name     = "idle";
    /* Temperature reading is "unknown" until temperature.c produces one — NaN
     * propagates correctly through Matter (nullable int16 → Matter null) and
     * the LED panel, where 0 °C would incorrectly enable the 40 °C LED. */
    s_state.temp.water_c             = NAN;
    s_state.temp.probe[0].regulation_c = NAN;
    s_state.temp.probe[0].safety_c     = NAN;
    s_state.temp.probe[1].regulation_c = NAN;
    s_state.temp.probe[1].safety_c     = NAN;
    reset_mode_state();

    if (xTaskCreate(control_task, "heater", 4096, NULL, 7, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
