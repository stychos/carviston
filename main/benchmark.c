#include "benchmark.h"

#include <math.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "event_log.h"

static const char *TAG = "bench";

#define NVS_PART      "storage"
#define NVS_NS        "bench"
#define NVS_KEY       "active"

typedef struct __attribute__((packed)) {
    uint8_t  in_progress;   /* 0/1 */
    uint8_t  mode;
    uint8_t  target_c;
    uint8_t  pad;
    uint64_t start_unix;    /* 0 if wall clock unavailable */
    uint64_t start_uptime_us;
    float    start_temp_c;
} active_bench_t;

static active_bench_t s_act;
static SemaphoreHandle_t s_lock;
static bool s_inited;

static bool wall_now(uint64_t *out)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    *out = (uint64_t)now;
    return true;
}

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, &s_act, sizeof(s_act));
    nvs_commit(h);
    nvs_close(h);
}

static void load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        memset(&s_act, 0, sizeof(s_act));
        return;
    }
    size_t sz = sizeof(s_act);
    if (nvs_get_blob(h, NVS_KEY, &s_act, &sz) != ESP_OK || sz != sizeof(s_act)) {
        memset(&s_act, 0, sizeof(s_act));
    }
    nvs_close(h);
}

static void emit_end(bench_end_reason_t reason, float end_temp_c, uint32_t duration_s)
{
    int16_t a = (int16_t)((s_act.start_temp_c) * 10.0f);   /* deci-°C */
    int16_t b = (int16_t)(isnan(end_temp_c) ? 0 : end_temp_c * 10.0f);
    char text[36];
    snprintf(text, sizeof(text), "m=%u tgt=%u dur=%lus r=%d",
             s_act.mode, s_act.target_c, (unsigned long)duration_s, (int)reason);
    event_log_emit(EV_BENCH_END, a, b, text);
}

static void start_bench(uint8_t mode, uint8_t target_c, float water_c)
{
    s_act.in_progress = 1;
    s_act.mode        = mode;
    s_act.target_c    = target_c;
    uint64_t now_unix = 0;
    wall_now(&now_unix);
    s_act.start_unix  = now_unix;
    s_act.start_uptime_us = esp_timer_get_time();
    s_act.start_temp_c = isnan(water_c) ? 0.0f : water_c;
    persist_locked();
    char text[36];
    snprintf(text, sizeof(text), "m=%u tgt=%u from=%.1f",
             mode, target_c, (double)s_act.start_temp_c);
    event_log_emit(EV_BENCH_START, (int16_t)(s_act.start_temp_c * 10.0f), target_c, text);
}

static void end_bench(bench_end_reason_t reason, float water_c)
{
    if (!s_act.in_progress) return;
    uint64_t now_us = esp_timer_get_time();
    uint32_t dur_s = (uint32_t)((now_us - s_act.start_uptime_us) / 1000000ULL);
    /* If we have wall clocks at both ends, prefer that. */
    uint64_t now_unix; if (wall_now(&now_unix) && s_act.start_unix) {
        if (now_unix > s_act.start_unix) dur_s = (uint32_t)(now_unix - s_act.start_unix);
    }
    emit_end(reason, water_c, dur_s);
    s_act.in_progress = 0;
    persist_locked();
}

/* Resume logic: only kept if we can prove (via wall clock) that little time
 * has elapsed since the stored start. */
static void maybe_resume_locked(void)
{
    if (!s_act.in_progress) return;
    app_config_t cfg;
    app_config_get(&cfg);
    uint32_t threshold_s = cfg.bench_resume_threshold_s ? cfg.bench_resume_threshold_s : 600;

    uint64_t now_unix;
    if (!wall_now(&now_unix) || !s_act.start_unix) {
        ESP_LOGW(TAG, "active bench dropped (no wall clock to gauge elapsed time)");
        event_log_emit(EV_BENCH_ABORT, 0, 0, "no wall clock");
        s_act.in_progress = 0;
        persist_locked();
        return;
    }
    if (now_unix < s_act.start_unix || now_unix - s_act.start_unix > threshold_s) {
        ESP_LOGW(TAG, "active bench dropped (elapsed > threshold)");
        event_log_emit(EV_BENCH_ABORT, 0, 0, "elapsed > threshold");
        s_act.in_progress = 0;
        persist_locked();
        return;
    }
    /* Adjust start_uptime_us so durations stay consistent post-reboot. */
    uint64_t elapsed_s = now_unix - s_act.start_unix;
    s_act.start_uptime_us = esp_timer_get_time() - elapsed_s * 1000000ULL;
    ESP_LOGI(TAG, "resumed active bench (elapsed=%lu s)", (unsigned long)elapsed_s);
}

esp_err_t benchmark_init(void)
{
    if (s_inited) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_locked();
    /* Wall clock might not be synced yet at init time. The first tick after
     * SNTP can still see the stored start and decide. We try here AND once on
     * first tick. */
    maybe_resume_locked();
    xSemaphoreGive(s_lock);
    s_inited = true;
    return ESP_OK;
}

void benchmark_note_mode_change(uint8_t new_mode)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_act.in_progress && s_act.mode != new_mode) {
        end_bench(BENCH_END_MODE_CHANGED, NAN);
    }
    xSemaphoreGive(s_lock);
}

void benchmark_note_target_change(uint8_t new_target)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_act.in_progress && s_act.target_c != new_target) {
        end_bench(BENCH_END_TARGET_CHANGED, NAN);
    }
    xSemaphoreGive(s_lock);
}

void benchmark_tick(bool heating_phase, uint8_t mode, uint8_t target_c, float water_c,
                    bool master_enabled, int safety)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* deferred resume attempt — covers the case where SNTP synced after init.
     * Run AT MOST once after init regardless of outcome: a successful resume
     * rewinds start_uptime_us, but elapsed-since-start_unix keeps growing —
     * re-entering maybe_resume_locked() on each tick would eventually exceed
     * bench_resume_threshold_s and spuriously abort a live session. */
    static bool resume_attempted_after_sync;
    if (!resume_attempted_after_sync && s_act.in_progress) {
        maybe_resume_locked();
        resume_attempted_after_sync = true;
    }

    if (!master_enabled) {
        if (s_act.in_progress) end_bench(BENCH_END_MASTER_OFF, water_c);
        xSemaphoreGive(s_lock);
        return;
    }
    if (safety != 0) {
        if (s_act.in_progress) end_bench(BENCH_END_SAFETY_FAULT, water_c);
        xSemaphoreGive(s_lock);
        return;
    }

    if (heating_phase) {
        if (!s_act.in_progress) {
            start_bench(mode, target_c, water_c);
        } else if (s_act.mode != mode || s_act.target_c != target_c) {
            /* split at the boundary */
            end_bench(s_act.mode != mode ? BENCH_END_MODE_CHANGED : BENCH_END_TARGET_CHANGED,
                      water_c);
            start_bench(mode, target_c, water_c);
        }
    } else {
        if (s_act.in_progress) end_bench(BENCH_END_TARGET_REACHED, water_c);
    }
    xSemaphoreGive(s_lock);
}
