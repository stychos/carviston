#include "web_assets.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"

/* The packed archive embedded by target_add_binary_data(web_assets.bin). */
extern const uint8_t web_assets_bin_start[] asm("_binary_web_assets_bin_start");
extern const uint8_t web_assets_bin_end[]   asm("_binary_web_assets_bin_end");

static const char *TAG = "web_assets";

/* Plenty for the handful of files Vite emits; bump if the bundle ever grows
 * past this. Excess entries are dropped with a warning rather than crashing. */
#define MAX_ASSETS 48

static web_asset_t        s_assets[MAX_ASSETS];
static int                s_count;
static const web_asset_t *s_index;

/* The blob lives in memory-mapped flash; multi-byte fields aren't guaranteed
 * aligned, so assemble them byte-wise rather than dereferencing a cast ptr. */
static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void web_assets_init(void)
{
    if (s_count) return;

    const uint8_t *base  = web_assets_bin_start;
    const size_t   total = (size_t)(web_assets_bin_end - web_assets_bin_start);

    if (total < 12 || memcmp(base, "WBND", 4) != 0) {
        ESP_LOGE(TAG, "embedded web archive missing or invalid (%u bytes)", (unsigned)total);
        return;
    }
    uint32_t ver   = rd_u32(base + 4);
    uint32_t count = rd_u32(base + 8);
    if (ver != 1) {
        ESP_LOGE(TAG, "web archive version %u unsupported", (unsigned)ver);
        return;
    }

    const uint8_t *p     = base + 12;
    const uint8_t *limit = base + total;

    for (uint32_t i = 0; i < count; ++i) {
        /* Use the remaining-bytes form everywhere ((limit - p) can't wrap)
         * so a corrupt/truncated archive can never advance p past `limit`. */
        if (limit - p < 2) break;
        uint16_t plen = rd_u16(p); p += 2;
        const char *path = (const char *)p;
        if (limit - p < (ptrdiff_t)plen + 1) break;
        if (path[plen] != '\0') break;       /* must be NUL-terminated for strcmp/%s */
        p += plen + 1;                        /* string + NUL */

        if (limit - p < 2) break;
        uint16_t mlen = rd_u16(p); p += 2;
        const char *mime = (const char *)p;
        if (limit - p < (ptrdiff_t)mlen + 1) break;
        if (mime[mlen] != '\0') break;
        p += mlen + 1;

        if (limit - p < 1 + 4 + 4) break;
        uint8_t  flags = *p++;
        uint32_t off   = rd_u32(p); p += 4;
        uint32_t dlen  = rd_u32(p); p += 4;

        /* Overflow-safe range check: off + dlen can wrap on the 32-bit target,
         * so test against the remaining space without ever computing the sum. */
        if (off > total || dlen > total - off) {
            ESP_LOGW(TAG, "asset '%s' data out of range (off=%u len=%u)",
                     path, (unsigned)off, (unsigned)dlen);
            continue;
        }
        if (s_count >= MAX_ASSETS) {
            ESP_LOGW(TAG, "more than %d assets; '%s' and rest dropped", MAX_ASSETS, path);
            break;
        }

        s_assets[s_count] = (web_asset_t){
            .path = path,
            .mime = mime,
            .data = base + off,
            .len  = dlen,
            .gzip = (flags & 1) != 0,
        };
        if (strcmp(path, "/index.html") == 0) s_index = &s_assets[s_count];
        s_count++;
    }

    ESP_LOGI(TAG, "embedded web: %d assets, %u bytes total", s_count, (unsigned)total);
    if (!s_index) ESP_LOGW(TAG, "no /index.html in embedded web archive");
}

const web_asset_t *web_assets_get(const char *path)
{
    for (int i = 0; i < s_count; ++i) {
        if (strcmp(s_assets[i].path, path) == 0) return &s_assets[i];
    }
    return NULL;
}

const web_asset_t *web_assets_index(void) { return s_index; }
int                web_assets_count(void) { return s_count; }
