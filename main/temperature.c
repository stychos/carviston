/* temperature — ADS1115 over I2C + NTC math.
 *
 * The four thermistors feed AIN0..AIN3 on an ADS1115 at address 0x48 on the
 * I2C bus (SDA=GPIO6, SCL=GPIO7):
 *   AIN0 = inlet  tank regulation NTC   AIN1 = inlet  tank safety NTC
 *   AIN2 = outlet tank regulation NTC   AIN3 = outlet tank safety NTC
 * The two NTCs of one tank are redundant (same water); the two tanks are
 * independent and read different temperatures during a draw.
 *
 * Why ADS1115 instead of the ESP32-S3 internal ADC:
 *   - 16-bit Σ-Δ vs ~11-bit effective on the internal SAR
 *   - Galvanically separated from the WiFi RF noise that bleeds into ADC1
 *   - Frees GPIO1/2/4/5 for future use
 *
 * Wiring per channel: 3V3 → 10 kΩ (1 %) → AINx → NTC → GND.  The PGA is set
 * to gain=1 (±4.096 V full scale, LSB = 125 µV) so the 0–3.3 V divider output
 * lives comfortably inside the input range without saturating.
 *
 * Failure mode: any I²C error or out-of-range raw reading collapses to NaN,
 * which propagates through mv_to_celsius() and is caught by safety.c as a
 * probe fault — the heaters are dropped.
 */

#include "temperature.h"

#include <math.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "temperature";

/* --- Hardware constants ------------------------------------------------- */

#define R_TOP_OHM            10000.0f
#define VREF_MV              3300.0f      /* MCU 3V3 rail powering the divider tops */

#define I2C_PORT             I2C_NUM_0
#define I2C_SDA_GPIO         6
#define I2C_SCL_GPIO         7
#define I2C_FREQ_HZ          100000

#define ADS1115_ADDR         0x48         /* ADDR pin → GND */
#define ADS1115_REG_CONV     0x00
#define ADS1115_REG_CONFIG   0x01

/* Config-register bits (see ADS1115 datasheet table 8-3). */
#define ADS_CFG_OS_START     (1u << 15)
#define ADS_CFG_MUX_AIN0     (0b100u << 12)
#define ADS_CFG_MUX_AIN1     (0b101u << 12)
#define ADS_CFG_MUX_AIN2     (0b110u << 12)
#define ADS_CFG_MUX_AIN3     (0b111u << 12)
#define ADS_CFG_PGA_4_096V   (0b001u << 9)   /* gain = 1, LSB = 125 µV */
#define ADS_CFG_MODE_SINGLE  (1u << 8)
#define ADS_CFG_DR_128SPS    (0b100u << 5)   /* 128 SPS — ~7.8 ms per conversion */
#define ADS_CFG_COMP_DISABLE (0b11u)
#define ADS_LSB_UV           125            /* µV per LSB at PGA=1 */
/* Upper bound on how long we'll wait for a single conversion to finish before
 * giving up (→ -1 → NaN → loud fault). A 128-SPS conversion is ~7.8 ms; this
 * leaves margin for the FreeRTOS-tick-granularity poll below. An absent/wedged
 * ADS1115 fails fast at this bound instead of stalling the 1 Hz heater tick.
 * 30 ms ≈ 3 FreeRTOS ticks at 100 Hz: a healthy 128-SPS conversion (~7.8 ms)
 * completes within ~2 ticks, so this never false-times-out a working sensor. */
#define ADS_CONV_TIMEOUT_MS  30

/* Per-I2C-transaction timeout. Each transfer moves only 2-3 bytes at 100 kHz
 * (~0.3 ms nominal), so 20 ms is enormous headroom for a healthy bus while
 * still bounding a clock-stretched/wedged one — the IDF default (100 ms) could
 * let a wedged-but-ACKing ADS1115 stretch a full 16-read temperature_read()
 * past the 1 Hz control tick. */
#define ADS_I2C_TIMEOUT_MS   20

/* Samples averaged per channel. An odd count gives a clean median, and 5
 * samples (each ~10-20 ms with the conversion-ready poll) span ~100 ms ≈ 5-6
 * mains cycles, so the trimmed mean below settles to the DC level even with
 * 50/60 Hz pickup on the NTC leads. */
#define SAMPLES_PER_CHANNEL  5

/* Bounds outside which we declare the sensor open/shorted. With the divider
 * we use, 25 °C reads ~1650 mV and 80 °C reads ~600 mV. Anything below 50 mV
 * means the NTC is shorted to GND (or wire broken pulling low); above 3200 mV
 * means the NTC is open-circuit (or wire broken pulling high). */
#define ADC_MIN_MV           50
#define ADC_MAX_MV           3200

/* --- State -------------------------------------------------------------- */

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static const uint16_t s_mux_for_channel[4] = {
    ADS_CFG_MUX_AIN0, ADS_CFG_MUX_AIN1, ADS_CFG_MUX_AIN2, ADS_CFG_MUX_AIN3,
};

/* --- ADS1115 driver (kept inline — single consumer) -------------------- */

static esp_err_t ads_write_config(uint16_t cfg)
{
    uint8_t buf[3] = {
        ADS1115_REG_CONFIG,
        (uint8_t)(cfg >> 8),
        (uint8_t)(cfg & 0xff),
    };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), ADS_I2C_TIMEOUT_MS);
}

static esp_err_t ads_read_conversion(int16_t *out_raw)
{
    uint8_t reg = ADS1115_REG_CONV;
    uint8_t r[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, r, 2, ADS_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *out_raw = (int16_t)(((uint16_t)r[0] << 8) | r[1]);
    return ESP_OK;
}

static esp_err_t ads_read_config(uint16_t *out_cfg)
{
    uint8_t reg = ADS1115_REG_CONFIG;
    uint8_t r[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, r, 2, ADS_I2C_TIMEOUT_MS);
    if (err != ESP_OK) return err;
    *out_cfg = ((uint16_t)r[0] << 8) | r[1];
    return ESP_OK;
}

/* One single-ended conversion. Returns voltage in mV, or -1 on I/O fault.
 *
 * The conversion-ready wait is the load-bearing part. A fixed
 * vTaskDelay(pdMS_TO_TICKS(10)) is NOT safe here: at CONFIG_FREERTOS_HZ=100
 * that rounds to vTaskDelay(1), which unblocks at the NEXT tick edge — often
 * well under the ~7.8 ms a 128-SPS conversion needs. Reading the conversion
 * register early returns the PREVIOUS channel's result, so a value from one
 * tank leaks into another tank's NTC slot and fabricates a within-tank
 * disagreement (the bigger the real inter-tank gap, the more it overshoots
 * probe_disagree_c). Instead we poll the ADS1115 OS bit (config register
 * bit 15: 1 = idle/done, 0 = converting) until the conversion we started has
 * actually finished, bounded by ADS_CONV_TIMEOUT_MS. */
static int ads_read_channel_mv(int channel)
{
    uint16_t cfg = ADS_CFG_OS_START
                 | s_mux_for_channel[channel]
                 | ADS_CFG_PGA_4_096V
                 | ADS_CFG_MODE_SINGLE
                 | ADS_CFG_DR_128SPS
                 | ADS_CFG_COMP_DISABLE;
    if (ads_write_config(cfg) != ESP_OK) return -1;

    int64_t deadline_us = esp_timer_get_time() + ADS_CONV_TIMEOUT_MS * 1000;
    for (;;) {
        /* Yield first: guarantees the conversion is underway (so the OS bit
         * reflects THIS conversion, not the pre-write idle state) and gives it
         * time to complete before we poll. */
        vTaskDelay(1);
        uint16_t st;
        if (ads_read_config(&st) != ESP_OK) return -1;
        if (st & ADS_CFG_OS_START) break;             /* bit 15 set → done */
        if (esp_timer_get_time() >= deadline_us) return -1;
    }

    int16_t raw;
    if (ads_read_conversion(&raw) != ESP_OK) return -1;
    if (raw < 0) raw = 0;  /* single-ended; small negatives are noise */

    /* mv = raw * LSB(µV) / 1000 */
    return ((int)raw * ADS_LSB_UV) / 1000;
}

/* Sample a channel several times and collapse to one robust value.
 *
 * A plain mean is the wrong estimator here: when an element energises (e.g.
 * right after a mode switch), the mains switching couples a noise spike onto
 * the NTC leads, and a single bad sample smears the mean enough to push a
 * tank's regulation vs safety apart by > probe_disagree_c — a FALSE within-tank
 * disagreement that latches as a probe fault. So we drop the min and max
 * (rejecting one positive and one negative outlier) and average the rest; with
 * too few valid samples to trim we fall back to the median, then the lone
 * value. Returns mV, or -1 if every sample was an I/O fault. */
static int sample_channel_mv(int channel)
{
    int v[SAMPLES_PER_CHANNEL];
    int n = 0;
    for (int i = 0; i < SAMPLES_PER_CHANNEL; ++i) {
        int mv = ads_read_channel_mv(channel);
        if (mv >= 0) v[n++] = mv;
    }
    if (n == 0) return -1;

    /* Insertion sort — n is tiny (<= SAMPLES_PER_CHANNEL). */
    for (int i = 1; i < n; ++i) {
        int x = v[i], j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }

    if (n >= 3) {                       /* trimmed mean: drop min and max */
        int sum = 0;
        for (int i = 1; i < n - 1; ++i) sum += v[i];
        return sum / (n - 2);
    }
    return v[n / 2];                    /* 1-2 samples: median / lone value */
}

/* --- NTC math (unchanged) ---------------------------------------------- */

static float mv_to_celsius(int mv, uint32_t r25, uint16_t beta)
{
    if (mv < ADC_MIN_MV || mv > ADC_MAX_MV) return NAN;

    float v = (float)mv;
    float r_ntc = R_TOP_OHM * v / (VREF_MV - v);
    if (r_ntc <= 0.0f) return NAN;

    const float T0 = 298.15f;
    float ln_ratio = logf(r_ntc / (float)r25);
    float inv_T = 1.0f / T0 + ln_ratio / (float)beta;
    return 1.0f / inv_T - 273.15f;
}

/* --- Public API -------------------------------------------------------- */

esp_err_t temperature_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port   = I2C_PORT,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .glitch_ignore_cnt = 7,
        /* Internal pull-ups are weak (~45 kΩ). External 4.7 kΩ pull-ups to
         * 3V3 on SDA and SCL are REQUIRED for reliable operation. */
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ADS1115_ADDR,
        .scl_speed_hz    = I2C_FREQ_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ads1115 add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Probe: write a known config and verify the device ACKs. If the user
     * forgot the pull-ups or wired the bus wrong, we'd rather fail loudly at
     * init than emit NaN forever and have safety latch every boot. */
    if (ads_write_config(ADS_CFG_OS_START | ADS_CFG_MUX_AIN0
                       | ADS_CFG_PGA_4_096V | ADS_CFG_MODE_SINGLE
                       | ADS_CFG_DR_128SPS | ADS_CFG_COMP_DISABLE) != ESP_OK) {
        ESP_LOGE(TAG, "ADS1115 not responding at 0x%02x — check wiring + pull-ups",
                 ADS1115_ADDR);
        /* Non-fatal: leave handles in place so retries during reads can recover. */
    } else {
        ESP_LOGI(TAG, "ADS1115 @0x%02x ready on SDA=%d SCL=%d",
                 ADS1115_ADDR, I2C_SDA_GPIO, I2C_SCL_GPIO);
    }
    return ESP_OK;
}

esp_err_t temperature_read(temperature_reading_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    app_config_t cfg;
    app_config_get(&cfg);

    int mv[4];
    /* Channel order: AIN0=inlet reg, AIN1=inlet safety, AIN2=outlet reg,
     * AIN3=outlet safety. */
    for (int ch = 0; ch < 4; ++ch) mv[ch] = sample_channel_mv(ch);

    float t[4];
    for (int i = 0; i < 4; ++i) {
        t[i] = (mv[i] < 0) ? NAN
                           : mv_to_celsius(mv[i], cfg.ntc_r25_ohm, cfg.ntc_beta);
    }

    memset(out, 0, sizeof(*out));
    out->tank[TANK_INLET].regulation_c  = t[0];
    out->tank[TANK_INLET].safety_c      = t[1];
    out->tank[TANK_OUTLET].regulation_c = t[2];
    out->tank[TANK_OUTLET].safety_c     = t[3];

    /* Per-tank fault, two independent causes (tanks are NOT cross-checked):
     *   - sensor bad : the two redundant NTCs disagree by more than THIS tank's
     *                  window (inlet tolerates more — it stratifies on a draw),
     *                  or one reads open/short (NaN).
     *   - over-temp  : either NTC at/above the soft limit. The reading is
     *                  trusted but the tank is too hot, so drop its element and
     *                  let it auto-recover once it cools — the other tank keeps
     *                  heating. Excluded from max_c so this stays a per-tank
     *                  drop, not the global all-off that safety.c watches for. */
    int healthy = 0;
    float sum_c = 0.0f;
    float max_c = NAN;
    for (int k = 0; k < TANK_COUNT; ++k) {
        float r = out->tank[k].regulation_c;
        float s = out->tank[k].safety_c;
        bool sensor_bad = isnan(r) || isnan(s) ||
                          fabsf(r - s) > (float)cfg.probe_disagree_c[k];
        bool overtemp = (!isnan(r) && r >= TEMP_OVERTEMP_LIMIT_C) ||
                        (!isnan(s) && s >= TEMP_OVERTEMP_LIMIT_C);
        out->tank[k].fault = sensor_bad || overtemp;
        if (!out->tank[k].fault) {
            healthy++;
            sum_c += r;
            max_c = isnan(max_c) ? r : fmaxf(max_c, r);
        }
    }

    /* water_c = MEAN of the healthy tanks: the whole-heater temperature shown
     * on the gauge and the LED bar. outlet_c is the outlet tank alone,
     * used only for shower-ready. max_c (hottest tank) drives over-temp. */
    out->water_c   = healthy ? (sum_c / (float)healthy) : NAN;
    out->outlet_c  = out->tank[TANK_OUTLET].fault ? NAN
                                                  : out->tank[TANK_OUTLET].regulation_c;
    out->max_c     = max_c;
    out->any_fault = (healthy < TANK_COUNT);
    out->all_fault = (healthy == 0);

    return ESP_OK;
}
