/* temperature — ADC1 + NTC math.
 *
 * Two probes (A, B). Each probe has a regulation NTC and a safety NTC,
 * both 10k @ 25C. Voltage divider: 3V3 → R_TOP (10k) → ADC pin → NTC → GND.
 *
 * The driver samples all four NTCs, converts to °C using the Beta equation
 * (R25 + Beta configurable via app_config), and exposes:
 *   - per-probe regulation & safety temperatures
 *   - per-probe fault flag (regulation/safety disagreement)
 *   - aggregate water temperature (average of both regulation NTCs, ignoring
 *     faulted probes)
 *   - overall fault flag (set if any probe is faulted OR sensor is open/short)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROBE_A = 0,
    PROBE_B,
    PROBE_COUNT,
} probe_id_t;

typedef struct {
    float regulation_c;
    float safety_c;
    bool  fault;           /* sensor disagreement, open or short */
} probe_reading_t;

typedef struct {
    probe_reading_t probe[PROBE_COUNT];
    float water_c;         /* average of healthy regulation NTCs */
    bool  any_fault;       /* any probe faulted */
    bool  all_fault;       /* all probes faulted — no valid reading */
} temperature_reading_t;

/* Initialize ADC1 + calibration. Must be called after app_config_init. */
esp_err_t temperature_init(void);

/* Take a fresh reading (blocking, ~30 ms with averaging). */
esp_err_t temperature_read(temperature_reading_t *out);

#ifdef __cplusplus
}
#endif
