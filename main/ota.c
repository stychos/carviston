#include "ota.h"

#include <string.h>

#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.h"
#include "event_log.h"
#include "web_fs.h"

static const char *TAG = "ota";

#define CHUNK 4096

static void delayed_restart(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
}

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
                                 const uint8_t *prefix, int prefix_len)
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
    event_log_flush();
    reply(req, 200, "rebooting");
    xTaskCreate(delayed_restart, "reboot", 1024, NULL, 1, NULL);
    return ESP_OK;
}

/* --- web SPIFFS path (anything that isn't an ESP image) ------------------- */
static esp_err_t handle_web_bundle(httpd_req_t *req, int total,
                                   const uint8_t *prefix, int prefix_len)
{
    const esp_partition_t *web = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "web");
    if (!web) return reply(req, 500, "no web partition");
    if ((uint32_t)total > web->size) {
        ESP_LOGW(TAG, "web bundle %d > partition %lu", total, web->size);
        event_log_emit(EV_OTA_FAIL, 0, 0, "web_too_large");
        return reply(req, 400, "image too large");
    }
    ESP_LOGI(TAG, "OTA web bundle → %s @0x%lx (%d bytes)",
             web->label, web->address, total);
    event_log_emit(EV_OTA_START, 0, 0, "web");

    /* Drop the live mount before erasing — the VFS layer would otherwise
     * see corrupted pages mid-stream and could crash on any active read. */
    web_fs_unmount();

    esp_err_t err = esp_partition_erase_range(web, 0, web->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web erase failed: %s", esp_err_to_name(err));
        event_log_emit(EV_OTA_FAIL, 0, 0, "web_erase");
        return reply(req, 500, "erase failed");
    }

    err = esp_partition_write(web, 0, prefix, prefix_len);
    if (err != ESP_OK) {
        event_log_emit(EV_OTA_FAIL, 0, 0, "web_write_prefix");
        return reply(req, 500, "write failed");
    }

    char *buf = malloc(CHUNK);
    if (!buf) return reply(req, 500, "no mem");
    int recvd = prefix_len;
    while (recvd < total) {
        int want = (total - recvd) > CHUNK ? CHUNK : (total - recvd);
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            free(buf);
            event_log_emit(EV_OTA_FAIL, 0, 0, "web_recv");
            return reply(req, 500, "recv error");
        }
        err = esp_partition_write(web, recvd, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web write failed: %s", esp_err_to_name(err));
            free(buf);
            event_log_emit(EV_OTA_FAIL, 0, 0, "web_write");
            return reply(req, 500, "write failed");
        }
        recvd += r;
    }
    free(buf);

    ESP_LOGI(TAG, "web bundle written (%d bytes); rebooting to remount", recvd);
    event_log_emit(EV_OTA_DONE, 0, 0, "web");
    event_log_flush();
    reply(req, 200, "rebooting");
    xTaskCreate(delayed_restart, "reboot", 1024, NULL, 1, NULL);
    return ESP_OK;
}

/* ESP firmware image header (esp_app_format.h):
 *   byte 0    : ESP_IMAGE_HEADER_MAGIC = 0xE9
 *   byte 1    : segment_count (1..16 in practice)
 *   byte 12   : chip_id; ESP_CHIP_ID_ESP32S3 = 0x0009 (LE, so byte 12 = 0x09)
 * Verifying all three rules out a SPIFFS image that incidentally begins
 * with 0xE9. */
static bool looks_like_firmware(const uint8_t *p, int n)
{
    if (n < 16)                                return false;
    if (p[0] != ESP_IMAGE_HEADER_MAGIC)         return false;
    if (p[1] == 0 || p[1] > 16)                 return false;
    if (p[12] != 0x09 /* ESP_CHIP_ID_ESP32S3 */) return false;
    return true;
}

/* SPIFFS images produced by spiffsgen open with a Lookup (LU) page whose
 * first entry is a 16-bit little-endian object ID. Populated images always
 * begin with a file-index entry, which has bit 15 of that uint16 set —
 * i.e. byte[1] & 0x80. (An all-0xFF erased partition also passes this, but
 * we never accept short bodies, so an erased-only upload would still need
 * to match the partition size to do any damage.) */
static bool looks_like_web_bundle(const uint8_t *p, int n)
{
    if (n < 2)               return false;
    if ((p[1] & 0x80) == 0)  return false;
    return true;
}

esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!bearer_ok(req)) return reply(req, 401, "unauthorized");

    int total = req->content_len;
    if (total <= 0) return reply(req, 400, "empty body");

    /* Peek enough bytes to identify the payload before doing anything
     * destructive. 16 B covers the full firmware header (including chip_id)
     * and the SPIFFS LU-page check, and can be re-streamed as the start of
     * either destination. */
    uint8_t prefix[16] = { 0 };
    int prefix_len = (total < (int)sizeof(prefix)) ? total : (int)sizeof(prefix);
    int got = recv_exact(req, prefix, prefix_len);
    if (got != prefix_len) {
        ESP_LOGW(TAG, "header read short: %d/%d", got, prefix_len);
        event_log_emit(EV_OTA_FAIL, 0, 0, "short_header");
        return reply(req, 400, "could not read header");
    }

    if (looks_like_firmware(prefix, prefix_len)) {
        return handle_firmware(req, total, prefix, prefix_len);
    }
    if (looks_like_web_bundle(prefix, prefix_len)) {
        return handle_web_bundle(req, total, prefix, prefix_len);
    }

    ESP_LOGW(TAG, "rejected upload: header=%02x %02x %02x %02x ... (not a firmware or web image)",
             prefix[0], prefix[1], prefix[2], prefix[3]);
    event_log_emit(EV_OTA_FAIL, 0, 0, "bad_image");
    return reply(req, 400, "unrecognised image");
}
