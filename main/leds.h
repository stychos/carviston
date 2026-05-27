/* leds — 8 LEDs on GPIO8..15. Owns a 10 Hz render task that consumes the
 * panel state published by heater_control and overlays transient animations.
 *
 *   GPIO8  POWER       under button 1 (On/Off)
 *   GPIO9  TEMP 40°
 *   GPIO10 TEMP 50°
 *   GPIO11 TEMP 60°
 *   GPIO12 TEMP 70°
 *   GPIO13 TEMP 80°
 *   GPIO14 SHOWER      under button 3 (Shower-Ready)
 *   GPIO15 ECO         under button 2 (ECO)
 *
 * Display modes for the top row (POWER / ECO / SHOWER) and the temperature
 * row are independent:
 *
 *   Master OFF        → panel blank, ECO LED still shows wifi-state when
 *                       configured for ECO_LED_WIFI_STATE.
 *   Master ON         → POWER per config, ECO per config, SHOWER if ready,
 *                       temp row shows current temp by default.
 *   Preview target    → temp row blanks except the LED for the target temp.
 *                       Held while +/- is pressed; lingers `preview_release_ms`
 *                       after final release.
 *   Mode-cycle blink  → POWER/ECO/SHOWER blank then blink N times (N = mode+1)
 *                       to confirm a new mode after long-press SHOWER.
 *   AP-mode blink     → ECO blinks rapidly a few times after long-press ECO.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool   master_enabled;
    float  current_temp_c;       /* may be NaN on full sensor fault */
    bool   any_heater_active;
    bool   one_heater_active;
    bool   wifi_connected;
    bool   shower_ready;
} led_panel_state_t;

esp_err_t leds_init(void);

/* Latest panel state; safe to call from any task. */
void leds_publish_state(const led_panel_state_t *s);

/* Show the LED for `target_c` (one of 40,50,60,70,80) while the user is
 * holding +/-; the temp row reverts to ACTUAL after the last release plus
 * `preview_release_ms`. Call once per press AND once on the final release
 * (so the linger window starts ticking). */
void leds_target_preview_press(uint8_t target_c);
void leds_target_preview_release(void);

/* Blank POWER/ECO/SHOWER, then blink them N times (N = mode_index + 1)
 * before returning to normal. Non-blocking. */
void leds_animate_mode(uint8_t mode_index);

/* Quick distinctive animation when force-AP-mode fires. */
void leds_animate_ap_mode(void);

/* While Matter commissioning is open, override the SHOWER LED with a
 * fast double-blink (heartbeat-style) so the user sees the device is
 * advertising for pairing. Idempotent; pass `false` when the window
 * closes. */
void leds_set_matter_pairing(bool on);

#ifdef __cplusplus
}
#endif
