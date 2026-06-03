/* safety — over-temp + total-sensor-loss aggregation.
 *
 * Two global hazards stop the system:
 *   - OVERTEMP: a tank crosses the firmware soft limit (the sole over-temp
 *     protection — there is no hardware thermal cutoff). Real-time, immediate.
 *   - NO_PROBES: BOTH tanks lose their sensors, leaving no usable temperature.
 * A SINGLE tank's probe failing is NOT a global fault — heater_control gates
 * that tank's element off while the healthy tank keeps heating.
 *
 * A fault latches (drops the relays the instant it appears) but AUTO-RECOVERS:
 * safety_evaluate() keeps re-checking the live inputs each tick, and once the
 * condition has read clear for a short recovery window the latch drops on its
 * own and heating resumes — no user action required. safety_clear_fault()
 * still exists for an explicit manual reset from the UI/Matter.
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
    SAFETY_FAULT_OVERTEMP,     /* a tank exceeds the soft over-temp limit (90C) */
    SAFETY_FAULT_NO_PROBES,    /* all tanks faulted — no usable reading */
} safety_status_t;

esp_err_t safety_init(void);

/* Evaluate inputs and update internal state. Returns the current status. */
safety_status_t safety_evaluate(const temperature_reading_t *temp);

/* Latest cached status. */
safety_status_t safety_status(void);

/* True iff status == SAFETY_OK. Used by heater_control before energising. */
bool safety_is_ok(void);

/* Explicit manual reset (from UI/Matter). The fault re-latches on the next
 * evaluate if its condition still holds; normally the latch auto-recovers on
 * its own, so this is only for forcing an immediate clear. */
void safety_clear_fault(void);

#ifdef __cplusplus
}
#endif
