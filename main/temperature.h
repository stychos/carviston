/* temperature — ADS1115 + NTC math.
 *
 * Two independent tanks in series: the INLET tank (cold-feed side) and the
 * OUTLET tank (delivered hot water). Each tank carries a *redundant* dual-NTC
 * probe — a regulation NTC and a safety NTC that sit in the same water and
 * should read nearly equal; a disagreement between them flags that tank's
 * sensor as failed. The two tanks themselves legitimately run at different
 * temperatures (the inlet tank cools fast during a draw), so they are NEVER
 * cross-checked against each other and are regulated independently.
 *
 * Both NTCs are 10k @ 25C. Voltage divider: 3V3 → R_TOP (10k) → ADC pin → NTC → GND.
 *
 * The driver samples all four NTCs, converts to °C using the Beta equation
 * (R25 + Beta configurable via app_config), and exposes:
 *   - per-tank regulation & safety temperatures
 *   - per-tank fault flag (its two NTCs disagree, or one is open/short)
 *   - water_c: the MEAN of the healthy tanks — the single number shown on the
 *     UI gauge and the LED bar (the whole-heater temperature)
 *   - outlet_c: the OUTLET tank's regulation NTC alone (delivered water) — used
 *     only for the shower-ready decision; NaN if the outlet sensor has faulted
 *   - max_c: the hottest healthy tank — what the soft over-temp limit watches
 *   - overall fault flags (any tank faulted / all tanks faulted)
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TANK_INLET = 0,    /* Sensor A — cold-feed tank */
    TANK_OUTLET,       /* Sensor B — delivered-water tank */
    TANK_COUNT,
} tank_id_t;

typedef struct {
    float regulation_c;    /* control NTC — drives this tank's thermostat */
    float safety_c;        /* redundant NTC — cross-checked against regulation */
    bool  fault;           /* the two NTCs disagree, or one is open/short */
} tank_reading_t;

typedef struct {
    tank_reading_t tank[TANK_COUNT];
    float water_c;         /* mean of healthy tanks — UI gauge / LEDs */
    float outlet_c;        /* outlet tank only — shower-ready (NaN if faulted) */
    float max_c;           /* hottest healthy tank — soft over-temp watches this */
    bool  any_fault;       /* any tank faulted */
    bool  all_fault;       /* all tanks faulted — no valid reading */
} temperature_reading_t;

/* Initialize ADC1 + calibration. Must be called after app_config_init. */
esp_err_t temperature_init(void);

/* Take a fresh reading (blocking, ~30 ms with averaging). */
esp_err_t temperature_read(temperature_reading_t *out);

#ifdef __cplusplus
}
#endif
