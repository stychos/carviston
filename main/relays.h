/* relays — 2-channel optocoupler relay module with staggered turn-on.
 *
 *   GPIO47 → IN1  (SongLe SRD-05VDC-SL-C module, active-LOW)
 *   GPIO42 → IN2
 *
 * The module provides on-board NPN drivers, flyback diodes, opto-isolation
 * and the relays themselves. We just drive IN1/IN2. Logic is INVERTED in
 * this driver — callers see normal active-HIGH semantics (relay_on[i] = true
 * means the contact is closed).
 *
 * relays_set() requests a desired state; the driver enforces a minimum
 * 300 ms gap before energising a second relay so the two heating elements
 * never inrush simultaneously. De-energising is immediate.
 *
 * The driver does not enforce safety policy — that's safety.c. Heater_control
 * calls relays_set() only when safety_is_ok() returns true.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RELAY_COUNT 2

esp_err_t relays_init(void);

/* Request desired energised state for each relay. Idempotent.
 * Energising a previously-off relay is delayed if another relay was energised
 * within the last 300 ms. */
void relays_set(bool relay_on[RELAY_COUNT]);

/* Force everything off immediately. Safe to call from any context. */
void relays_all_off(void);

/* Current commanded state (post-delay logic). */
void relays_get(bool relay_on[RELAY_COUNT]);

#ifdef __cplusplus
}
#endif
