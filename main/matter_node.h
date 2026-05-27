/* matter_node — Matter over WiFi (Water Heater profile).
 *
 * Brings up a Matter node whose primary endpoint is a water-heater bridge to
 * heater_control. Clusters wired in phase 2:
 *   - OnOff                    → heater_set_master_enabled
 *   - Thermostat               → heater_set_target / heater_set_master_enabled
 *   - TemperatureMeasurement   → mirror of LocalTemperature
 *
 * Disabled at compile time unless CONFIG_CARVISTON_MATTER_ENABLE=y. When
 * disabled, matter_node_start() is a no-op so call sites stay stable.
 *
 * See docs/MATTER.md for the procedure to enable Matter and provision
 * factory data (DAC + CD into the matter_fac partition).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the Matter stack. Must be called after wifi_mgr_start(). */
esp_err_t matter_node_start(void);

/* Snapshot of the current commissioning window state. When `active` is
 * true, `qr` carries the chip-spec QR payload (e.g. "MT:0000000000000000")
 * and `manual` carries the 11-digit decimal pairing code. Empty strings
 * when `active` is false. `window_s_remaining` is the seconds left until
 * the window closes; 0 when inactive. */
typedef struct {
    bool     active;
    char     qr[64];
    char     manual[12];
    uint32_t window_s_remaining;
} matter_pairing_info_t;

/* Open a basic commissioning window. window_s is clamped to
 * [180, 900] by the chip stack. Idempotent — re-opens a fresh window
 * if one is already open. Returns ESP_OK on success, or a stub-OK when
 * Matter is disabled at build time. */
esp_err_t matter_node_open_pairing(uint32_t window_s);

/* Populate *out with the current pairing window state. Always returns
 * ESP_OK; sets out->active=false when Matter is disabled or no window
 * is open. */
esp_err_t matter_node_get_pairing_info(matter_pairing_info_t *out);

#ifdef __cplusplus
}
#endif
