#include "relays.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "relays";

#define GPIO_RELAY_1   47
#define GPIO_RELAY_2   42

/* The 2-channel optocoupler relay module (SRD-05VDC-SL-C / SongLe) is
 * active-LOW: pulling IN1/IN2 to GND energises the coil via the on-board
 * NPN driver. A HIGH input keeps the relay de-energised. */
#define RELAY_LEVEL_ON   0
#define RELAY_LEVEL_OFF  1

#define STAGGER_US     (300 * 1000)  /* 300 ms */

static const int s_pins[RELAY_COUNT] = { GPIO_RELAY_1, GPIO_RELAY_2 };

static SemaphoreHandle_t s_lock;
static bool s_actual[RELAY_COUNT];
static bool s_desired[RELAY_COUNT];
static uint64_t s_last_energise_us;

static void apply_locked(void)
{
    uint64_t now = esp_timer_get_time();
    for (int i = 0; i < RELAY_COUNT; ++i) {
        if (s_actual[i] && !s_desired[i]) {
            /* turn off immediately */
            s_actual[i] = false;
            gpio_set_level(s_pins[i], RELAY_LEVEL_OFF);
        }
    }
    for (int i = 0; i < RELAY_COUNT; ++i) {
        if (!s_actual[i] && s_desired[i]) {
            if (now - s_last_energise_us < STAGGER_US) continue;  /* defer */
            s_actual[i] = true;
            s_last_energise_us = now;
            gpio_set_level(s_pins[i], RELAY_LEVEL_ON);
        }
    }
}

static void stagger_task(void *arg)
{
    (void)arg;
    /* Wake periodically to honour deferred turn-ons. */
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply_locked();
        bool deferred = false;
        for (int i = 0; i < RELAY_COUNT; ++i) {
            if (!s_actual[i] && s_desired[i]) deferred = true;
        }
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(deferred ? 50 : 500));
    }
}

esp_err_t relays_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* Make the first energise pass the stagger gate without delay. Without
     * this, `now - 0 < STAGGER_US` is true for the first 300 ms of boot and
     * the very first relay turn-on is silently deferred. */
    s_last_energise_us = (uint64_t)0 - (uint64_t)STAGGER_US;

    /* Drive the output register HIGH (= relay OFF for active-LOW) BEFORE the
     * pin is switched to output mode so the line never glitches LOW during
     * configuration. The pin still floats up via the module's own input
     * pull-up while we're a high-Z input. */
    for (int i = 0; i < RELAY_COUNT; ++i) gpio_set_level(s_pins[i], RELAY_LEVEL_OFF);

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_RELAY_1) | (1ULL << GPIO_RELAY_2),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* defensive: keep IN pin high pre-config */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    for (int i = 0; i < RELAY_COUNT; ++i) gpio_set_level(s_pins[i], RELAY_LEVEL_OFF);

    if (xTaskCreate(stagger_task, "relay", 2048, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void relays_set(bool relay_on[RELAY_COUNT])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) s_desired[i] = relay_on[i];
    apply_locked();
    xSemaphoreGive(s_lock);
}

void relays_all_off(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) {
        s_desired[i] = false;
        s_actual[i] = false;
        gpio_set_level(s_pins[i], RELAY_LEVEL_OFF);
    }
    xSemaphoreGive(s_lock);
}

void relays_get(bool relay_on[RELAY_COUNT])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RELAY_COUNT; ++i) relay_on[i] = s_actual[i];
    xSemaphoreGive(s_lock);
}
