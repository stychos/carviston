#include "leds.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "leds";

#define GPIO_POWER     8
#define GPIO_TEMP_40   9
#define GPIO_TEMP_50   10
#define GPIO_TEMP_60   11
#define GPIO_TEMP_70   12
#define GPIO_TEMP_80   13
#define GPIO_SHOWER    14
#define GPIO_ECO       15

#define RENDER_INTERVAL_MS  100   /* 10 Hz */

/* While heating, the next threshold LED above the current temp blinks. */
#define TEMP_BLINK_PERIOD_MS  1000
#define TEMP_BLINK_ON_MS      500

/* The 5 temp LEDs are a coarse 10 °C-step gauge (40..80). Light each step once
 * the temperature is within half a step of it (round to NEAREST mark) rather
 * than only after it's fully crossed — otherwise resting just below target
 * (e.g. 78 °C with an 80 °C target) would light only up to 70 and read as "70".
 * With this, 78 rounds to 80. Half of the 10 °C step: */
#define TEMP_LED_HALF_STEP_C  5.0f

static const gpio_num_t s_temp_leds[5] = {
    GPIO_TEMP_40, GPIO_TEMP_50, GPIO_TEMP_60, GPIO_TEMP_70, GPIO_TEMP_80,
};
static const uint8_t s_temp_thresholds[5] = { 40, 50, 60, 70, 80 };

static SemaphoreHandle_t s_lock;
static led_panel_state_t s_state;

/* --- preview state (temp row) --- */
static uint8_t  s_preview_target;          /* 0 = inactive */
static bool     s_preview_held;            /* true while +/- is down */
static uint64_t s_preview_release_us;      /* expiry when held=false */

/* --- mode-cycle animation (POWER/ECO/SHOWER row) ---
 *
 * Timeline (held in s_anim_*):
 *   t=0       blank top row for HOLD_MS
 *   t=HOLD    blink N times (HALF_ON + HALF_OFF per cycle)
 *   t=end     return to normal
 */
typedef struct {
    bool     active;
    uint64_t start_us;
    uint8_t  blink_count;        /* total blinks */
    uint8_t  blink_done;
} mode_anim_t;
static mode_anim_t s_mode_anim;

#define MODE_ANIM_BLANK_MS   500
#define MODE_ANIM_ON_MS      280
#define MODE_ANIM_OFF_MS     220

/* --- AP-mode animation: ECO blinks rapidly N times --- */
typedef struct {
    bool     active;
    uint64_t start_us;
    uint8_t  blink_count;
} ap_anim_t;
static ap_anim_t s_ap_anim;

#define AP_ANIM_ON_MS        120
#define AP_ANIM_OFF_MS       120
#define AP_ANIM_BLINKS       6

esp_err_t leds_init(void);  /* forward */

static inline void set(gpio_num_t pin, bool on) { gpio_set_level(pin, on ? 1 : 0); }

void leds_publish_state(const led_panel_state_t *s)
{
    if (!s) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = *s;
    xSemaphoreGive(s_lock);
}

void leds_target_preview_press(uint8_t target_c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_preview_target = target_c;
    s_preview_held   = true;
    xSemaphoreGive(s_lock);
}

void leds_target_preview_release(void)
{
    app_config_t cfg;
    app_config_get(&cfg);
    uint32_t linger_ms = cfg.preview_release_ms ? cfg.preview_release_ms : 3000;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_preview_held = false;
    s_preview_release_us = esp_timer_get_time() + (uint64_t)linger_ms * 1000ULL;
    xSemaphoreGive(s_lock);
}

void leds_animate_mode(uint8_t mode_index)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_mode_anim.active      = true;
    s_mode_anim.start_us    = esp_timer_get_time();
    s_mode_anim.blink_count = (uint8_t)(mode_index + 1);
    s_mode_anim.blink_done  = 0;
    xSemaphoreGive(s_lock);
}

void leds_animate_ap_mode(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ap_anim.active      = true;
    s_ap_anim.start_us    = esp_timer_get_time();
    s_ap_anim.blink_count = AP_ANIM_BLINKS;
    xSemaphoreGive(s_lock);
}

/* Render-time evaluators for the two animations.
 * Each returns whether the animation is currently overriding the affected
 * LEDs, and the override value via *led_on (when active=true).
 */
static bool eval_mode_anim(uint64_t now_us, bool *led_on)
{
    if (!s_mode_anim.active) return false;
    uint64_t t = now_us - s_mode_anim.start_us;
    if (t < (uint64_t)MODE_ANIM_BLANK_MS * 1000ULL) {
        *led_on = false;
        return true;
    }
    uint64_t cycle = (uint64_t)(MODE_ANIM_ON_MS + MODE_ANIM_OFF_MS) * 1000ULL;
    uint64_t into  = t - (uint64_t)MODE_ANIM_BLANK_MS * 1000ULL;
    uint32_t idx   = (uint32_t)(into / cycle);
    if (idx >= s_mode_anim.blink_count) {
        s_mode_anim.active = false;
        return false;
    }
    uint64_t phase = into - (uint64_t)idx * cycle;
    *led_on = phase < (uint64_t)MODE_ANIM_ON_MS * 1000ULL;
    return true;
}

static bool eval_ap_anim(uint64_t now_us, bool *led_on)
{
    if (!s_ap_anim.active) return false;
    uint64_t t = now_us - s_ap_anim.start_us;
    uint64_t cycle = (uint64_t)(AP_ANIM_ON_MS + AP_ANIM_OFF_MS) * 1000ULL;
    uint32_t idx = (uint32_t)(t / cycle);
    if (idx >= s_ap_anim.blink_count) {
        s_ap_anim.active = false;
        return false;
    }
    uint64_t phase = t - (uint64_t)idx * cycle;
    *led_on = phase < (uint64_t)AP_ANIM_ON_MS * 1000ULL;
    return true;
}

static void render_once(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    led_panel_state_t st = s_state;
    uint64_t now_us = esp_timer_get_time();

    /* --- temp row -------------------------------------------------------- */
    bool temp_on[5] = { false, false, false, false, false };
    bool preview_active = false;
    if (s_preview_target != 0) {
        if (s_preview_held || now_us < s_preview_release_us) {
            preview_active = true;
            for (size_t i = 0; i < 5; ++i)
                temp_on[i] = (s_temp_thresholds[i] == s_preview_target);
        } else {
            s_preview_target = 0;
        }
    }
    if (!preview_active && st.master_enabled && !isnan(st.current_temp_c)) {
        int next = -1;
        for (size_t i = 0; i < 5; ++i) {
            if (st.current_temp_c >= (float)s_temp_thresholds[i] - TEMP_LED_HALF_STEP_C) {
                temp_on[i] = true;   /* round to nearest mark */
            } else {
                next = (int)i;   /* first threshold not yet reached */
                break;
            }
        }
        /* Heating toward `next` → blink it at 1 Hz instead of leaving it dark. */
        if (next >= 0 && st.any_heater_active) {
            uint32_t t = (uint32_t)((now_us / 1000ULL) % TEMP_BLINK_PERIOD_MS);
            temp_on[next] = (t < TEMP_BLINK_ON_MS);
        }
    }
    /* Master OFF without active preview → all temp LEDs off. */

    /* --- POWER LED ------------------------------------------------------- */
    bool power_on = false;
    if (st.master_enabled) {
        power_on = (cfg.power_led_mode == POWER_LED_ALWAYS_ON)
                   ? true : st.any_heater_active;
    }

    /* --- SHOWER LED ------------------------------------------------------ */
    bool shower_on = st.master_enabled && st.shower_ready;

    /* --- ECO LED --------------------------------------------------------- */
    /* When master is OFF, ECO still shows wifi state IF configured to do so. */
    bool eco_on;
    if (cfg.eco_led_mode == ECO_LED_WIFI_STATE) {
        eco_on = st.wifi_connected;   /* visible even when master OFF */
    } else if (st.master_enabled) {
        eco_on = st.one_heater_active;
    } else {
        eco_on = false;
    }

    /* --- AP animation overrides ECO ------------------------------------- */
    bool anim_val;
    if (eval_ap_anim(now_us, &anim_val)) {
        eco_on = anim_val;
    }

    /* --- mode-cycle animation overrides top-row (POWER/ECO/SHOWER) ------ */
    if (eval_mode_anim(now_us, &anim_val)) {
        power_on  = anim_val;
        eco_on    = anim_val;
        shower_on = anim_val;
    }

    xSemaphoreGive(s_lock);

    /* --- commit GPIO ----------------------------------------------------- */
    for (size_t i = 0; i < 5; ++i) set(s_temp_leds[i], temp_on[i]);
    set(GPIO_POWER, power_on);
    set(GPIO_ECO, eco_on);
    set(GPIO_SHOWER, shower_on);
}

static void render_task(void *arg)
{
    (void)arg;
    for (;;) {
        render_once();
        vTaskDelay(pdMS_TO_TICKS(RENDER_INTERVAL_MS));
    }
}

esp_err_t leds_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    const gpio_num_t pins[] = {
        GPIO_POWER, GPIO_TEMP_40, GPIO_TEMP_50, GPIO_TEMP_60, GPIO_TEMP_70,
        GPIO_TEMP_80, GPIO_SHOWER, GPIO_ECO,
    };
    uint64_t mask = 0;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        mask |= 1ULL << pins[i];
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) gpio_set_level(pins[i], 0);

    if (xTaskCreate(render_task, "leds", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
