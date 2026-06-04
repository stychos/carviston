/* wifi_mgr — AP / STA network bring-up.
 *
 *   AP:  unconditional SoftAP. SSID/pass from config (defaults derived
 *        from MAC if unset).
 *   STA: join configured network. Failed connects retry with exponential
 *        backoff (1 s → 30 s, reset on success) so a wrong password can't
 *        starve the rest of the firmware on the same core. If no IP arrives
 *        within sta_fallback_seconds, the radio is promoted to APSTA and a
 *        recovery AP comes up so the device stays reachable to fix settings.
 *
 * On first boot (!configured) we always force AP mode regardless of stored
 * wifi_mode so the user has a path to set the password.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_STATE_OFF = 0,
    WIFI_STATE_AP,
    WIFI_STATE_STA_CONNECTING,
    WIFI_STATE_STA_CONNECTED,
    WIFI_STATE_STA_FAILED,
} wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;     /* WIFI_AUTH_OPEN etc. */
    uint8_t channel;
} wifi_scan_entry_t;

esp_err_t wifi_mgr_init(void);

/* Register a callback invoked just before any operation that disrupts
 * existing TCP sockets on the AP/STA netifs (restart, force-AP, scan in
 * AP mode, credential test). web_server_start() registers
 * web_server_close_all_ws here so a stale browser WebSocket doesn't sit
 * in httpd's recv loop spewing "error in recv : 128" while the radio is
 * being torn down or its channel reshuffled. */
typedef void (*wifi_mgr_disruption_cb_t)(void);
void wifi_mgr_set_disruption_cb(wifi_mgr_disruption_cb_t cb);

/* Bring up the radio based on current app_config. Returns immediately;
 * connection state evolves asynchronously. */
esp_err_t wifi_mgr_start(void);

/* Tear down and bring back up (e.g. after wifi config change). */
esp_err_t wifi_mgr_restart(void);

/* Force AP mode immediately. If an AP SSID/pass is configured, uses that;
 * otherwise behaves as first-boot (open carviston-XXXXXX). Does not modify
 * the stored wifi_mode in app_config — next reboot uses the saved setting. */
esp_err_t wifi_mgr_force_ap_mode(void);

/* True once SNTP has populated wall-clock time. */
bool wifi_mgr_time_synced(void);

wifi_state_t wifi_mgr_state(void);

/* True if either STA is connected OR AP has clients. */
bool wifi_mgr_is_up(void);

/* Currently connected STA SSID, or "" if not STA. */
const char *wifi_mgr_current_ssid(void);

/* Active SoftAP SSID, or "" if no AP is up. Returns the effective SSID,
 * which may be the MAC-derived default ("carviston-xxxxxx") if no
 * ap_ssid was configured. */
const char *wifi_mgr_ap_ssid(void);

/* RSSI of STA link, or 0 if not connected. */
int8_t wifi_mgr_rssi(void);

/* Scan nearby APs (synchronous, ~3s). Fills entries up to max_entries; returns count. */
int wifi_mgr_scan(wifi_scan_entry_t *entries, int max_entries);

/* True if a SoftAP is currently serving (pure AP, or the APSTA recovery AP).
 * Used to decide whether a connection test can run synchronously: when an AP is
 * up the caller's link survives the test; in pure STA it does not. */
bool wifi_mgr_ap_serving(void);

typedef struct {
    bool ok;
    char error[64];   /* human-readable failure reason; empty on success */
} wifi_test_result_t;

/* Attempt to associate with `ssid`/`password` and obtain an IP, blocking
 * up to `timeout_s` seconds. The radio is restored to the prior config
 * regardless of outcome; the call does NOT persist anything to NVS.
 *
 * Works from any started mode. In pure STA it drops the live link to run the
 * test and then actively rejoins the remembered network — so it BLOCKS for the
 * whole window and the caller (if reached over that link) is cut off; prefer
 * wifi_mgr_test_sta_async() there. Synchronous use is for when an AP is up.
 *
 * Returns ESP_OK with out->ok=true on association+IP within timeout;
 * ESP_OK with out->ok=false and out->error populated on failure;
 * ESP_ERR_* if the request can't be attempted at all. */
esp_err_t wifi_mgr_test_sta(const char *ssid, const char *password,
                            int timeout_s, wifi_test_result_t *out);

typedef struct {
    bool running;     /* an async test is in progress */
    bool done;        /* a finished result is available (cleared when a new test starts) */
    bool ok;          /* verdict of the last finished test */
    char error[64];
} wifi_test_async_t;

/* Run wifi_mgr_test_sta() on a background task and return immediately. For pure
 * STA, where the test must drop the very link carrying the HTTP request: the
 * caller answers the client right away, the device disconnects → tests → rejoins
 * the remembered network, and the client polls wifi_mgr_test_result() once it
 * can reach the device again. Returns ESP_ERR_INVALID_STATE if a test is already
 * running. */
esp_err_t wifi_mgr_test_sta_async(const char *ssid, const char *password, int timeout_s);

/* Snapshot the async-test state (running/done/ok/error). */
void wifi_mgr_test_result(wifi_test_async_t *out);

#ifdef __cplusplus
}
#endif
