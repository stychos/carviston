/* safety — hardware cutoff sense + fault aggregation.
 *
 *   GPIO21 (input, internal pull-down enabled — see safety.c): senses the
 *   KSD301 hardware cutoff continuity through an external divider on the
 *   post-KSD +5 V rail (relay module's JD-VCC line). When the KSD301 trips,
 *   that rail collapses and firmware sees LOW → SAFETY_FAULT_CUTOFF. Internal
 *   pull-up stays disabled (it would mask a tripped rail); internal pull-down
 *   IS enabled so a broken sense wire also reads LOW instead of floating
 *   HIGH from leakage and silently defeating the fail-safe. The hardware
 *   cutoff itself is unaffected by firmware.
 *
 * Faults are sticky from this module's view — once we see a fault, we report
 * faulted until safety_clear_fault() is called (typically from the UI).
 *
 * safety_evaluate() should be called periodically (e.g. once per heater_control
 * tick) with the latest temperature reading. It updates the cached state.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "temperature.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAFETY_OK = 0,
    SAFETY_FAULT_CUTOFF,       /* hardware cutoff has tripped */
    SAFETY_FAULT_PROBE,        /* probe disagreement / sensor failure */
    SAFETY_FAULT_OVERTEMP,     /* water temp exceeds soft limit (95C) */
    SAFETY_FAULT_NO_PROBES,    /* all probes faulted, no valid reading */
} safety_status_t;

esp_err_t safety_init(void);

/* Evaluate inputs and update internal state. Returns the current status. */
safety_status_t safety_evaluate(const temperature_reading_t *temp);

/* Latest cached status. */
safety_status_t safety_status(void);

/* True iff status == SAFETY_OK. Used by heater_control before energising. */
bool safety_is_ok(void);

/* Clear sticky fault (from UI). Cutoff sense still gates this — clearing
 * while the cutoff is open will immediately re-latch the fault. */
void safety_clear_fault(void);

/* Cutoff sense raw read (true = continuity intact). */
bool safety_cutoff_intact(void);

#ifdef __cplusplus
}
#endif
