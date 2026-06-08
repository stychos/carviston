#include "energy.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "energy";

#define NVS_PART   "storage"
#define NVS_NS     "energy"
#define NVS_KEY    "store"

#define ENERGY_DAYS      32          /* day-bucket ring; covers the 30-day window + slack */
#define STORE_MAGIC      0x454E4731u /* "ENG1" */
#define PERSIST_EVERY_US (60ULL * 1000000ULL)
#define MAX_DT_S         5.0         /* clamp a tick gap so a stall can't dump huge energy */

/* Not packed: this blob is only ever written and read back by this firmware on
 * the same platform, so natural alignment is fine — and it keeps us clear of
 * -Werror=address-of-packed-member when handing out &slot_wh[i]. */
typedef struct {
    uint32_t magic;
    int32_t  slot_day[ENERGY_DAYS];  /* local-day index per slot; -1 = empty */
    float    slot_wh[ENERGY_DAYS];   /* Wh accumulated for that day */
    double   total_wh;               /* lifetime Wh */
} energy_store_t;

static energy_store_t   s_store;
static SemaphoreHandle_t s_lock;
static bool             s_inited;
static int64_t          s_last_us;        /* esp_timer at previous account() */
static int64_t          s_last_persist_us;
static double           s_pending_wh;      /* energy accrued before the clock synced */
static bool             s_dirty;

/* Days since 1970-01-01 for a civil (y,m,d) date — Howard Hinnant's algorithm.
 * Contiguous across month/year boundaries, so day differences are exact. */
static int32_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int)doe - 719468;
}

/* Local calendar day index, or -1 if the wall clock hasn't synced yet. The
 * configured TZ is already applied (setenv/tzset), so localtime_r() yields the
 * local date and day boundaries fall at local midnight. */
static int32_t local_day(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return -1;
    struct tm lt;
    localtime_r(&now, &lt);
    return days_from_civil(lt.tm_year + 1900, (unsigned)lt.tm_mon + 1, (unsigned)lt.tm_mday);
}

static void persist_locked(void)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY, &s_store, sizeof(s_store));
    nvs_commit(h);
    nvs_close(h);
    s_dirty = false;
}

static void reset_store_locked(void)
{
    memset(&s_store, 0, sizeof(s_store));
    s_store.magic = STORE_MAGIC;
    for (int i = 0; i < ENERGY_DAYS; ++i) s_store.slot_day[i] = -1;
    s_pending_wh = 0.0;
}

/* Find the slot for `day`, allocating (or evicting the oldest) if needed. */
static float *slot_for_day_locked(int32_t day)
{
    int free_idx = -1, oldest_idx = 0;
    for (int i = 0; i < ENERGY_DAYS; ++i) {
        if (s_store.slot_day[i] == day) return &s_store.slot_wh[i];
        if (s_store.slot_day[i] < 0 && free_idx < 0) free_idx = i;
        if (s_store.slot_day[i] < s_store.slot_day[oldest_idx]) oldest_idx = i;
    }
    int idx = (free_idx >= 0) ? free_idx : oldest_idx;
    s_store.slot_day[idx] = day;
    s_store.slot_wh[idx]  = 0.0f;
    return &s_store.slot_wh[idx];
}

esp_err_t energy_init(void)
{
    if (s_inited) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_store_locked();
    nvs_handle_t h;
    if (nvs_open_from_partition(NVS_PART, NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        energy_store_t tmp;
        size_t sz = sizeof(tmp);
        if (nvs_get_blob(h, NVS_KEY, &tmp, &sz) == ESP_OK &&
            sz == sizeof(tmp) && tmp.magic == STORE_MAGIC) {
            s_store = tmp;
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);

    s_inited = true;
    return ESP_OK;
}

void energy_account(const bool relay_on[2], const uint16_t watts[2])
{
    if (!s_inited) return;

    int64_t now_us = esp_timer_get_time();
    if (s_last_us == 0) { s_last_us = now_us; return; }  /* establish baseline */
    double dt_s = (double)(now_us - s_last_us) / 1000000.0;
    s_last_us = now_us;
    if (dt_s <= 0.0) return;
    if (dt_s > MAX_DT_S) dt_s = MAX_DT_S;

    double wh = 0.0;
    for (int i = 0; i < 2; ++i) {
        if (relay_on[i]) wh += (double)watts[i] * dt_s / 3600.0;
    }
    if (wh <= 0.0) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_store.total_wh += wh;
    int32_t day = local_day();
    if (day < 0) {
        s_pending_wh += wh;     /* no clock yet — hold for the first known day */
    } else {
        float *slot = slot_for_day_locked(day);
        *slot += (float)(wh + s_pending_wh);
        s_pending_wh = 0.0;
    }
    s_dirty = true;
    if (now_us - s_last_persist_us > (int64_t)PERSIST_EVERY_US) {
        persist_locked();
        s_last_persist_us = now_us;
    }
    xSemaphoreGive(s_lock);
}

void energy_get(energy_totals_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!s_inited) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->total_wh = s_store.total_wh;
    int32_t day = local_day();
    if (day >= 0) {
        for (int i = 0; i < ENERGY_DAYS; ++i) {
            int32_t d = s_store.slot_day[i];
            if (d < 0 || d > day) continue;
            int32_t age = day - d;
            double wh = s_store.slot_wh[i];
            if (age == 0)  out->today_wh += wh;
            if (age <= 6)  out->week_wh  += wh;
            if (age <= 29) out->month_wh += wh;
        }
    }
    xSemaphoreGive(s_lock);
}

void energy_flush(void)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dirty) persist_locked();
    xSemaphoreGive(s_lock);
}

void energy_clear(void)
{
    if (!s_inited) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reset_store_locked();
    persist_locked();
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "energy counters cleared");
}
