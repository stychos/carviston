#include "app_config.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "app_config";

/* NVS namespace + blob key */
#define CFG_NVS_PART      "storage"
#define CFG_NVS_NS        "carviston"
#define CFG_NVS_BLOB_KEY  "cfg"

#define CFG_VERSION       7

/* Coalescing window for deferred saves — after a save is signalled we wait
 * this long for further changes to land before committing. Tunes the
 * trade-off between NVS wear (smaller = more writes) and the "saved
 * change visible after reboot" window (larger = more change lost on a
 * reboot in the window). 250 ms is comfortably below normal interactive
 * timing so a slider drag's last value lands within a few flushes. */
#define DEFERRED_SAVE_COALESCE_MS  250

static app_config_t s_cfg;
static SemaphoreHandle_t s_mutex;

/* Deferred-save plumbing: pending snapshot + a binary semaphore that the
 * worker task waits on. s_pending_valid distinguishes "no save needed"
 * from "save the all-zeros snapshot" after a signal arrives. */
static app_config_t      s_pending_cfg;
static bool              s_pending_valid;
static SemaphoreHandle_t s_save_sem;

static void save_worker_task(void *arg);

static void apply_defaults(app_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->version              = CFG_VERSION;
    strlcpy(c->device_name, "Carviston", sizeof(c->device_name));
    c->configured           = false;
    c->pbkdf2_iterations    = 10000;
    c->dashboard_locked     = false;       /* dashboard is open by default; only settings need a password */
    c->heating_mode         = HEATING_MODE_OPTIMAL;
    c->target_temp_c        = 60;
    c->shower_ready_c       = 50;
    /* Timings sized for a 100 L tank with 2× 1.5 kW elements. Cold-start
     * (15→60 °C, 45 °C rise, 18.8 MJ): SUPER_FAST ~105 min, FAST ~115 min,
     * OPTIMAL ~210 min, ECO ~7 h. FAST keeps a short rest so it's distinct
     * from SUPER_FAST and lets the elements de-stress briefly; ECO is 50%
     * duty (alternating heaters) so it isn't pathologically slow from cold. */
    c->fast_on_min          = 25;
    c->fast_rest_min        = 2;
    c->optimal_swap_min     = 10;
    c->eco_on_min           = 10;
    c->eco_rest_min         = 10;
    c->hysteresis_c         = 3;
    c->ntc_r25_ohm          = 10000;
    c->ntc_beta             = 3950;
    c->probe_disagree_c     = 5;
    c->power_led_mode       = POWER_LED_ALWAYS_ON;
    c->eco_led_mode         = ECO_LED_WIFI_STATE;
    c->dashboard_unit       = TEMP_UNIT_CELSIUS;
    c->wifi_mode            = WIFI_ROLE_AP;
    c->sta_fallback_enabled = true;
    c->sta_fallback_seconds = 60;
    c->long_press_ms        = 1500;
    c->preview_release_ms   = 3000;
    c->bench_resume_threshold_s = 600;
    c->restore_power_on_boot = true;        /* heater returns to its last on/off state after a
                                               reboot/power cut. A never-configured device still
                                               boots OFF, since last_power_state defaults to 0
                                               (memset) until the user turns it on at least once. */
    /* ap_ssid empty → derived from MAC at runtime */
}

void app_config_default_ap_ssid(char *buf, size_t buflen)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(buf, buflen, "carviston-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

esp_err_t app_config_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (!s_mutex) return ESP_ERR_NO_MEM;
    }
    if (s_save_sem == NULL) {
        s_save_sem = xSemaphoreCreateBinary();
        if (!s_save_sem) return ESP_ERR_NO_MEM;
        if (xTaskCreate(save_worker_task, "cfg_save", 4096, NULL, 2, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }

    apply_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_NVS_PART, CFG_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_PART_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config; using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s — using defaults", esp_err_to_name(err));
        return ESP_OK;
    }

    size_t sz = 0;
    err = nvs_get_blob(h, CFG_NVS_BLOB_KEY, NULL, &sz);
    if (err == ESP_OK && sz == sizeof(app_config_t)) {
        app_config_t tmp;
        if (nvs_get_blob(h, CFG_NVS_BLOB_KEY, &tmp, &sz) == ESP_OK &&
            tmp.version == CFG_VERSION) {
            s_cfg = tmp;
            ESP_LOGI(TAG, "config loaded (v%u, configured=%d)",
                     (unsigned)tmp.version, (int)tmp.configured);
        } else {
            ESP_LOGW(TAG, "config schema mismatch — using defaults");
        }
    } else {
        ESP_LOGI(TAG, "no stored blob — using defaults");
    }
    nvs_close(h);
    return ESP_OK;
}

void app_config_get(app_config_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mutex);
}

/* Push *in to NVS. Caller already updated the in-memory snapshot if it
 * wanted to. Returns the nvs error (or ESP_OK). */
static esp_err_t commit_to_nvs(const app_config_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CFG_NVS_PART, CFG_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open RW failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, CFG_NVS_BLOB_KEY, in, sizeof(*in));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) ESP_LOGI(TAG, "config saved");
    else ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t app_config_save(const app_config_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (in->version != CFG_VERSION) return ESP_ERR_INVALID_VERSION;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg = *in;
    xSemaphoreGive(s_mutex);

    return commit_to_nvs(in);
}

esp_err_t app_config_save_deferred(const app_config_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (in->version != CFG_VERSION) return ESP_ERR_INVALID_VERSION;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_cfg           = *in;
    s_pending_cfg   = *in;
    s_pending_valid = true;
    xSemaphoreGive(s_mutex);

    /* Binary semaphore — if it's already signalled we don't queue another
     * one, which is exactly the coalescing behaviour we want. */
    if (s_save_sem) xSemaphoreGive(s_save_sem);
    return ESP_OK;
}

/* Low-priority worker. Sleeps on s_save_sem; on wake waits a short window
 * for further changes, drains any subsequent signals, then commits the
 * latest snapshot to NVS. */
static void save_worker_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_save_sem, portMAX_DELAY);

        /* Coalesce: anything that arrives in the next 250 ms is folded in. */
        vTaskDelay(pdMS_TO_TICKS(DEFERRED_SAVE_COALESCE_MS));
        while (xSemaphoreTake(s_save_sem, 0) == pdTRUE) { }

        app_config_t snapshot;
        bool have = false;
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_pending_valid) {
            snapshot        = s_pending_cfg;
            have            = true;
            s_pending_valid = false;
        }
        xSemaphoreGive(s_mutex);

        if (have) commit_to_nvs(&snapshot);
    }
}

esp_err_t app_config_factory_reset(void)
{
    /* Wipe every namespace we own in the storage partition. Missing
     * namespaces here means a previous owner's event log, in-flight
     * benchmark, or future module data survives a hardware reset and is
     * served to the next user. */
    static const char *namespaces[] = { CFG_NVS_NS, "evtlog", "bench" };
    for (size_t i = 0; i < sizeof(namespaces) / sizeof(namespaces[0]); ++i) {
        nvs_handle_t h;
        if (nvs_open_from_partition(CFG_NVS_PART, namespaces[i],
                                    NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    apply_defaults(&s_cfg);
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "factory reset complete");
    return ESP_OK;
}

heating_mode_t app_config_get_heating_mode(void)
{
    heating_mode_t v;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    v = s_cfg.heating_mode;
    xSemaphoreGive(s_mutex);
    return v;
}

uint8_t app_config_get_target_temp(void)
{
    uint8_t v;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    v = s_cfg.target_temp_c;
    xSemaphoreGive(s_mutex);
    return v;
}

bool app_config_is_configured(void)
{
    bool v;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    v = s_cfg.configured;
    xSemaphoreGive(s_mutex);
    return v;
}

void app_config_get_device_name(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    /* Fall back to the product name when the user has cleared it. */
    const char *name = s_cfg.device_name[0] ? s_cfg.device_name : "Carviston";
    strlcpy(buf, name, len);
    xSemaphoreGive(s_mutex);
}

bool app_config_dashboard_locked(void)
{
    bool v;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    v = s_cfg.dashboard_locked;
    xSemaphoreGive(s_mutex);
    return v;
}
