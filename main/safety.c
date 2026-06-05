#include "safety.h"

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "relays.h"

static const char *TAG = "safety";

/* Soft over-temperature limit. With no hardware thermal cutoff, this firmware
 * limit is the SOLE over-temp protection. The highest user-settable target is
 * 80 °C, so 90 °C is a hard ceiling 10 °C above any legitimate setpoint — no
 * tank may exceed it under any condition. It is a real-time hazard: evaluated
 * independently of the (debounced) sensor faults and never debounced or masked. */
#define SOFT_OVERTEMP_C   90.0f

/* A total sensor loss (BOTH tanks faulted → no usable temperature anywhere)
 * must persist this many consecutive 1 Hz evaluations before it latches. The
 * ADS1115 read is conversion-ready-gated now, so a genuine fault holds steady
 * and still latches on the 3rd consecutive tick (~3 s); this only swallows a
 * lone transient (EMI on I2C, a NaN at a divider edge) that would otherwise
 * latch and force a needless recovery cycle. A SINGLE tank faulting is NOT a
 * global fault — heater_control gates that tank's element off and the healthy
 * tank keeps running. OVERTEMP is a real-time hazard: never debounced. */
#define PROBE_FAULT_DEBOUNCE   3

/* Auto-recovery: a latched fault clears itself once the live condition has
 * read clear for this many consecutive 1 Hz evaluations — no user action
 * needed. The window is an anti-flap guard so a fault that flickers OK for a
 * single tick doesn't bounce heating back on. element_enabled/master are
 * preserved through a latch, so heating resumes on its own when the latch
 * drops. */
#define SAFETY_RECOVERY_TICKS  5

static SemaphoreHandle_t s_lock;
static safety_status_t s_status = SAFETY_OK;
/* Consecutive sensor-fault tick count for the debounce. Guarded by s_lock: the
 * heater task increments/resets it in safety_evaluate(), and a UI clear
 * (safety_clear_fault) resets it from another task. */
static uint8_t s_sensor_fault_streak = 0;
/* Consecutive clear-condition ticks while a fault is latched — drives
 * auto-recovery (see SAFETY_RECOVERY_TICKS). Guarded by s_lock. */
static uint8_t s_recover_streak = 0;

esp_err_t safety_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

safety_status_t safety_evaluate(const temperature_reading_t *temp)
{
    /* Over-temp is the one real-time hazard — computed independently of the
     * sensor-fault state and NEVER debounced. It watches the HOTTEST healthy
     * tank (max_c): any tank we can still read crossing SOFT_OVERTEMP_C trips
     * immediately, even when the OTHER tank's probe is faulted. */
    bool overtemp = temp && !isnan(temp->max_c) && temp->max_c >= SOFT_OVERTEMP_C;

    /* Total sensor loss — only BOTH tanks faulting (no usable temperature
     * anywhere) is a global fault. A single faulted tank is handled per-tank by
     * heater_control; the healthy tank keeps heating. Debounced below. */
    safety_status_t sensor = (!temp || temp->all_fault) ? SAFETY_FAULT_NO_PROBES
                                                        : SAFETY_OK;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Debounce ONLY the sensor-loss fault so one glitchy read can't latch a
     * fault — see PROBE_FAULT_DEBOUNCE. The streak lives under s_lock because
     * safety_clear_fault() resets it from another task. */
    if (sensor != SAFETY_OK) {
        if (s_sensor_fault_streak < PROBE_FAULT_DEBOUNCE) s_sensor_fault_streak++;
    } else {
        s_sensor_fault_streak = 0;
    }

    /* Priority: over-temp latches immediately; the debounced sensor-loss fault
     * only surfaces once it has persisted PROBE_FAULT_DEBOUNCE ticks. */
    safety_status_t s = SAFETY_OK;
    if (overtemp)                                          s = SAFETY_FAULT_OVERTEMP;
    else if (sensor != SAFETY_OK &&
             s_sensor_fault_streak >= PROBE_FAULT_DEBOUNCE) s = sensor;

    if (s != SAFETY_OK) {
        /* Hazard present: latch (drop the relays on the OK→fault edge) and
         * arm the recovery window afresh. */
        if (s_status == SAFETY_OK) {
            ESP_LOGW(TAG, "safety fault latched: %d", (int)s);
            relays_all_off();
        }
        s_status = s;
        s_recover_streak = 0;
    } else if (s_status != SAFETY_OK) {
        /* Latched, but the live condition now reads clear. Auto-recover once it
         * has stayed clear for SAFETY_RECOVERY_TICKS consecutive ticks — no user
         * action required; heating resumes on its own (master was preserved). */
        if (s_recover_streak < SAFETY_RECOVERY_TICKS) s_recover_streak++;
        if (s_recover_streak >= SAFETY_RECOVERY_TICKS) {
            ESP_LOGI(TAG, "safety fault auto-cleared — condition gone for %d s",
                     SAFETY_RECOVERY_TICKS);
            s_status = SAFETY_OK;
        }
    }
    safety_status_t out = s_status;
    xSemaphoreGive(s_lock);
    return out;
}

safety_status_t safety_status(void)
{
    safety_status_t s;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = s_status;
    xSemaphoreGive(s_lock);
    return s;
}

bool safety_is_ok(void)
{
    return safety_status() == SAFETY_OK;
}

void safety_clear_fault(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_status = SAFETY_OK;
    s_sensor_fault_streak = 0;   /* fresh debounce window after a manual clear */
    s_recover_streak = 0;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "safety fault cleared (will re-latch if condition persists)");
}
