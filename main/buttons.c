#include "buttons.h"

#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "buttons";

#define POLL_MS         20
#define DEBOUNCE_TICKS  2   /* 2 polls (40 ms) of stable state */

static const gpio_num_t s_pins[BTN_COUNT] = {
    [BTN_POWER]  = 16,
    [BTN_ECO]    = 17,
    [BTN_SHOWER] = 18,
    /* PLUS/MINUS are on 41/40 (swapped from the original 40/41) to match the
     * as-built wiring — the +/- pads were crossed on the harness. */
    [BTN_PLUS]   = 41,
    [BTN_MINUS]  = 40,
};

typedef struct {
    uint8_t  stable;          /* debounced level: 1=idle, 0=pressed */
    uint8_t  candidate;       /* level we're considering */
    uint8_t  candidate_ticks; /* ticks of stable candidate before accept */
    uint64_t pressed_us;      /* timestamp when last debounced press began */
    bool     long_emitted;
} btn_state_t;

static btn_state_t s_btn[BTN_COUNT];
static QueueHandle_t s_queue;
static _Atomic uint8_t s_held_mask;

static const char *kind_name(button_kind_t k)
{
    switch (k) {
        case BTN_KIND_PRESS:   return "PRESS";
        case BTN_KIND_RELEASE: return "RELEASE";
        case BTN_KIND_LONG:    return "LONG";
        default:               return "?";
    }
}

static const char *id_name(uint8_t id)
{
    switch (id) {
        case BTN_POWER:  return "POWER";
        case BTN_ECO:    return "ECO";
        case BTN_SHOWER: return "SHOWER";
        case BTN_PLUS:   return "PLUS";
        case BTN_MINUS:  return "MINUS";
        default:         return "?";
    }
}

static void send(uint8_t id, button_kind_t k, uint32_t held_ms)
{
    button_event_t ev = { .id = id, .kind = (uint8_t)k,
                          .held_ms = (uint16_t)(held_ms > 0xFFFFu ? 0xFFFFu : held_ms) };
    ESP_LOGI(TAG, "%s %s held=%lu ms", id_name(id), kind_name(k), (unsigned long)held_ms);
    xQueueSend(s_queue, &ev, 0);
}

static void button_task(void *arg)
{
    (void)arg;

    /* Initial state: all idle (high). */
    for (int i = 0; i < BTN_COUNT; ++i) s_btn[i].stable = 1;

    uint32_t long_press_ms = 1500;

    for (;;) {
        /* Refresh long-press threshold occasionally — cheap. */
        static uint32_t cfg_refresh_counter;
        if (cfg_refresh_counter++ % 50 == 0) {
            app_config_t c;
            app_config_get(&c);
            long_press_ms = c.long_press_ms ? c.long_press_ms : 1500;
        }

        uint64_t now_us = esp_timer_get_time();
        uint8_t held = 0;

        for (size_t i = 0; i < BTN_COUNT; ++i) {
            /* Capacitive sensor buttons are active-HIGH (HIGH = touched).
             * Invert to the driver's internal convention: 0 = pressed, 1 = idle. */
            uint8_t raw = gpio_get_level(s_pins[i]) ? 0u : 1u;
            btn_state_t *b = &s_btn[i];

            if (raw == b->stable) {
                b->candidate_ticks = 0;
            } else {
                if (raw == b->candidate) {
                    if (++b->candidate_ticks >= DEBOUNCE_TICKS) {
                        b->stable = raw;
                        b->candidate_ticks = 0;
                        if (raw == 0) {
                            /* press */
                            b->pressed_us = now_us;
                            b->long_emitted = false;
                            send((uint8_t)i, BTN_KIND_PRESS, 0);
                        } else {
                            /* release */
                            uint32_t held_ms = (uint32_t)((now_us - b->pressed_us) / 1000ULL);
                            send((uint8_t)i, BTN_KIND_RELEASE, held_ms);
                        }
                    }
                } else {
                    b->candidate = raw;
                    b->candidate_ticks = 1;
                }
            }

            /* long-press emission */
            if (b->stable == 0 && !b->long_emitted) {
                uint32_t held_ms = (uint32_t)((now_us - b->pressed_us) / 1000ULL);
                if (held_ms >= long_press_ms) {
                    b->long_emitted = true;
                    send((uint8_t)i, BTN_KIND_LONG, held_ms);
                }
            }
            if (b->stable == 0) held |= (uint8_t)(1u << i);
        }

        atomic_store(&s_held_mask, held);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t buttons_init(void)
{
    uint64_t mask = 0;
    for (size_t i = 0; i < BTN_COUNT; ++i) mask |= 1ULL << s_pins[i];
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_queue = xQueueCreate(16, sizeof(button_event_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(button_task, "btn", 2560, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

QueueHandle_t buttons_event_queue(void) { return s_queue; }

bool buttons_is_held(button_id_t id)
{
    if (id >= BTN_COUNT) return false;
    return (atomic_load(&s_held_mask) >> id) & 1u;
}
