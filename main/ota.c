#include "ota.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "auth.h"
#include "event_log.h"

static const char *TAG = "ota";

#define CHUNK 4096

static bool bearer_ok(httpd_req_t *req)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = hdr;
    while (*p == ' ') p++;
    if (strncasecmp(p, "Bearer ", 7) != 0) return false;
    p += 7;
    return auth_validate_token(p);
}

static esp_err_t reply(httpd_req_t *req, int status, const char *msg)
{
    httpd_resp_set_status(req, status == 200 ? "200 OK" :
                               status == 400 ? "400 Bad Request" :
                               status == 401 ? "401 Unauthorized" :
                                               "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}",
             status == 200 ? "true" : "false", msg ? msg : "");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* Send the 200 "rebooting" reply, then reboot WITHOUT returning to the httpd
 * accept loop.
 *
 * The dashboard no longer depends on receiving this reply: it polls
 * /api/auth/status until the device answers again, then reloads (see
 * MaintenanceTab.vue). We still send the reply — LWIP's tcpip task flushes it
 * during the delay below, so a browser still listening on the socket gets it —
 * but the load-bearing part is that esp_restart() fires from *inside* the
 * handler.
 *
 * Why not return and reboot from a timer: returning hands control back to the
 * single-task HTTP server, which would keep answering on the OLD firmware for
 * the ~1 s before the reset — so a dashboard readiness-poll landing in that
 * gap would reload the OLD UI, only to need another reload once the new image
 * boots. Staying in the handler until the reset keeps the server dark until it
 * genuinely comes back, so the first poll that succeeds again is served by the
 * freshly-flashed firmware (which carries its matching embedded UI). */
static void reply_and_reboot(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
    reply(req, 200, "rebooting");

    vTaskDelay(pdMS_TO_TICKS(1200));   /* let LWIP push the reply out first */
    esp_restart();
}

/* Read `want` bytes from the request body into `dst`. Returns the count
 * actually read (which is `want` on success, or less if the peer closed
 * mid-stream). Negative on error. */
static int recv_exact(httpd_req_t *req, void *dst, int want)
{
    int got = 0;
    while (got < want) {
        int r = httpd_req_recv(req, (char *)dst + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return r;
        got += r;
    }
    return got;
}

/* --- firmware OTA path (ESP image magic 0xE9 at byte 0) ------------------- */
static esp_err_t handle_firmware(httpd_req_t *req, int total,
                                 const uint8_t *prefix, int prefix_len,
                                 bool reset_config)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) return reply(req, 500, "no ota partition");
    if ((uint32_t)total > update->size) {
        ESP_LOGW(TAG, "firmware %d > partition %lu", total, update->size);
        event_log_emit(EV_OTA_FAIL, 0, 0, "too_large");
        return reply(req, 400, "image too large");
    }
    ESP_LOGI(TAG, "OTA firmware → %s @0x%lx (%d bytes)",
             update->label, update->address, total);
    event_log_emit(EV_OTA_START, 0, 0, update->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin failed: %s", esp_err_to_name(err));
        event_log_emit(EV_OTA_FAIL, 0, 0, "ota_begin");
        return reply(req, 500, "ota_begin failed");
    }

    /* Stream in the prefix we already peeked at, then the rest. */
    err = esp_ota_write(handle, prefix, prefix_len);
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        event_log_emit(EV_OTA_FAIL, 0, 0, "ota_write_prefix");
        return reply(req, 500, "ota_write failed");
    }

    char *buf = malloc(CHUNK);
    if (!buf) { esp_ota_abort(handle); return reply(req, 500, "no mem"); }
    int recvd = prefix_len;
    while (recvd < total) {
        int want = (total - recvd) > CHUNK ? CHUNK : (total - recvd);
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf); esp_ota_abort(handle);
            event_log_emit(EV_OTA_FAIL, 0, 0, "recv");
            return reply(req, 500, "recv error");
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed: %s", esp_err_to_name(err));
            free(buf); esp_ota_abort(handle);
            event_log_emit(EV_OTA_FAIL, 0, 0, "ota_write");
            return reply(req, 500, "ota_write failed");
        }
        recvd += r;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end failed: %s", esp_err_to_name(err));
        event_log_emit(EV_OTA_FAIL, 0, 0, "ota_end");
        return reply(req, 400, "image invalid");
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot failed: %s", esp_err_to_name(err));
        event_log_emit(EV_OTA_FAIL, 0, 0, "set_boot");
        return reply(req, 500, "set_boot failed");
    }

    ESP_LOGI(TAG, "firmware OTA complete (%d bytes); rebooting", recvd);
    event_log_emit(EV_OTA_DONE, 0, 0, "fw");

    /* Optional "reset settings" — drop the config blob now, AFTER the image is
     * committed and set-boot succeeded, so a failed OTA never wipes config. The
     * new firmware then boots on defaults (network + auth preserved). */
    if (reset_config) {
        app_config_erase_feature_blob();
        event_log_emit(EV_OTA_DONE, 0, 0, "cfg_reset");
    }

    event_log_flush();
    reply_and_reboot(req);
    return ESP_OK;
}

/* ESP firmware image header (esp_app_format.h):
 *   byte 0  : ESP_IMAGE_HEADER_MAGIC = 0xE9
 *   byte 1  : segment_count (1..16 in practice)
 *   byte 12 : chip_id; ESP_CHIP_ID_ESP32S3 = 0x0009 (LE, so byte 12 = 0x09)
 * Checking all three rejects anything that isn't an ESP32-S3 app image. */
static bool looks_like_firmware(const uint8_t *p, int n)
{
    if (n < 16)                                 return false;
    if (p[0] != ESP_IMAGE_HEADER_MAGIC)         return false;
    if (p[1] == 0 || p[1] > 16)                 return false;
    if (p[12] != 0x09 /* ESP_CHIP_ID_ESP32S3 */) return false;
    return true;
}

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!bearer_ok(req)) return reply(req, 401, "unauthorized");

    int total = req->content_len;
    if (total <= 0) return reply(req, 400, "empty body");

    /* ?reset_config=1 → wipe settings to defaults as part of this update. */
    bool reset_config = false;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "reset_config", val, sizeof(val)) == ESP_OK
            && (val[0] == '1' || val[0] == 't' || val[0] == 'T'))
            reset_config = true;
    }

    /* Peek the 16-byte ESP image header (magic + chip_id) before touching any
     * flash, then re-stream those bytes as the start of the image. The web UI
     * now ships inside the firmware, so a firmware image is the only thing we
     * accept here. */
    uint8_t prefix[16] = { 0 };
    int prefix_len = (total < (int)sizeof(prefix)) ? total : (int)sizeof(prefix);
    int got = recv_exact(req, prefix, prefix_len);
    if (got != prefix_len) {
        ESP_LOGW(TAG, "header read short: %d/%d", got, prefix_len);
        event_log_emit(EV_OTA_FAIL, 0, 0, "short_header");
        return reply(req, 400, "could not read header");
    }

    if (looks_like_firmware(prefix, prefix_len)) {
        return handle_firmware(req, total, prefix, prefix_len, reset_config);
    }

    ESP_LOGW(TAG, "rejected upload: header=%02x %02x %02x %02x ... (not an ESP32-S3 firmware image)",
             prefix[0], prefix[1], prefix[2], prefix[3]);
    event_log_emit(EV_OTA_FAIL, 0, 0, "bad_image");
    return reply(req, 400, "not a firmware image");
}
