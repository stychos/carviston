/* benchmark — track heating "sessions" for performance logging.
 *
 * A session begins the moment the controller enters "heating phase" (any of
 * the four modes is actively driving relays toward the target) and ends when
 * heating phase exits — whether the target was reached, the user changed the
 * mode/target, or master power was turned off.
 *
 * Active sessions are persisted to NVS so a reboot can resume them — but only
 * if the elapsed wall time between shutdown and reboot is within
 * `bench_resume_threshold_s` (from app_config). Wall time requires SNTP; if
 * the clock hasn't synced (e.g. AP-only mode), the active session is aborted.
 *
 * Completed sessions surface via the event_log as EV_BENCH_END / EV_BENCH_ABORT.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BENCH_END_TARGET_REACHED = 0,
    BENCH_END_MODE_CHANGED,
    BENCH_END_TARGET_CHANGED,
    BENCH_END_MASTER_OFF,
    BENCH_END_SAFETY_FAULT,
    BENCH_END_REBOOT,
} bench_end_reason_t;

esp_err_t benchmark_init(void);

/* Called from heater_control every tick. Drives the state machine. */
void benchmark_tick(bool heating_phase, uint8_t mode, uint8_t target_c, float water_c,
                    bool master_enabled, int safety);

/* External transitions worth noticing immediately (so the bench is split at
 * the right boundary, not deferred to the next tick). */
void benchmark_note_mode_change(uint8_t new_mode);
void benchmark_note_target_change(uint8_t new_target);

#ifdef __cplusplus
}
#endif
