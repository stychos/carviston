/* web_assets — the Vue frontend, embedded straight into the firmware image.
 *
 * Replaces the old SPIFFS 'web' partition: the build packs every built file
 * into one archive (see tools/pack-web.mjs) which is linked in via
 * target_add_binary_data. There is no filesystem and nothing to mount — the
 * assets live in memory-mapped flash and are served directly. Because the UI
 * ships inside the firmware, a firmware OTA always carries its matching UI.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char    *path;   /* request path, e.g. "/index.html" (null-terminated) */
    const char    *mime;   /* Content-Type (null-terminated) */
    const uint8_t *data;    /* points into the embedded blob (flash) */
    uint32_t       len;
    bool           gzip;    /* data is gzip-compressed → send Content-Encoding: gzip */
} web_asset_t;

/* Parse the embedded archive into the lookup table. Safe to call more than
 * once; subsequent calls are no-ops. */
void web_assets_init(void);

/* Exact path lookup. Returns NULL if there is no such asset. */
const web_asset_t *web_assets_get(const char *path);

/* The "/index.html" entry, or NULL if absent — used as the SPA fallback. */
const web_asset_t *web_assets_index(void);

int web_assets_count(void);

#ifdef __cplusplus
}
#endif
