#include "event_log.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "event_log";

#define NVS_PART      "storage"
#define NVS_NS        "evtlog"
#define NVS_BLOB_KEY  "log"
#define NVS_META_KEY  "meta"

typedef struct __attribute__((packed)) {
    uint32_t boot_num;
    uint16_t head;        /* next write index */
    uint16_t count;       /* 0..EVENT_LOG_CAP */
} meta_t;

static event_record_t s_ring[EVENT_LOG_CAP];
static meta_t  s_meta;
static SemaphoreHandle_t s_lock;
static volatile bool s_dirty;
static volatile bool s_inited;

uint32_t event_log_current_boot(void) { return s_meta.boot_num; }

static bool wall_clock_available(uint64_t *out_unix)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;   /* clearly pre-NTP */
    if (out_unix) *out_unix = (uint64_t)now;
    return true;
}

static void load_locked(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_meta);
    if (nvs_get_blob(h, NVS_META_KEY, &s_meta, &sz) != ESP_OK || sz != sizeof(s_meta)) {
        s_meta = (meta_t){ 0 };
    }
    sz = sizeof(s_ring);
    if (nvs_get_blob(h, NVS_BLOB_KEY, s_ring, &sz) != ESP_OK || sz != sizeof(s_ring)) {
        memset(s_ring, 0, sizeof(s_ring));
        s_meta = (meta_t){ 0 };
    }
    nvs_close(h);
}

static void save_locked(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_META_KEY, &s_meta, sizeof(s_meta));
    nvs_set_blob(h, NVS_BLOB_KEY, s_ring, sizeof(s_ring));
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
}

void event_log_flush(void)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) save_locked();
    xSemaphoreGive(s_lock);
}

static void flusher_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        event_log_flush();
    }
}

void event_log_emit(event_type_t type, int16_t ia, int16_t ib, const char *text)
{
    if (!s_inited) return;
    event_record_t r = {
        .boot_num = s_meta.boot_num,
        .uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL),
        .type     = (uint8_t)type,
        .a        = ia,
        .b        = ib,
    };
    uint64_t now_unix;
    if (wall_clock_available(&now_unix)) {
        r.ts_unix = now_unix;
        r.flags  |= 0x01;
    }
    if (text) {
        strncpy(r.text, text, sizeof(r.text) - 1);
        r.text[sizeof(r.text) - 1] = '\0';
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ring[s_meta.head] = r;
    s_meta.head = (uint16_t)((s_meta.head + 1) % EVENT_LOG_CAP);
    if (s_meta.count < EVENT_LOG_CAP) s_meta.count++;
    s_dirty = true;
    xSemaphoreGive(s_lock);
}

int event_log_snapshot(event_record_t *out, int cap)
{
    if (!out || cap <= 0 || !s_inited) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = (int)s_meta.count;
    if (n > cap) n = cap;
    /* newest-first iteration */
    int idx = (int)s_meta.head;
    for (int i = 0; i < n; ++i) {
        idx = (idx == 0) ? (EVENT_LOG_CAP - 1) : (idx - 1);
        out[i] = s_ring[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t event_log_init(void)
{
    if (s_inited) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_locked();
    s_meta.boot_num++;
    save_locked();
    xSemaphoreGive(s_lock);
    s_inited = true;

    event_log_emit(EV_BOOT, 0, 0, NULL);
    ESP_LOGI(TAG, "event log ready (boot %lu, %u/%u stored)",
             (unsigned long)s_meta.boot_num, s_meta.count, EVENT_LOG_CAP);

    if (xTaskCreate(flusher_task, "evlog", 2048, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
