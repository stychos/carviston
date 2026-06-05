/* Carviston firmware entry point. */

#include "esp_log.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "auth.h"
#include "benchmark.h"
#include "buttons.h"
#include "event_log.h"
#include "heater_control.h"
#include "leds.h"
#include "relays.h"
#include "safety.h"
#include "temperature.h"
#include "web_server.h"
#include "wifi_mgr.h"

static const char *TAG = "carviston";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    err = nvs_flash_init_partition("storage");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("storage"));
        err = nvs_flash_init_partition("storage");
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(app_config_init());
    ESP_ERROR_CHECK(event_log_init());
    ESP_ERROR_CHECK(benchmark_init());

    char ap[24];
    app_config_default_ap_ssid(ap, sizeof(ap));
    ESP_LOGI(TAG, "carviston boot; default AP SSID: %s", ap);

    ESP_ERROR_CHECK(leds_init());
    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(relays_init());
    ESP_ERROR_CHECK(safety_init());
    ESP_ERROR_CHECK(temperature_init());
    ESP_ERROR_CHECK(heater_control_init());

    ESP_ERROR_CHECK(auth_init());
    ESP_ERROR_CHECK(wifi_mgr_init());
    ESP_ERROR_CHECK(wifi_mgr_start());
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG, "subsystems up; web server running on :80");
}
