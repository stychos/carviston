#include "web_fs.h"

#include <sys/stat.h>

#include "esp_log.h"
#include "esp_spiffs.h"

static const char *TAG = "web_fs";
static bool s_has_bundle;

esp_err_t web_fs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/web",
        .partition_label = "web",
        .max_files = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spiffs mount failed: %s — falling back to embedded UI",
                 esp_err_to_name(err));
        s_has_bundle = false;
        return ESP_OK;   /* non-fatal */
    }
    struct stat st;
    if (stat("/web/index.html", &st) == 0 && st.st_size > 0) {
        s_has_bundle = true;
        ESP_LOGI(TAG, "web bundle mounted (index.html: %ld bytes)", (long)st.st_size);
    } else {
        s_has_bundle = false;
        ESP_LOGW(TAG, "web partition mounted but no index.html — using embedded UI");
    }
    return ESP_OK;
}

bool web_fs_has_bundle(void) { return s_has_bundle; }

void web_fs_unmount(void)
{
    if (esp_spiffs_mounted("web")) {
        esp_vfs_spiffs_unregister("web");
        ESP_LOGI(TAG, "web spiffs unmounted");
    }
    s_has_bundle = false;
}
