/* benchmark — track heating "programs" for performance logging.
 *
 * A program models one meaningful heat-up: it begins when heating starts with a
 * real gap to target (a fresh power-on, a raised setpoint, or heating resuming
 * after a water draw) and ends when the target is reached — OR when a water
 * draw interrupts it. Routine hysteresis maintenance cycling (the small reheats
 * that keep the tank at target) does NOT start a program, so the log isn't
 * spammed with short runs. See heater_control's call site and app_config's
 * bench_min_gap_c / draw_detect_* fields.
 *
 * Active programs are persisted to NVS so a reboot can resume them — but only
 * if the elapsed wall time between shutdown and reboot is within
 * `bench_resume_threshold_s` (from app_config). Wall time requires SNTP; if
 * the clock hasn't synced (e.g. AP-only mode), the active program is aborted.
 *
 * Completed programs surface via the event_log as EV_BENCH_END / EV_BENCH_ABORT.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BENCH_END_TARGET_REACHED = 0,
    BENCH_END_MODE_CHANGED,    /* retired — kept for log/value stability */
    BENCH_END_TARGET_CHANGED,  /* retired — kept for log/value stability */
    BENCH_END_MASTER_OFF,
    BENCH_END_SAFETY_FAULT,
    BENCH_END_REBOOT,
    BENCH_END_WATER_DRAW,      /* a draw interrupted the heat-up */
} bench_end_reason_t;

esp_err_t benchmark_init(void);

/* Called from heater_control every tick. Drives the program state machine.
 *  - heating_phase : thermostat demand (a tank wants heat this tick)
 *  - water_c       : whole-heater temperature (program gap is measured off this)
 *  - inlet_c       : inlet regulation temperature (fed to the water-draw detector)
 *  - cfg           : live config (thresholds); borrowed, not retained */
void benchmark_tick(bool heating_phase, uint8_t mode, uint8_t target_c,
                    float water_c, float inlet_c, bool master_enabled, int safety,
                    const app_config_t *cfg);

#ifdef __cplusplus
}
#endif
