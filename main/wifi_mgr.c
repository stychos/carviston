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
#include <sys/time.h>

#include "app_config.h"
#include "event_log.h"

static const char *TAG = "wifi";

/* STA reconnect backoff. Doubles per failure, capped at MAX_MS, reset to
 * MIN_MS on every successful GOT_IP. Without this, a wrong-password STA
 * sits in a tight reconnect loop that starves the heater control task on
 * the same core. */
#define RECONNECT_DELAY_MIN_MS   1000u
#define RECONNECT_DELAY_MAX_MS  30000u

/* Pure-STA → APSTA fallback. If STA can't get an IP within
 * app_config.sta_fallback_seconds we promote to APSTA so the user has a
 * way to reach the device. Both the toggle and the timeout are user-
 * configurable from the Wi-Fi tab. */

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;

static volatile wifi_state_t s_state = WIFI_STATE_OFF;
static char    s_current_ssid[33];
static char    s_active_ap_ssid[33];   /* cached at AP/APSTA bring-up; "" when no AP up */
static int8_t  s_rssi;
static bool    s_started;
static bool    s_sntp_started;
static volatile bool s_time_synced;
static volatile bool s_time_manual;   /* clock seeded by the user, not SNTP */

/* When true, suppress auto-reconnect inside the disconnect event handler —
 * a higher-level path (force_ap, restart) is intentionally tearing things
 * down and will bring the radio back up itself. */
static volatile bool s_intentional_stop;

/* True when a SoftAP is up — either pure AP mode, or the recovery AP that the
 * STA-fallback path promotes to APSTA. Lets wifi_mgr_is_up() report true even
 * while STA has no IP. */
static volatile bool s_ap_active;

static esp_timer_handle_t s_reconnect_timer;
static uint32_t           s_reconnect_delay_ms = RECONNECT_DELAY_MIN_MS;
/* Set when reconnect_timer_cb has dropped the WS sockets and re-armed itself
 * for a short drain window; the next fire performs the actual esp_wifi_connect()
 * without ever blocking the shared esp_timer task. */
static volatile bool      s_reconnect_drain_pending;

/* Microsecond timestamp of the current STA attempt cycle. Set when STA
 * is started without a working connection, cleared on GOT_IP and when
 * the radio is reconfigured. 0 = no active attempt. */
static int64_t  s_sta_attempt_start_us;
static bool     s_sta_fallback_armed;     /* fallback only fires once per attempt cycle */

static wifi_mgr_disruption_cb_t s_disruption_cb;

void wifi_mgr_set_disruption_cb(wifi_mgr_disruption_cb_t cb)
{
    s_disruption_cb = cb;
}

/* Tear-down delay after running the disruption callback.
 *
 * httpd_sess_trigger_close() (used inside web_server_close_all_ws) is
 * ASYNCHRONOUS — it marks each fd for closure and pokes the httpd worker
 * via its control socket, but the actual close() runs on httpd's task on
 * the next select() iteration. If the caller barrels straight on into
 * esp_wifi_stop() / esp_wifi_set_mode() / esp_wifi_connect() before that
 * iteration runs, the netif (or the AP's channel) changes under fds
 * httpd still thinks are live — which manifests as the recv-128 /
 * "WS frame is not properly masked" storm we keep hitting.
 *
 * 250 ms is more than enough: httpd's control-socket wake unblocks the
 * select() within a couple of ms, and the close path is cheap. The
 * delay is yielded with vTaskDelay so other tasks (especially httpd's
 * own worker) get the CPU. */
#define DISRUPTION_DRAIN_MS  250

static void fire_disruption(void)
{
    if (!s_disruption_cb) return;
    s_disruption_cb();
    vTaskDelay(pdMS_TO_TICKS(DISRUPTION_DRAIN_MS));
}

/* Forward decls — promote_sta_to_apsta_fallback uses fill_ap_config, and
 * the reconnect timer calls the promotion helper. */
static void fill_ap_config(const app_config_t *cfg, wifi_config_t *ap_wc);
static void promote_sta_to_apsta_fallback(void);

/* --- STA credential test state ------------------------------------------- */
#define TEST_BIT_OK    BIT0
#define TEST_BIT_FAIL  BIT1

static SemaphoreHandle_t s_test_lock;
static EventGroupHandle_t s_test_events;
static volatile bool     s_testing;
static volatile uint8_t  s_test_disconnect_reason;

/* Async-test plumbing (pure-STA path): params captured for the worker, and the
 * pollable result, guarded by s_async_lock. */
static SemaphoreHandle_t s_async_lock;
static wifi_test_async_t s_async;
static char              s_async_ssid[33];
static char              s_async_pass[65];
static int               s_async_timeout;

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    /* A real sync supersedes any manual seed; restore the configured TZ in case
     * a prior manual set forced UTC0 this boot (recovery-AP → reconnect). */
    s_time_manual = false;
    app_config_apply_timezone();
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
    if (s_intentional_stop) { s_reconnect_drain_pending = false; return; }

    /* Second stage of an APSTA reconnect: WS sockets were dropped on the
     * previous fire and the drain window has now elapsed, so it's safe to
     * hop the channel. */
    if (s_reconnect_drain_pending) {
        s_reconnect_drain_pending = false;
        esp_wifi_connect();
        return;
    }

    wifi_mode_t m = WIFI_MODE_NULL;
    esp_wifi_get_mode(&m);

    /* Recovery-AP fallback: if pure STA has been failing for longer than
     * the user-configured window without ever getting an IP, bring the AP
     * up alongside it. Only fires once per attempt cycle. */
    if (m == WIFI_MODE_STA && s_sta_fallback_armed && s_sta_attempt_start_us > 0) {
        app_config_t cfg;
        app_config_get(&cfg);
        if (cfg.sta_fallback_enabled && cfg.sta_fallback_seconds > 0) {
            int64_t elapsed_s = (esp_timer_get_time() - s_sta_attempt_start_us) / 1000000LL;
            if (elapsed_s >= cfg.sta_fallback_seconds) {
                promote_sta_to_apsta_fallback();
                s_sta_fallback_armed = false;
                /* Mode is now APSTA — the next esp_wifi_connect() below
                 * uses the same STA creds, scanning may still hop the
                 * channel, so fall through to the APSTA disruption guard. */
                m = WIFI_MODE_APSTA;
            }
        }
    }

    ESP_LOGI(TAG, "STA reconnect (next backoff %lu ms)",
             (unsigned long)s_reconnect_delay_ms);
    /* APSTA shares one radio: the upcoming esp_wifi_connect() may scan
     * channels to find the AP, dragging our SoftAP along and breaking
     * existing TCP sockets on it. Drop tracked WS sessions BEFORE the
     * hop so httpd doesn't sit polling dead sockets. (Browsers reopen
     * automatically once the AP stabilises.) Pure STA mode has no AP
     * clients to worry about. */
    if (m == WIFI_MODE_APSTA && s_disruption_cb) {
        /* Drop the WS sessions now, then defer the actual connect by a short
         * drain window — realised as a second timer fire, NOT a vTaskDelay,
         * because this callback runs in the shared esp_timer task and must
         * never block it (that would stall every other esp_timer callback). */
        s_disruption_cb();
        s_reconnect_drain_pending = true;
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)DISRUPTION_DRAIN_MS * 1000ULL);
        return;
    }
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
    s_reconnect_drain_pending = false;
    if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
}

/* Promote a stuck pure-STA configuration to APSTA: keep the configured
 * STA creds (so the retry loop continues to try the real network) but
 * also raise the recovery SoftAP. Used by the reconnect timer when the
 * STA attempt has been failing past STA_FALLBACK_TIMEOUT_S. */
static void promote_sta_to_apsta_fallback(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    wifi_config_t ap_wc;
    fill_ap_config(&cfg, &ap_wc);

    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK) {
        ESP_LOGW(TAG, "STA fallback: mode promotion failed");
        return;
    }
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_wc) != ESP_OK) {
        ESP_LOGW(TAG, "STA fallback: AP config failed");
        return;
    }

    strlcpy(s_active_ap_ssid, (const char *)ap_wc.ap.ssid, sizeof(s_active_ap_ssid));
    s_ap_active = true;
    /* is_up() will now return true via the AP, even while STA keeps
     * retrying. wifi_mgr_current_ssid() still returns "" until STA gets
     * an IP, so the UI shows "AP · <ssid>" with a good (green) signal. */
    ESP_LOGW(TAG, "STA unreachable — recovery AP '%s' is up",
             (const char *)ap_wc.ap.ssid);
    event_log_emit(EV_WIFI_AP, 0, 0, "sta fallback");
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* First connect attempt is immediate; subsequent failures back off. */
            s_sta_attempt_start_us = esp_timer_get_time();
            s_sta_fallback_armed   = true;
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
                /* Re-arm the fallback timer for the post-connect outage
                 * case (router rebooted, walked out of range, etc.) so
                 * extended downtime brings the recovery AP up. */
                s_sta_attempt_start_us = esp_timer_get_time();
                s_sta_fallback_armed   = true;
            }
            /* Preserve AP-up status: once the recovery AP is up it keeps
             * serving even while STA is failing. is_up() must stay true. */
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
            /* Cycle resolved successfully — disarm the fallback so the next
             * disconnect doesn't immediately trip it. */
            s_sta_attempt_start_us = 0;
            s_sta_fallback_armed   = false;
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

    /* Best-effort mDNS so the device is reachable at <hostname>.local. The
     * hostname is configurable (defaults to "carviston"); the netif/DHCP name
     * the router shows is set to the same value in apply_hostname(). */
    if (mdns_init() == ESP_OK) {
        char hn[APP_CFG_HOSTNAME_MAX];
        app_config_get_hostname(hn, sizeof hn);
        mdns_hostname_set(hn);
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

/* Range-over-throughput RF profile for a mains-powered node behind an internal
 * antenna — trade power budget and bandwidth for link robustness at the edge:
 *   - PS_NONE: never park the modem between beacons. The STA default
 *     WIFI_PS_MIN_MODEM wakes late and misses beacons on a marginal link, which
 *     is what drops the association at range. ~+40 mA — nothing on a heater.
 *   - max TX power pinned to the regulatory ceiling (80 = 20 dBm; the country
 *     limit still clamps it, so this only ever raises, never overrides law).
 *   - HT20: ~2-3 dB better RX sensitivity than HT40 and less interference-prone;
 *     we have zero use for 40 MHz throughput here.
 * Best-effort: these only refine an already-working radio, so a failure logs
 * and continues rather than aborting startup. Must run AFTER esp_wifi_start() —
 * TX power and bandwidth aren't settable while the radio is down. */
static void apply_rf_profile(wifi_interface_t iface)
{
    esp_err_t err;
    if ((err = esp_wifi_set_ps(WIFI_PS_NONE)) != ESP_OK)
        ESP_LOGW(TAG, "set_ps(NONE): %s", esp_err_to_name(err));
    if ((err = esp_wifi_set_max_tx_power(80)) != ESP_OK)
        ESP_LOGW(TAG, "set_max_tx_power: %s", esp_err_to_name(err));
    if ((err = esp_wifi_set_bandwidth(iface, WIFI_BW20)) != ESP_OK)
        ESP_LOGW(TAG, "set_bandwidth(HT20): %s", esp_err_to_name(err));
}

/* Apply the configured network name to the active interface (the DHCP hostname
 * the router displays) and to mDNS. esp_netif_set_hostname needs the netif
 * STARTED, so this runs after esp_wifi_start(); a change takes effect on the
 * next association, which is why a hostname edit triggers wifi_mgr_restart(). */
static void apply_hostname(esp_netif_t *netif)
{
    char hn[APP_CFG_HOSTNAME_MAX];
    app_config_get_hostname(hn, sizeof hn);
    if (netif) {
        esp_err_t e = esp_netif_set_hostname(netif, hn);
        if (e != ESP_OK) ESP_LOGW(TAG, "set_hostname: %s", esp_err_to_name(e));
    }
    mdns_hostname_set(hn);
}

/* Apply the configured regulatory domain with 802.11d OFF. The chosen country's
 * table sets the legal channel set and TX-power ceiling that clamps
 * apply_rf_profile()'s set_max_tx_power — e.g. "US" covers ch 1-11 at the full
 * chip ceiling (no world-domain power clamp, so TX actually reaches 20 dBm) but
 * omits ch 12/13; "01" is the conservative world domain. 802.11d disabled means
 * a neighbour AP's beacon country IE can't drift us onto a different table.
 * Re-applied on every start() so a region change in the UI takes effect on the
 * radio restart. Must run after esp_wifi_init(), before esp_wifi_start(). */
static void apply_country_code(const app_config_t *cfg)
{
    const char *cc = (cfg->wifi_country[0]) ? cfg->wifi_country : "US";
    esp_err_t err = esp_wifi_set_country_code(cc, false);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "set_country_code(%s) failed: %s", cc, esp_err_to_name(err));
    else
        ESP_LOGI(TAG, "regulatory domain: %s", cc);
}

esp_err_t wifi_mgr_start(void)
{
    app_config_t cfg;
    app_config_get(&cfg);

    apply_country_code(&cfg);

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
        apply_rf_profile(WIFI_IF_AP);
        apply_hostname(s_ap_netif);
        s_state = WIFI_STATE_AP;
        event_log_emit(EV_WIFI_AP, 0, 0, NULL);
        return ESP_OK;
    }

    /* STA — join the configured network. If it can't get an IP within
     * sta_fallback_seconds, the event handler promotes to APSTA and brings
     * up a recovery AP (promote_sta_to_apsta_fallback). */
    configure_sta(&cfg);
    ESP_ERROR_CHECK(esp_wifi_start());
    apply_rf_profile(WIFI_IF_STA);
    apply_hostname(s_sta_netif);
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

bool wifi_mgr_time_manual(void) { return s_time_manual; }

void wifi_mgr_set_manual_time(time_t local_epoch)
{
    /* The user enters their LOCAL wall-clock. Run the C library in UTC0 so
     * localtime_r() returns exactly those components — the scheduler reasons in
     * local wall-clock, and with UTC0 that IS the entered time. No DST math: a
     * hand-set clock is the user's responsibility, and it's transient anyway. */
    setenv("TZ", "UTC0", 1);
    tzset();
    struct timeval tv = { .tv_sec = local_epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_time_manual = true;
    s_time_synced = true;

    struct tm lt; localtime_r(&local_epoch, &lt);
    ESP_LOGI(TAG, "manual time set: %04d-%02d-%02d %02d:%02d:%02d (UTC0)",
             lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
             lt.tm_hour, lt.tm_min, lt.tm_sec);
    event_log_emit(EV_TIME_SYNCED, 0, 0, NULL);
}

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
        /* Mode-restore may hop the AP channel back — close any WS clients
         * that reconnected during our short APSTA promotion. */
        if (restored_needed) {
            fire_disruption();
            esp_wifi_set_mode(prev_mode);
        }
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
    if (restored_needed) {
        /* Same reason as the early-return path above. */
        fire_disruption();
        esp_wifi_set_mode(prev_mode);
    }
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

    /* Any started mode is testable now. Pure STA drops the live link for the
     * test window and rejoins the remembered network afterwards (see the rejoin
     * at the end) — callers reached over that link must use the async path. */
    wifi_mode_t prev_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&prev_mode);
    if (prev_mode == WIFI_MODE_NULL) {
        snprintf(out->error, sizeof(out->error), "wi-fi is not running");
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
        /* Mode flip back to AP-only can hop the channel and break any WS
         * client that re-opened during the 12 s test window. */
        fire_disruption();
        esp_wifi_set_mode(prev_mode);
        /* Coming back from APSTA→AP: refresh the cached active AP SSID,
         * the value didn't change but s_ap_active is still set. */
    }

    s_testing          = false;
    s_intentional_stop = prev_intentional;
    reset_reconnect_backoff();

    /* If we tore down a live STA link to run the test (pure STA, or APSTA with a
     * real saved network), actively rejoin it now. Events flow normally again
     * (s_testing is clear), so the state machine tracks the reconnect and
     * SNTP/mDNS re-arm on GOT_IP. The pure-AP case leaves prev_sta empty. */
    if (!promoted && prev_sta.sta.ssid[0] != '\0') {
        s_state = WIFI_STATE_STA_CONNECTING;
        esp_wifi_connect();
    }

    out->ok = ok;
    if (!ok) {
        strlcpy(out->error, disconnect_reason_str(reason), sizeof(out->error));
    }
    xSemaphoreGive(s_test_lock);
    return ESP_OK;
}

bool wifi_mgr_ap_serving(void) { return s_ap_active; }

/* Background worker for the async (pure-STA) test path. Params were captured
 * under s_async_lock before the task was created. */
static void test_async_task(void *arg)
{
    (void)arg;
    /* Let the HTTP handler flush its "{async:true}" reply before we drop the STA
     * link the request rode in on — otherwise the browser sees a dead socket
     * instead of the acknowledgement and never starts polling. */
    vTaskDelay(pdMS_TO_TICKS(300));

    wifi_test_result_t r;
    esp_err_t err = wifi_mgr_test_sta(s_async_ssid, s_async_pass, s_async_timeout, &r);

    xSemaphoreTake(s_async_lock, portMAX_DELAY);
    s_async.running = false;
    s_async.done    = true;
    s_async.ok      = (err == ESP_OK) && r.ok;
    if (s_async.ok) {
        s_async.error[0] = '\0';
    } else {
        strlcpy(s_async.error, r.error[0] ? r.error : "test failed", sizeof(s_async.error));
    }
    xSemaphoreGive(s_async_lock);
    vTaskDelete(NULL);
}

esp_err_t wifi_mgr_test_sta_async(const char *ssid, const char *password, int timeout_s)
{
    if (!ssid || !ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!s_async_lock) s_async_lock = xSemaphoreCreateMutex();
    if (!s_async_lock) return ESP_ERR_NO_MEM;

    xSemaphoreTake(s_async_lock, portMAX_DELAY);
    if (s_async.running) {
        xSemaphoreGive(s_async_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_async.running  = true;
    s_async.done     = false;
    s_async.ok       = false;
    s_async.error[0] = '\0';
    strlcpy(s_async_ssid, ssid, sizeof(s_async_ssid));
    strlcpy(s_async_pass, password ? password : "", sizeof(s_async_pass));
    s_async_timeout  = timeout_s;
    xSemaphoreGive(s_async_lock);

    if (xTaskCreate(test_async_task, "wifi_test", 4096, NULL, 4, NULL) != pdPASS) {
        xSemaphoreTake(s_async_lock, portMAX_DELAY);
        s_async.running = false;
        xSemaphoreGive(s_async_lock);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void wifi_mgr_test_result(wifi_test_async_t *out)
{
    if (!out) return;
    if (!s_async_lock) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_async_lock, portMAX_DELAY);
    *out = s_async;
    xSemaphoreGive(s_async_lock);
}
