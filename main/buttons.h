/* buttons — 5 push-buttons with debounce and long-press.
 *
 * Physical layout (push-buttons, momentary, active-low w/ internal pull-up):
 *
 *   GPIO16  BTN_POWER    On/Off  — short press toggles "heaters allowed".
 *                                  Has a POWER LED under it.
 *   GPIO17  BTN_ECO      ECO     — long press forces AP mode (first-boot AP
 *                                  if nothing is configured). Has an ECO LED
 *                                  under it.
 *   GPIO18  BTN_SHOWER   Shower  — long press cycles heating mode with a
 *                                  blink-X-times animation on the top row
 *                                  (X = new mode index). Has a SHOWER LED
 *                                  under it.
 *   GPIO40  BTN_PLUS     +       — short press: next target temp step.
 *   GPIO41  BTN_MINUS    −       — short press: previous target temp step.
 *
 * Events are posted to a FreeRTOS queue as struct button_event_t. Subscribers
 * (heater_control) handle press/long/release semantics. Long-press fires once
 * per hold; subsequent short ticks while still held do not retrigger.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_POWER  = 0,
    BTN_ECO,
    BTN_SHOWER,
    BTN_PLUS,
    BTN_MINUS,
    BTN_COUNT,
} button_id_t;

typedef enum {
    BTN_KIND_PRESS = 0,   /* button just went down (debounced) */
    BTN_KIND_LONG,        /* held past long-press threshold (fires once) */
    BTN_KIND_RELEASE,     /* button just went up */
} button_kind_t;

typedef struct {
    uint8_t  id;          /* button_id_t */
    uint8_t  kind;        /* button_kind_t */
    uint16_t held_ms;     /* meaningful on RELEASE (and roughly on LONG) */
} button_event_t;

esp_err_t buttons_init(void);

/* Returns the queue carrying button_event_t records. */
QueueHandle_t buttons_event_queue(void);

/* True iff the named button is currently physically held (debounced). */
bool buttons_is_held(button_id_t id);

#ifdef __cplusplus
}
#endif
