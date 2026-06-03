/* event_log — small ring-buffer of operational events.
 *
 *   - 128 records × 64 B = 8 KB, in RAM.
 *   - Persisted as a single NVS blob; flushed every 60 s when dirty plus on
 *     graceful reboot/factory reset.
 *   - Wall-clock timestamps when SNTP has synced; otherwise (boot_num,
 *     uptime_s) lets the UI order events monotonically.
 *
 * Event payload is two int16_ts + a short string for descriptions; consumers
 * interpret the int16s per event type (e.g. for TARGET_CHANGE, a = new °C).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EV_BOOT = 0,
    EV_SHUTDOWN,
    EV_REBOOT_REQUEST,
    EV_FACTORY_RESET,
    EV_WIFI_AP,
    EV_WIFI_STA_CONNECTED,
    EV_WIFI_STA_DISCONNECTED,
    EV_TIME_SYNCED,
    EV_BUTTON_PRESS,
    EV_BUTTON_LONG,
    EV_MODE_CHANGE,
    EV_TARGET_CHANGE,
    EV_MASTER_ON,
    EV_MASTER_OFF,
    EV_HEATER_ON,
    EV_HEATER_OFF,
    EV_SAFETY_FAULT,
    EV_SAFETY_CLEARED,
    EV_OTA_START,
    EV_OTA_DONE,
    EV_OTA_FAIL,
    EV_BENCH_START,
    EV_BENCH_END,
    EV_BENCH_ABORT,
    EV_TYPE_COUNT,
} event_type_t;

typedef struct __attribute__((packed)) {
    uint64_t ts_unix;       /* seconds since epoch; 0 if no wall clock yet */
    uint32_t boot_num;
    uint32_t uptime_s;
    uint8_t  type;          /* event_type_t */
    uint8_t  flags;         /* bit0: ts_unix is real wall clock */
    int16_t  a;
    int16_t  b;
    char     text[36];      /* total: 64 bytes */
} event_record_t;

#define EVENT_LOG_CAP   128

esp_err_t event_log_init(void);

/* Persist now (e.g. before reboot). */
void event_log_flush(void);

uint32_t event_log_current_boot(void);

/* Append an event. ia/ib are type-specific (may be 0). text is optional
 * (NULL → empty). Any extra characters are truncated. Every call also
 * prints the event to the serial console at INFO level so `idf.py monitor`
 * shows a live trace alongside the in-memory ring. */
void event_log_emit(event_type_t type, int16_t ia, int16_t ib, const char *text);

/* Human-readable snake_case name for an event type — "target_change",
 * "wifi_sta_connected", etc. Returns "unknown" for out-of-range values.
 * Used by both the serial-log path and the JSON HTTP endpoint. */
const char *event_type_name(event_type_t type);

/* Fill `out` (up to `cap` slots) with the newest-first chronological order.
 * Returns count written. */
int event_log_snapshot(event_record_t *out, int cap);

/* Erase every stored event and persist the empty ring. boot_num is preserved.
 * Also empties the derived heating-performance view (benchmarks live in the
 * event log). */
void event_log_clear(void);

/* Drop only the benchmark records (bench_start/end/abort), keeping the rest of
 * the event log. Used by the "clear heating performance" action. */
void event_log_purge_benchmarks(void);

#ifdef __cplusplus
}
#endif
