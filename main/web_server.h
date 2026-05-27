/* web_server — HTTP REST + WebSocket interface to the device.
 *
 * Auth model:
 *   - Public routes:  /, static assets, /api/auth/status, /api/auth/setup
 *                     (only when device is unconfigured), /api/auth/login
 *   - Everything else requires Authorization: Bearer <token>
 *
 * Live state pushed via WebSocket at /ws (~1 Hz).
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_server_start(void);
void      web_server_stop(void);

/* Forcibly close every active WebSocket session and clear the slot table.
 * Used right before wifi_mgr_restart() so the old sockets don't sit in
 * httpd's worker poll loop spewing recv errors after the radio is torn
 * down. */
void      web_server_close_all_ws(void);

#ifdef __cplusplus
}
#endif
