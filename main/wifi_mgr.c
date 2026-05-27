#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "time.h"

#include "app_config.h"
#include "event_log.h"

static const char *TAG = "wifi";

/* STA reconnect backoff. Doubles per failure, capped at MAX_MS, reset to
 * MIN_MS on every successful GOT_IP. Without this, a wrong-password STA
 * sits in a tight reconnect loop that starves the heater control task on
 * the same core. */
#define RECONNECT_DELAY_MIN_MS   1000u
#define RECONNECT_DELAY_MAX_MS  30000u

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;

static volatile wifi_state_t s_state = WIFI_STATE_OFF;
static char    s_current_ssid[33];
static char    s_active_ap_ssid[33];   /* cached at AP/APSTA bring-up; "" when no AP up */
static int8_t  s_rssi;
static bool    s_started;
static bool    s_sntp_started;
static volatile bool s_time_synced;

/* When true, suppress auto-reconnect inside the disconnect event handler —
 * a higher-level path (force_ap, restart) is intentionally tearing things
 * down and will bring the radio back up itself. */
static volatile bool s_intentional_stop;

/* True when the active role keeps the SoftAP up (AP or HYBRID/APSTA). Lets
 * wifi_mgr_is_up() report true even before STA gets an IP in hybrid mode. */
static volatile bool s_ap_active;

static esp_timer_handle_t s_reconnect_timer;
static uint32_t           s_reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;

static wifi_mgr_disruption_cb_t s_disruption_cb;

void wifi_mgr_set_disruption_cb(wifi_mgr_disruption_cb_t cb)
{
    s_disruption_cb = cb;
}

static void fire_disruption(void)
{
    if (s_disruption_cb) s_disruption_cb();
}

/* --- STA credential test state ------------------------------------------- */
#define TEST_BIT_OK    BIT0
#define TEST_BIT_FAIL  BIT1

static SemaphoreHandle_t s_test_lock;
static EventGroupHandle_t s_test_events;
static volatile bool     s_testing;
static volatile uint8_t  s_test_disconnect_reason;

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    time_t now = time(NULL);
    struct tm tm; gmtime_r(&now, &tm);
    ESP_LOGI("sntp", "time synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    event_log_emit(EV_TIME_SYNCED, 0, 0, NULL);
}

static void start_sntp_once(void)
{
    if (s_sntp_started) return;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = on_sntp_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) s_sntp_started = true;
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_intentional_stop) return;
    ESP_LOGI(TAG, "STA reconnect (next backoff %lu ms)",
             (unsigned long)s_reconnect_delay_ms);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer) {
        const esp_timer_create_args_t args = {
            .callback = reconnect_timer_cb,
            .name     = "wifi-reconnect",
        };
        if (esp_timer_create(&args, &s_reconnect_timer) != ESP_OK) {
            ESP_LOGE(TAG, "reconnect timer create failed; falling back to immediate reconnect");
            esp_wifi_connect();
            return;
        }
    }
    esp_timer_stop(s_reconnect_timer);  /* cancel any prior pending fire */
    esp_timer_start_once(s_reconnect_timer, (uint64_t)s_reconnect_delay_ms * 1000ULL);
    /* Ramp the next delay BEFORE firing — gives 1s, 2s, 4s, 8s, 16s, 30s, 30s… */
    uint32_t next = s_reconnect_delay_ms * 2u;
    if (next > RECONNECT_DELAY_MAX_MS) next = RECONNECT_DELAY_MAX_MS;
    s_reconnect_delay_ms = next;
}

static void reset_reconnect_backoff(void)
{
    s_reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* First connect attempt is immediate; subsequent failures back off. */
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            if (s_testing) {
                /* Test path swallows the event and stops the normal
                 * reconnect / state-change side effects. */
                if (d) s_test_disconnect_reason = d->reason;
                if (s_test_events) xEventGroupSetBits(s_test_events, TEST_BIT_FAIL);
                break;
            }
            ESP_LOGW(TAG, "STA disconnected (reason %u)", d ? d->reason : 0);
            if (s_state == WIFI_STATE_STA_CONNECTED) {
                event_log_emit(EV_WIFI_STA_DISCONNECTED, 0, 0, NULL);
            }
            /* Preserve AP-up status: in hybrid the AP is still serving even
             * while STA is failing. is_up() must keep returning true. */
            s_state = s_ap_active ? WIFI_STATE_AP : WIFI_STATE_STA_FAILED;
            if (!s_intentional_stop) schedule_reconnect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED:
            ESP_LOGI(TAG, "AP client connected");
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            ESP_LOGI(TAG, "AP client disconnected");
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        if (s_testing) {
            /* Test succeeded — let wifi_mgr_test_sta finish up. Don't touch
             * s_state / s_current_ssid / SNTP, those belong to the real
             * configuration that is about to be restored. */
            ESP_LOGI(TAG, "test: STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
            if (s_test_events) xEventGroupSetBits(s_test_events, TEST_BIT_OK);
        } else {
            ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&e->ip_info.ip));
            s_state = WIFI_STATE_STA_CONNECTED;
            reset_reconnect_backoff();
            event_log_emit(EV_WIFI_STA_CONNECTED, 0, 0, s_current_ssid);
            start_sntp_once();
        }
    }
}

esp_err_t wifi_mgr_init(void)
{
    if (s_started) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_ap_netif  = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));

    /* Best-effort mDNS so the device is reachable at carviston.local. */
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set("carviston");
        mdns_instance_name_set("Carviston Water Heater");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    s_started = true;
    return ESP_OK;
}

/* Fill ap_wc from cfg (or MAC-derived default if no SSID set). */
static void fill_ap_config(const app_config_t *cfg, wifi_config_t *ap_wc)
{
    memset(ap_wc, 0, sizeof(*ap_wc));
    char default_ssid[24];
    const char *ssid = cfg->ap_ssid[0] ? cfg->ap_ssid : NULL;
    if (!ssid) {
        app_config_default_ap_ssid(default_ssid, sizeof(default_ssid));
        ssid = default_ssid;
    }
    strlcpy((char *)ap_wc->ap.ssid, ssid, sizeof(ap_wc->ap.ssid));
    ap_wc->ap.ssid_len       = strlen((const char *)ap_wc->ap.ssid);
    ap_wc->ap.channel        = 1;
    ap_wc->ap.max_connection = 4;
    ap_wc->ap.authmode       = WIFI_AUTH_OPEN;
    if (cfg->ap_pass[0] && strlen(cfg->ap_pass) >= 8) {
        strlcpy((char *)ap_wc->ap.password, cfg->ap_pass, sizeof(ap_wc->ap.password));
        ap_wc->ap.authmode = WIFI_AUTH_WPA2_PSK;
    }
}

static esp_err_t configure_ap(const app_config_t *cfg)
{
    wifi_config_t wc;
    fill_ap_config(cfg, &wc);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_LOGI(TAG, "AP up: ssid='%s' %s",
             (const char *)wc.ap.ssid,
             (wc.ap.authmode == WIFI_AUTH_OPEN) ? "(open)" : "(WPA2)");
    strlcpy(s_active_ap_ssid, (const char *)wc.ap.ssid, sizeof(s_active_ap_ssid));
    s_ap_active = true;
    return ESP_OK;
}

static esp_err_t configure_sta(const app_config_t *cfg)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, cfg->sta_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->sta_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    strlcpy(s_current_ssid, cfg->sta_ssid, sizeof(s_current_ssid));
    s_state = WIFI_STATE_STA_CONNECTING;
    s_ap_active = false;
    return ESP_OK;
}

/* APSTA: AP + STA simultaneously. AP is reachable instantly (no blocking
 * wait on STA), STA tries in the background and promotes the state to
 * STA_CONNECTED on got-IP. AP stays up so a user always has a fall-back
 * path even if STA later drops. */
static esp_err_t configure_apsta(const app_config_t *cfg)
{
    wifi_config_t ap_wc;
    fill_ap_config(cfg, &ap_wc);

    wifi_config_t sta_wc = { 0 };
    strlcpy((char *)sta_wc.sta.ssid, cfg->sta_ssid, sizeof(sta_wc.sta.ssid));
    strlcpy((char *)sta_wc.sta.password, cfg->sta_pass, sizeof(sta_wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP,  &ap_wc));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_wc));
    strlcpy(s_current_ssid, cfg->sta_ssid, sizeof(s_current_ssid));
    strlcpy(s_active_ap_ssid, (const char *)ap_wc.ap.ssid, sizeof(s_active_ap_ssid));
    /* AP is the load-bearing reachability path until STA gets an IP; the
     * IP_EVENT handler will lift state to STA_CONNECTED when that happens. */
    s_state     = WIFI_STATE_AP;
    s_ap_active = true;
    ESP_LOGI(TAG, "APSTA up: AP='%s' STA='%s'",
             (const char *)ap_wc.ap.ssid, cfg->sta_ssid);
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    wifi_role_t mode = cfg.wifi_mode;
    if (!cfg.configured) {
        ESP_LOGW(TAG, "first boot — forcing AP mode");
        mode = WIFI_ROLE_AP;
    }

    s_intentional_stop = false;
    reset_reconnect_backoff();

    if (mode == WIFI_ROLE_AP || cfg.sta_ssid[0] == '\0') {
        configure_ap(&cfg);
        ESP_ERROR_CHECK(esp_wifi_start());
        s_state = WIFI_STATE_AP;
        event_log_emit(EV_WIFI_AP, 0, 0, NULL);
        return ESP_OK;
    }

    if (mode == WIFI_ROLE_STA) {
        configure_sta(&cfg);
        ESP_ERROR_CHECK(esp_wifi_start());
        return ESP_OK;
    }

    /* HYBRID — AP + STA in parallel, no blocking on app_main. */
    configure_apsta(&cfg);
    ESP_ERROR_CHECK(esp_wifi_start());
    event_log_emit(EV_WIFI_AP, 0, 0, "hybrid");
    return ESP_OK;
}

esp_err_t wifi_mgr_restart(void)
{
    /* esp_wifi_stop() invalidates every socket on the AP/STA netifs — drop
     * the http server's WS table FIRST so httpd doesn't loop on the dead
     * sockets after the radio is down. */
    fire_disruption();
    s_intentional_stop = true;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    esp_wifi_stop();
    s_ap_active = false;
    s_active_ap_ssid[0] = '\0';
    return wifi_mgr_start();
}

esp_err_t wifi_mgr_force_ap_mode(void)
{
    app_config_t cfg;
    app_config_get(&cfg);
    /* Same teardown as restart — the long-press ECO path stops and
     * re-starts the radio entirely. */
    fire_disruption();
    s_intentional_stop = true;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
    esp_wifi_stop();
    s_ap_active = false;
    s_active_ap_ssid[0] = '\0';
    /* configure_ap honours empty ap_ssid by deriving carviston-XXXXXX and
     * empty ap_pass by going open — exactly the first-boot behaviour. */
    configure_ap(&cfg);
    s_intentional_stop = false;
    esp_err_t err = esp_wifi_start();
    if (err == ESP_OK) {
        s_state = WIFI_STATE_AP;
        event_log_emit(EV_WIFI_AP, 0, 0, "forced (eco long-press)");
    }
    return err;
}

bool wifi_mgr_time_synced(void) { return s_time_synced; }

wifi_state_t wifi_mgr_state(void) { return s_state; }

bool wifi_mgr_is_up(void)
{
    return s_state == WIFI_STATE_STA_CONNECTED || s_ap_active;
}

const char *wifi_mgr_current_ssid(void)
{
    return (s_state == WIFI_STATE_STA_CONNECTED) ? s_current_ssid : "";
}

const char *wifi_mgr_ap_ssid(void)
{
    return s_ap_active ? s_active_ap_ssid : "";
}

int8_t wifi_mgr_rssi(void)
{
    if (s_state != WIFI_STATE_STA_CONNECTED) return 0;
    wifi_ap_record_t r;
    if (esp_wifi_sta_get_ap_info(&r) == ESP_OK) {
        s_rssi = r.rssi;
    }
    return s_rssi;
}

int wifi_mgr_scan(wifi_scan_entry_t *entries, int max_entries)
{
    if (!entries || max_entries <= 0) return 0;

    /* Scanning requires the STA interface. In pure AP mode the STA radio
     * isn't running, so esp_wifi_scan_start returns ESP_FAIL. Temporarily
     * promote to APSTA, scan, then restore to AP so STA-side state doesn't
     * linger. The AP and any connected AP clients are unaffected by the
     * mode transition. */
    wifi_mode_t prev_mode = WIFI_MODE_NULL;
    bool restored_needed  = false;
    esp_wifi_get_mode(&prev_mode);
    if (prev_mode == WIFI_MODE_AP) {
        /* APSTA shares the radio between AP and STA — during the scan the
         * AP will follow the STA channel through every channel it probes,
         * and AP clients drop transiently. Tear the WS table down so
         * httpd doesn't poll the dying sockets. */
        fire_disruption();
        esp_err_t mode_err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (mode_err != ESP_OK) {
            ESP_LOGW(TAG, "scan: APSTA promotion failed: %s", esp_err_to_name(mode_err));
            return 0;
        }
        restored_needed = true;
    }

    wifi_scan_config_t scan_cfg = { 0 };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        if (restored_needed) esp_wifi_set_mode(prev_mode);
        return 0;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    int out = 0;
    if (n > 0) {
        if (n > 32) n = 32;
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        if (recs) {
            esp_wifi_scan_get_ap_records(&n, recs);
            for (uint16_t i = 0; i < n && out < max_entries; ++i) {
                strlcpy(entries[out].ssid, (const char *)recs[i].ssid, sizeof(entries[out].ssid));
                if (entries[out].ssid[0] == '\0') continue;
                entries[out].rssi     = recs[i].rssi;
                entries[out].authmode = recs[i].authmode;
                entries[out].channel  = recs[i].primary;
                out++;
            }
            free(recs);
        }
    }
    if (restored_needed) esp_wifi_set_mode(prev_mode);
    return out;
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:                return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:                return "auth failed (wrong password?)";
    case WIFI_REASON_ASSOC_FAIL:                 return "association failed";
    case WIFI_REASON_BEACON_TIMEOUT:             return "lost AP (signal too weak)";
    case 0:                                      return "no IP within timeout";
    default:                                     return "association refused";
    }
}

esp_err_t wifi_mgr_test_sta(const char *ssid, const char *password,
                            int timeout_s, wifi_test_result_t *out)
{
    if (!ssid || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (timeout_s < 3)  timeout_s = 3;
    if (timeout_s > 30) timeout_s = 30;

    if (!s_test_lock)    s_test_lock    = xSemaphoreCreateMutex();
    if (!s_test_events)  s_test_events  = xEventGroupCreate();
    if (!s_test_lock || !s_test_events) return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_test_lock, 0) != pdTRUE) {
        snprintf(out->error, sizeof(out->error), "another test in progress");
        return ESP_ERR_INVALID_STATE;
    }

    /* Refuse if we're in pure STA — testing would knock the caller off. */
    wifi_mode_t prev_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev_mode);
    if (prev_mode != WIFI_MODE_AP && prev_mode != WIFI_MODE_APSTA) {
        snprintf(out->error, sizeof(out->error),
                 "switch the device to AP mode before testing");
        xSemaphoreGive(s_test_lock);
        return ESP_ERR_INVALID_STATE;
    }

    /* Snapshot the prior STA config so we can put it back. */
    wifi_config_t prev_sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &prev_sta);

    /* Lay in the test creds. */
    wifi_config_t test_sta = { 0 };
    strlcpy((char *)test_sta.sta.ssid, ssid, sizeof(test_sta.sta.ssid));
    if (password) {
        strlcpy((char *)test_sta.sta.password, password, sizeof(test_sta.sta.password));
    }

    /* Promote to APSTA if currently pure AP so STA has an interface.
     * Either way the AP's channel is about to hop while STA scans, so
     * close existing sockets before we start. */
    fire_disruption();
    bool promoted = false;
    if (prev_mode == WIFI_MODE_AP) {
        if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
            snprintf(out->error, sizeof(out->error), "could not enable STA radio");
            xSemaphoreGive(s_test_lock);
            return ESP_FAIL;
        }
        promoted = true;
    }
    esp_wifi_set_config(WIFI_IF_STA, &test_sta);

    /* Prime test state. s_intentional_stop suppresses the reconnect timer
     * during the test window — the event handler steers events to the
     * test event group via s_testing. */
    xEventGroupClearBits(s_test_events, TEST_BIT_OK | TEST_BIT_FAIL);
    s_test_disconnect_reason = 0;
    s_testing                = true;
    bool prev_intentional    = s_intentional_stop;
    s_intentional_stop       = true;

    /* If STA was already running (APSTA case) cancel it first so the test
     * creds take effect on the next connect. */
    esp_wifi_disconnect();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_test_events,
                                           TEST_BIT_OK | TEST_BIT_FAIL,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_s * 1000));
    bool ok = (bits & TEST_BIT_OK) != 0;
    uint8_t reason = s_test_disconnect_reason;

    /* Restore. Disconnect the test attempt first, then put the previous
     * STA config back. Mode goes back last so events generated during the
     * restore are still swallowed by `s_testing`. */
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &prev_sta);
    if (promoted) {
        esp_wifi_set_mode(prev_mode);
        /* Coming back from APSTA→AP: refresh the cached active AP SSID,
         * the value didn't change but s_ap_active is still set. */
    }

    s_testing          = false;
    s_intentional_stop = prev_intentional;
    reset_reconnect_backoff();

    out->ok = ok;
    if (!ok) {
        strlcpy(out->error, disconnect_reason_str(reason), sizeof(out->error));
    }
    xSemaphoreGive(s_test_lock);
    return ESP_OK;
}
