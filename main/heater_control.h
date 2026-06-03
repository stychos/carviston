/* heater_control — main control task.
 *
 * Owns the 1 Hz control loop: reads temperature, evaluates safety, processes
 * button events, runs the mode state machine, and commands relays.
 *
 * All public mutators are safe to call from any task (web server, button task,
 * future Matter handler). Internally they push a request that the next tick
 * picks up.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"
#include "safety.h"
#include "temperature.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of runtime state, sampled atomically. */
typedef struct {
    bool   master_enabled;       /* derived: true iff any element is enabled */
    bool   element_enabled[2];   /* user intent per heating element */
    heating_mode_t mode;
    uint8_t target_c;

    temperature_reading_t temp;

    bool   heater_active[2];
    bool   heating_phase;        /* are we currently in a "heating needed" cycle? */
    uint32_t phase_seconds_left; /* time left in current sub-stage of mode */
    const char *phase_name;      /* "heating", "rest", "swap1", etc. */

    safety_status_t safety;
    bool   shower_ready;
} heater_state_t;

esp_err_t heater_control_init(void);

/* External setters — thread-safe. */
void heater_set_target(uint8_t celsius);
void heater_set_mode(heating_mode_t mode);
void heater_set_master_enabled(bool on);     /* enables/disables both elements */
void heater_toggle_master(void);              /* convenience — what the power button does */

/* Per-element user enable. Independent of mode logic — the mode state
 * machine still decides when each enabled element fires, but a disabled
 * element will never close its relay. With both disabled, the system
 * is effectively off (master_enabled is reported as false). `channel`
 * is 0 or 1 (heater 1 / heater 2). */
void heater_set_element_enabled(uint8_t channel, bool on);
void heater_toggle_element(uint8_t channel);

/* Clear a latched safety fault. Forces master_enabled=false at the same time
 * so heating can't auto-resume the moment the latch drops — the user must
 * explicitly press POWER (or POST master=true) to re-arm. Use this instead
 * of safety_clear_fault() from any UI/API path. */
void heater_clear_safety_fault(void);

/* Atomically replace the current heating mode and return the previous value,
 * all under a single acquisition of the internal state lock. The Matter
 * boost path uses this to capture the pre-boost mode without races against
 * a concurrent button-press or web-API mode change. `prev_out` may be NULL.
 *
 * Side effects (NVS commit, event log emit, benchmark notify) happen outside
 * the lock, exactly as in heater_set_mode. */
void heater_atomic_swap_mode(heating_mode_t new_mode, heating_mode_t *prev_out);

/* Snapshot. */
void heater_get_state(heater_state_t *out);

#ifdef __cplusplus
}
#endif
