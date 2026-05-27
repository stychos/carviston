#include "safety.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "relays.h"

static const char *TAG = "safety";

#define GPIO_CUTOFF_SENSE 21

/* Soft over-temperature limit. The hardware KSD301 trips much higher, but
 * we want firmware to bail before the hardware does. */
#define SOFT_OVERTEMP_C   95.0f

static SemaphoreHandle_t s_lock;
static safety_status_t s_status = SAFETY_OK;
static bool s_sticky = false;

esp_err_t safety_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* Pull-up MUST stay disabled — it would clamp the input HIGH and mask
     * a tripped KSD301. Pull-down is ENABLED so a broken sense wire reads
     * LOW (= fault) instead of floating HIGH from leakage/coupling and
     * silently reporting "rail intact" when we can't actually see it.
     * The external divider must drive GPIO21 comfortably above the IO HIGH
     * threshold (~2.0 V) even with the internal ~45 kΩ pull-down loading it. */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_CUTOFF_SENSE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

bool safety_cutoff_intact(void)
{
    /* HIGH = cutoff loop intact (rail powered, pulls input high through divider). */
    return gpio_get_level(GPIO_CUTOFF_SENSE) == 1;
}

safety_status_t safety_evaluate(const temperature_reading_t *temp)
{
    safety_status_t s = SAFETY_OK;

    if (!safety_cutoff_intact())            s = SAFETY_FAULT_CUTOFF;
    else if (!temp)                          s = SAFETY_FAULT_NO_PROBES;
    else if (temp->all_fault)                s = SAFETY_FAULT_NO_PROBES;
    else if (temp->any_fault)                s = SAFETY_FAULT_PROBE;
    else if (temp->water_c >= SOFT_OVERTEMP_C) s = SAFETY_FAULT_OVERTEMP;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s != SAFETY_OK) {
        if (!s_sticky) {
            ESP_LOGW(TAG, "safety fault latched: %d", (int)s);
            relays_all_off();
        }
        s_status = s;
        s_sticky = true;
    } else if (!s_sticky) {
        s_status = SAFETY_OK;
    }
    /* If sticky and current input ok, status stays at the latched fault. */
    safety_status_t out = s_status;
    xSemaphoreGive(s_lock);
    return out;
}

safety_status_t safety_status(void)
{
    safety_status_t s;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s = s_status;
    xSemaphoreGive(s_lock);
    return s;
}

bool safety_is_ok(void)
{
    return safety_status() == SAFETY_OK;
}

void safety_clear_fault(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sticky = false;
    s_status = SAFETY_OK;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "safety fault cleared (will re-latch if condition persists)");
}
