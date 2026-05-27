/* wifi_mgr — AP / STA / hybrid network bring-up.
 *
 *   AP:     unconditional SoftAP. SSID/pass from config (defaults derived
 *           from MAC if unset).
 *   STA:    join configured network. Failed connects retry with exponential
 *           backoff (1 s → 30 s, reset on success) so a wrong password can't
 *           starve the rest of the firmware on the same core.
 *   HYBRID: APSTA — SoftAP and STA come up simultaneously. AP is reachable
 *           instantly; STA tries in the background and promotes the state
 *           to STA_CONNECTED on got-IP. AP stays up as a permanent
 *           fall-back path. No blocking on app_main.
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

typedef struct {
    bool ok;
    char error[64];   /* human-readable failure reason; empty on success */
} wifi_test_result_t;

/* Attempt to associate with `ssid`/`password` and obtain an IP, blocking
 * up to `timeout_s` seconds. The radio is restored to the prior config
 * regardless of outcome; the call does NOT persist anything to NVS.
 *
 * Must be called while the AP is up (pure AP or APSTA hybrid). Returns
 * ESP_ERR_INVALID_STATE if currently in pure STA mode, since testing
 * would otherwise disconnect the caller.
 *
 * Returns ESP_OK with out->ok=true on association+IP within timeout;
 * ESP_OK with out->ok=false and out->error populated on failure;
 * ESP_ERR_* if the request can't be attempted at all. */
esp_err_t wifi_mgr_test_sta(const char *ssid, const char *password,
                            int timeout_s, wifi_test_result_t *out);

#ifdef __cplusplus
}
#endif
