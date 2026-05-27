/* ota — firmware update via HTTP upload.
 *
 * The web UI POSTs a raw .bin to /api/ota. We write it to the inactive OTA
 * partition with esp_ota_*, validate the header, mark it as the boot
 * partition, and reboot.
 */

#pragma once

#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* httpd handler — registered by web_server.c at /api/ota. */
esp_err_t ota_upload_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
