/* web_fs — SPIFFS mount for the web bundle.
 *
 * Mounts the "web" partition at /spiffs. Returns whether the mount succeeded
 * and whether an index.html actually exists. If not, the web_server falls
 * back to a small embedded setup page.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t web_fs_init(void);
bool      web_fs_has_bundle(void);

/* Unregister the SPIFFS mount. Used by the OTA path before re-writing the
 * "web" partition so writes don't race with the mounted filesystem. The
 * device must reboot after the new bundle is written. */
void      web_fs_unmount(void);

#ifdef __cplusplus
}
#endif
