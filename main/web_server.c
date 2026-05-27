#include "web_server.h"

#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "app_config.h"
#include "auth.h"
#include "event_log.h"
#include "heater_control.h"
#include "ota.h"
#include "safety.h"
#include "web_fs.h"
#include "wifi_mgr.h"

static const char *TAG = "web";

extern const uint8_t setup_html_start[] asm("_binary_setup_html_start");
extern const uint8_t setup_html_end[]   asm("_binary_setup_html_end");

static httpd_handle_t s_httpd;

/* WebSocket client table. Each connected dashboard occupies one slot;
 * ws_pusher_task re-validates the cached token on every push and drops the
 * slot when the session is no longer authentic (logout, password change,
 * factory reset, session timeout). Two clients on different devices both
 * receive updates. */
#define MAX_WS_CLIENTS 4
typedef struct {
    int  fd;            /* -1 = slot empty */
    char token[AUTH_TOKEN_STR_LEN + 4];
} ws_client_t;
static ws_client_t s_ws[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_lock;

/* ---------- helpers ---------- */

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status)
{
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_FAIL;
    char status_str[8];
    snprintf(status_str, sizeof(status_str), "%d", status);
    httpd_resp_set_status(req, (status == 200) ? "200 OK" :
                               (status == 201) ? "201 Created" :
                               (status == 400) ? "400 Bad Request" :
                               (status == 401) ? "401 Unauthorized" :
                               (status == 403) ? "403 Forbidden" :
                               (status == 404) ? "404 Not Found" :
                                                 "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, s, HTTPD_RESP_USE_STRLEN);
    free(s);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg ? msg : "error");
    return send_json(req, o, status);
}

static cJSON *read_json_body(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 4096) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) { free(buf); return NULL; }
        got += r;
    }
    buf[total] = '\0';
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

/* Extract token from Authorization: Bearer <token>. Returns NULL if absent. */
static bool extract_token(httpd_req_t *req, char *out, size_t outsz)
{
    char hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK)
        return false;
    const char *p = hdr;
    while (*p == ' ') p++;
    if (strncasecmp(p, "Bearer ", 7) != 0) return false;
    p += 7;
    strlcpy(out, p, outsz);
    return out[0] != '\0';
}

static bool require_auth(httpd_req_t *req)
{
    char tok[AUTH_TOKEN_STR_LEN + 4];
    if (!extract_token(req, tok, sizeof(tok)) || !auth_validate_token(tok)) {
        send_error(req, 401, "unauthorized");
        return false;
    }
    return true;
}

/* Dashboard-level gate: open when app_config.dashboard_locked is false,
 * otherwise behaves exactly like require_auth. Use for endpoints that
 * back the dashboard UI (read state, set target, toggle power, set mode,
 * clear safety latch). Settings/maintenance endpoints keep require_auth. */
static bool require_dashboard(httpd_req_t *req)
{
    if (!app_config_dashboard_locked()) return true;
    return require_auth(req);
}

/* ---------- static / index ---------- */

static esp_err_t send_file_from_spiffs(httpd_req_t *req, const char *path,
                                       const char *mime)
{
    char full[80];
    snprintf(full, sizeof(full), "/web%s", path);
    FILE *f = fopen(full, "rb");
    if (!f) return ESP_FAIL;
    struct stat st;
    if (stat(full, &st) != 0) { fclose(f); return ESP_FAIL; }
    httpd_resp_set_type(req, mime);
    /* gz-precompressed bundles can set Content-Encoding via .gz suffix later. */
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) { fclose(f); return ESP_FAIL; }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const char *guess_mime(const char *uri)
{
    const char *dot = strrchr(uri, '.');
    if (!dot) return "application/octet-stream";
    if (!strcasecmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcasecmp(dot, ".js"))   return "application/javascript";
    if (!strcasecmp(dot, ".css"))  return "text/css";
    if (!strcasecmp(dot, ".json")) return "application/json";
    if (!strcasecmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(dot, ".png"))  return "image/png";
    if (!strcasecmp(dot, ".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

static esp_err_t serve_embedded_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, (const char *)setup_html_start,
                           setup_html_end - setup_html_start);
}

static esp_err_t static_get(httpd_req_t *req)
{
    /* If SPIFFS bundle is present, serve from it. Otherwise: every non-API
     * GET resolves to the embedded fallback (SPA-style routing). */
    if (web_fs_has_bundle()) {
        const char *uri = req->uri;
        if (strcmp(uri, "/") == 0) uri = "/index.html";
        if (send_file_from_spiffs(req, uri, guess_mime(uri)) == ESP_OK) return ESP_OK;
        /* SPA fallback to index */
        return send_file_from_spiffs(req, "/index.html", "text/html; charset=utf-8");
    }
    return serve_embedded_index(req);
}

/* ---------- auth API ---------- */

static esp_err_t api_auth_status(httpd_req_t *req)
{
    bool authed = false;
    char tok[AUTH_TOKEN_STR_LEN + 4];
    if (extract_token(req, tok, sizeof(tok)) && auth_validate_token(tok)) authed = true;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "configured",      app_config_is_configured());
    cJSON_AddBoolToObject(o, "authenticated",   authed);
    cJSON_AddBoolToObject(o, "dashboard_locked", app_config_dashboard_locked());
    return send_json(req, o, 200);
}

static esp_err_t api_auth_setup(httpd_req_t *req)
{
    /* Don't pre-check `configured` and then call set_password: the gap is a
     * TOCTOU on first-boot. auth_set_initial_password takes a non-blocking
     * mutex AND re-checks `configured` inside it. */
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(pw) && pw->valuestring) {
        err = auth_set_initial_password(pw->valuestring);
    }
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_STATE) return send_error(req, 403, "already configured");
    if (err != ESP_OK)                return send_error(req, 400, "could not set password");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o, 201);
}

static esp_err_t api_auth_login(httpd_req_t *req)
{
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");
    const cJSON *pw = cJSON_GetObjectItemCaseSensitive(body, "password");
    bool ok = cJSON_IsString(pw) && pw->valuestring && auth_verify_password(pw->valuestring);
    cJSON_Delete(body);
    if (!ok) {
        vTaskDelay(pdMS_TO_TICKS(500));   /* simple rate-limit */
        return send_error(req, 401, "invalid credentials");
    }
    char token[AUTH_TOKEN_STR_LEN];
    if (auth_issue_token(token) != ESP_OK) return send_error(req, 500, "token issue");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "token", token);
    return send_json(req, o, 200);
}

static esp_err_t api_auth_logout(httpd_req_t *req)
{
    char tok[AUTH_TOKEN_STR_LEN + 4];
    if (extract_token(req, tok, sizeof(tok))) auth_revoke_token(tok);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o, 200);
}

/* ---------- state ---------- */

static cJSON *state_json(void)
{
    heater_state_t st;
    heater_get_state(&st);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "target_c", st.target_c);
    cJSON_AddNumberToObject(o, "mode", st.mode);
    cJSON_AddBoolToObject(o, "master_enabled", st.master_enabled);
    if (!st.temp.all_fault) {
        cJSON_AddNumberToObject(o, "water_c", st.temp.water_c);
    } else {
        cJSON_AddNullToObject(o, "water_c");
    }
    cJSON *probes = cJSON_CreateArray();
    for (int i = 0; i < PROBE_COUNT; ++i) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "regulation_c", st.temp.probe[i].regulation_c);
        cJSON_AddNumberToObject(p, "safety_c",     st.temp.probe[i].safety_c);
        cJSON_AddBoolToObject(p, "fault",          st.temp.probe[i].fault);
        cJSON_AddItemToArray(probes, p);
    }
    cJSON_AddItemToObject(o, "probes", probes);

    cJSON *heaters = cJSON_CreateArray();
    cJSON_AddItemToArray(heaters, cJSON_CreateBool(st.heater_active[0]));
    cJSON_AddItemToArray(heaters, cJSON_CreateBool(st.heater_active[1]));
    cJSON_AddItemToObject(o, "heater_active", heaters);

    cJSON_AddStringToObject(o, "phase", st.phase_name ? st.phase_name : "idle");
    cJSON_AddNumberToObject(o, "phase_seconds_left", st.phase_seconds_left);
    cJSON_AddNumberToObject(o, "safety", st.safety);
    cJSON_AddBoolToObject(o, "shower_ready", st.shower_ready);

    cJSON_AddStringToObject(o, "wifi_ssid",    wifi_mgr_current_ssid());
    cJSON_AddStringToObject(o, "wifi_ap_ssid", wifi_mgr_ap_ssid());
    cJSON_AddNumberToObject(o, "wifi_rssi",    wifi_mgr_rssi());
    cJSON_AddNumberToObject(o, "wifi_state",   wifi_mgr_state());

    return o;
}

static esp_err_t api_state(httpd_req_t *req)
{
    if (!require_dashboard(req)) return ESP_OK;
    return send_json(req, state_json(), 200);
}

/* ---------- config ---------- */

static cJSON *config_to_json(const app_config_t *c)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "heating_mode", c->heating_mode);
    cJSON_AddNumberToObject(o, "target_c", c->target_temp_c);
    cJSON_AddNumberToObject(o, "shower_ready_c", c->shower_ready_c);
    cJSON_AddNumberToObject(o, "fast_on_min", c->fast_on_min);
    cJSON_AddNumberToObject(o, "fast_rest_min", c->fast_rest_min);
    cJSON_AddNumberToObject(o, "optimal_swap_min", c->optimal_swap_min);
    cJSON_AddNumberToObject(o, "eco_on_min", c->eco_on_min);
    cJSON_AddNumberToObject(o, "eco_rest_min", c->eco_rest_min);
    cJSON_AddNumberToObject(o, "hysteresis_c", c->hysteresis_c);
    cJSON_AddNumberToObject(o, "ntc_r25_ohm", c->ntc_r25_ohm);
    cJSON_AddNumberToObject(o, "ntc_beta", c->ntc_beta);
    cJSON_AddNumberToObject(o, "probe_disagree_c", c->probe_disagree_c);
    cJSON_AddNumberToObject(o, "power_led_mode", c->power_led_mode);
    cJSON_AddNumberToObject(o, "eco_led_mode", c->eco_led_mode);
    cJSON_AddNumberToObject(o, "dashboard_unit", c->dashboard_unit);
    cJSON_AddNumberToObject(o, "wifi_mode", c->wifi_mode);
    cJSON_AddStringToObject(o, "sta_ssid", c->sta_ssid);
    cJSON_AddStringToObject(o, "ap_ssid", c->ap_ssid);
    cJSON_AddNumberToObject(o, "hybrid_sta_seconds", c->hybrid_sta_seconds);
    cJSON_AddNumberToObject(o, "long_press_ms", c->long_press_ms);
    cJSON_AddNumberToObject(o, "preview_release_ms", c->preview_release_ms);
    cJSON_AddNumberToObject(o, "bench_resume_threshold_s", c->bench_resume_threshold_s);
    /* never serialise the AP/STA passwords back to client; just signal "set" */
    cJSON_AddBoolToObject(o, "sta_pass_set",    c->sta_pass[0] != '\0');
    cJSON_AddBoolToObject(o, "ap_pass_set",     c->ap_pass[0]  != '\0');
    cJSON_AddBoolToObject(o, "dashboard_locked", c->dashboard_locked);
    return o;
}

#define UPDATE_NUM(field, key, lo, hi) do {                                   \
    const cJSON *_v = cJSON_GetObjectItemCaseSensitive(body, key);            \
    if (cJSON_IsNumber(_v)) {                                                 \
        int _i = _v->valueint;                                                \
        if (_i >= (lo) && _i <= (hi)) cfg.field = _i;                         \
    }                                                                         \
} while (0)

static esp_err_t api_config_get(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    app_config_t c;
    app_config_get(&c);
    return send_json(req, config_to_json(&c), 200);
}

static esp_err_t api_config_put(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");

    app_config_t cfg;
    app_config_get(&cfg);

    /* Snapshot wifi-affecting fields before mutation so we can decide whether
     * to call wifi_mgr_restart() at the end. heating_mode/target_temp_c are
     * applied through the heater_* setters (so mode-state reset and event
     * logging fire) instead of being written into cfg directly. */
    app_config_t before = cfg;
    int new_heating_mode = -1;   /* sentinel = no change requested */
    int new_target_c     = -1;

    const cJSON *v;
    v = cJSON_GetObjectItemCaseSensitive(body, "heating_mode");
    if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint < HEATING_MODE_COUNT) {
        new_heating_mode = v->valueint;
    }
    v = cJSON_GetObjectItemCaseSensitive(body, "target_c");
    if (cJSON_IsNumber(v) && v->valueint >= 40 && v->valueint <= 80) {
        new_target_c = v->valueint;
    }

    UPDATE_NUM(shower_ready_c,     "shower_ready_c",   30, 70);
    UPDATE_NUM(fast_on_min,        "fast_on_min",      1, 120);
    UPDATE_NUM(fast_rest_min,      "fast_rest_min",    0, 120);
    UPDATE_NUM(optimal_swap_min,   "optimal_swap_min", 1, 120);
    UPDATE_NUM(eco_on_min,         "eco_on_min",       1, 120);
    UPDATE_NUM(eco_rest_min,       "eco_rest_min",     0, 240);
    UPDATE_NUM(hysteresis_c,       "hysteresis_c",     1, 10);
    UPDATE_NUM(ntc_r25_ohm,        "ntc_r25_ohm",      1000, 1000000);
    UPDATE_NUM(ntc_beta,           "ntc_beta",         2000, 5500);
    UPDATE_NUM(probe_disagree_c,   "probe_disagree_c", 1, 20);
    UPDATE_NUM(power_led_mode,     "power_led_mode",   0, 1);
    UPDATE_NUM(eco_led_mode,       "eco_led_mode",     0, 1);
    UPDATE_NUM(dashboard_unit,     "dashboard_unit",   0, 1);
    UPDATE_NUM(wifi_mode,          "wifi_mode",        0, 2);
    UPDATE_NUM(hybrid_sta_seconds, "hybrid_sta_seconds", 5, 600);
    UPDATE_NUM(long_press_ms,           "long_press_ms",           500, 5000);
    UPDATE_NUM(preview_release_ms,      "preview_release_ms",      500, 10000);
    UPDATE_NUM(bench_resume_threshold_s,"bench_resume_threshold_s", 0,   86400);

    v = cJSON_GetObjectItemCaseSensitive(body, "dashboard_locked");
    if (cJSON_IsBool(v)) cfg.dashboard_locked = cJSON_IsTrue(v);

    v = cJSON_GetObjectItemCaseSensitive(body, "sta_ssid");
    if (cJSON_IsString(v) && v->valuestring) strlcpy(cfg.sta_ssid, v->valuestring, sizeof(cfg.sta_ssid));
    v = cJSON_GetObjectItemCaseSensitive(body, "sta_pass");
    if (cJSON_IsString(v) && v->valuestring) strlcpy(cfg.sta_pass, v->valuestring, sizeof(cfg.sta_pass));
    v = cJSON_GetObjectItemCaseSensitive(body, "ap_ssid");
    if (cJSON_IsString(v) && v->valuestring) strlcpy(cfg.ap_ssid, v->valuestring, sizeof(cfg.ap_ssid));
    v = cJSON_GetObjectItemCaseSensitive(body, "ap_pass");
    if (cJSON_IsString(v) && v->valuestring) strlcpy(cfg.ap_pass, v->valuestring, sizeof(cfg.ap_pass));

    cJSON_Delete(body);
    esp_err_t err = app_config_save(&cfg);
    if (err != ESP_OK) return send_error(req, 500, "save failed");

    /* Route mode/target through the setters so heater_control resets its
     * mode_state and the event log records the change. Setters re-read the
     * config and only save if the field actually changed, so this won't
     * double-write to NVS. */
    if (new_heating_mode >= 0) heater_set_mode((heating_mode_t)new_heating_mode);
    if (new_target_c >= 0)     heater_set_target((uint8_t)new_target_c);

    /* If anything wifi-affecting changed, restart the radio so the new mode
     * actually takes effect without forcing the user to reboot manually. */
    bool wifi_changed = before.wifi_mode != cfg.wifi_mode
        || strcmp(before.sta_ssid, cfg.sta_ssid) != 0
        || strcmp(before.sta_pass, cfg.sta_pass) != 0
        || strcmp(before.ap_ssid,  cfg.ap_ssid)  != 0
        || strcmp(before.ap_pass,  cfg.ap_pass)  != 0;
    if (wifi_changed) wifi_mgr_restart();

    return api_config_get(req);
}

static esp_err_t api_change_password(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(body, "old");
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(body, "new");
    if (!cJSON_IsString(o) || !cJSON_IsString(n) || strlen(n->valuestring) < 4) {
        cJSON_Delete(body);
        return send_error(req, 400, "bad input");
    }
    bool ok = auth_verify_password(o->valuestring);
    esp_err_t err = ok ? auth_set_password(n->valuestring) : ESP_FAIL;
    cJSON_Delete(body);
    if (!ok) return send_error(req, 401, "wrong current password");
    if (err != ESP_OK) return send_error(req, 500, "could not set");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, r, 200);
}

/* ---------- heater controls ---------- */

static esp_err_t api_heater_target(httpd_req_t *req)
{
    if (!require_dashboard(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(body, "celsius");
    int v = cJSON_IsNumber(c) ? c->valueint : 0;
    cJSON_Delete(body);
    if (v < 40 || v > 80) return send_error(req, 400, "celsius must be 40..80");
    heater_set_target((uint8_t)v);
    return send_json(req, state_json(), 200);
}

static esp_err_t api_heater_power(httpd_req_t *req)
{
    if (!require_dashboard(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    /* Accept either {"on":true|false} or empty body → toggle. */
    const cJSON *on = body ? cJSON_GetObjectItemCaseSensitive(body, "on") : NULL;
    if (cJSON_IsBool(on)) heater_set_master_enabled(cJSON_IsTrue(on));
    else heater_toggle_master();
    if (body) cJSON_Delete(body);
    return send_json(req, state_json(), 200);
}

static esp_err_t api_heater_mode(httpd_req_t *req)
{
    if (!require_dashboard(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(body, "mode");
    int v = cJSON_IsNumber(m) ? m->valueint : -1;
    cJSON_Delete(body);
    if (v < 0 || v >= HEATING_MODE_COUNT) return send_error(req, 400, "bad mode");
    heater_set_mode((heating_mode_t)v);
    return send_json(req, state_json(), 200);
}

static esp_err_t api_safety_clear(httpd_req_t *req)
{
    if (!require_dashboard(req)) return ESP_OK;
    /* heater_clear_safety_fault forces master off before dropping the latch,
     * so clearing a fault never silently re-energises relays. */
    heater_clear_safety_fault();
    return send_json(req, state_json(), 200);
}

/* ---------- wifi ---------- */

static esp_err_t api_wifi_test(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    cJSON *body = read_json_body(req);
    if (!body) return send_error(req, 400, "invalid body");

    const cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(body, "password");
    const cJSON *to_j   = cJSON_GetObjectItemCaseSensitive(body, "timeout_s");
    if (!cJSON_IsString(ssid_j) || !ssid_j->valuestring || !ssid_j->valuestring[0]) {
        cJSON_Delete(body);
        return send_error(req, 400, "ssid required");
    }
    const char *ssid = ssid_j->valuestring;
    const char *pass = (cJSON_IsString(pass_j) && pass_j->valuestring) ? pass_j->valuestring : "";
    int timeout_s = (cJSON_IsNumber(to_j)) ? to_j->valueint : 12;

    wifi_test_result_t r;
    esp_err_t err = wifi_mgr_test_sta(ssid, pass, timeout_s, &r);
    cJSON_Delete(body);

    if (err == ESP_ERR_INVALID_STATE) {
        /* Test couldn't be run at all (busy, or device is in pure STA mode). */
        return send_error(req, 409, r.error[0] ? r.error : "cannot test now");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, r.error[0] ? r.error : "test failed");
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "ok",    r.ok);
    cJSON_AddStringToObject(o, "error", r.error);
    return send_json(req, o, 200);
}

static esp_err_t api_wifi_scan(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    wifi_scan_entry_t entries[16];
    int n = wifi_mgr_scan(entries, 16);
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", entries[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", entries[i].rssi);
        cJSON_AddNumberToObject(e, "auth", entries[i].authmode);
        cJSON_AddNumberToObject(e, "channel", entries[i].channel);
        cJSON_AddItemToArray(a, e);
    }
    return send_json(req, a, 200);
}

static void delayed_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(800));
    event_log_emit(EV_SHUTDOWN, 0, 0, NULL);
    event_log_flush();
    esp_restart();
}

static esp_err_t api_reboot(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    event_log_emit(EV_REBOOT_REQUEST, 0, 0, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    send_json(req, o, 200);
    xTaskCreate(delayed_reboot_task, "reboot", 1024, NULL, 1, NULL);
    return ESP_OK;
}

static esp_err_t api_factory_reset(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    event_log_emit(EV_FACTORY_RESET, 0, 0, NULL);
    event_log_flush();
    app_config_factory_reset();
    auth_revoke_all();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    send_json(req, o, 200);
    xTaskCreate(delayed_reboot_task, "reboot", 1024, NULL, 1, NULL);
    return ESP_OK;
}

/* --- boot partition info & switch ---------------------------------------- */

/* Returns 0/1 for ota_0/ota_1, or -1 for factory or anything else. We treat
 * ota_0 and ota_1 as "OTA slot 0" and "OTA slot 1" in the UI. */
static int ota_slot_index(const esp_partition_t *p)
{
    if (!p) return -1;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0) return 0;
    if (p->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1) return 1;
    return -1;
}

/* True if the partition has something that looks like a valid ESP firmware
 * image (magic byte at offset 0). Used to decide whether the alternate
 * boot slot is safe to switch to. */
static bool partition_has_app(const esp_partition_t *p)
{
    if (!p) return false;
    uint8_t magic = 0;
    if (esp_partition_read(p, 0, &magic, 1) != ESP_OK) return false;
    return magic == ESP_IMAGE_HEADER_MAGIC;
}

static void add_partition_info(cJSON *parent, const char *key,
                               const esp_partition_t *p)
{
    cJSON *o = cJSON_CreateObject();
    if (p) {
        cJSON_AddStringToObject(o, "label", p->label);
        cJSON_AddNumberToObject(o, "slot", ota_slot_index(p));
        cJSON_AddBoolToObject  (o, "bootable", partition_has_app(p));
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(p, &state) == ESP_OK) {
            cJSON_AddNumberToObject(o, "state", (int)state);
        }
    } else {
        cJSON_AddNullToObject(o, "label");
        cJSON_AddNumberToObject(o, "slot", -1);
        cJSON_AddBoolToObject  (o, "bootable", false);
    }
    cJSON_AddItemToObject(parent, key, o);
}

static esp_err_t api_boot_info(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    const esp_partition_t *running   = esp_ota_get_running_partition();
    const esp_partition_t *alternate = esp_ota_get_next_update_partition(NULL);

    cJSON *o = cJSON_CreateObject();
    add_partition_info(o, "running",   running);
    add_partition_info(o, "alternate", alternate);
    return send_json(req, o, 200);
}

static esp_err_t api_boot_switch(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    const esp_partition_t *alternate = esp_ota_get_next_update_partition(NULL);
    if (!alternate)                        return send_error(req, 500, "no alternate slot");
    if (!partition_has_app(alternate))     return send_error(req, 400, "alternate slot is empty");
    esp_err_t err = esp_ota_set_boot_partition(alternate);
    if (err != ESP_OK) return send_error(req, 500, esp_err_to_name(err));

    event_log_emit(EV_REBOOT_REQUEST, (int16_t)ota_slot_index(alternate), 0, "boot_switch");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "ok", true);
    cJSON_AddStringToObject(o, "now_booting", alternate->label);
    send_json(req, o, 200);
    xTaskCreate(delayed_reboot_task, "reboot", 1024, NULL, 1, NULL);
    return ESP_OK;
}

/* ---------- event log ---------- */

static const char *event_type_name(uint8_t t)
{
    static const char *names[] = {
        "boot", "shutdown", "reboot_request", "factory_reset",
        "wifi_ap", "wifi_sta_connected", "wifi_sta_disconnected",
        "time_synced",
        "button_press", "button_long",
        "mode_change", "target_change",
        "master_on", "master_off",
        "heater_on", "heater_off",
        "safety_fault", "safety_cleared",
        "ota_start", "ota_done", "ota_fail",
        "bench_start", "bench_end", "bench_abort",
    };
    return (t < sizeof(names) / sizeof(names[0])) ? names[t] : "unknown";
}

static esp_err_t api_log(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    static event_record_t buf[64];
    int n = event_log_snapshot(buf, (int)(sizeof(buf) / sizeof(buf[0])));
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; ++i) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ts",       (double)buf[i].ts_unix);
        cJSON_AddNumberToObject(e, "boot",     buf[i].boot_num);
        cJSON_AddNumberToObject(e, "uptime_s", buf[i].uptime_s);
        cJSON_AddStringToObject(e, "type",     event_type_name(buf[i].type));
        cJSON_AddNumberToObject(e, "a",        buf[i].a);
        cJSON_AddNumberToObject(e, "b",        buf[i].b);
        cJSON_AddStringToObject(e, "text",     buf[i].text);
        cJSON_AddItemToArray(a, e);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "current_boot", event_log_current_boot());
    cJSON_AddItemToObject(o, "events", a);
    return send_json(req, o, 200);
}

/* ---------- Matter commissioning ---------- */
#include "matter_node.h"

static esp_err_t api_matter_code(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    matter_pairing_info_t pi;
    matter_node_get_pairing_info(&pi);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "active",  pi.active);
    cJSON_AddStringToObject(o, "qr",      pi.qr);
    cJSON_AddStringToObject(o, "manual",  pi.manual);
    cJSON_AddNumberToObject(o, "window_s_remaining", pi.window_s_remaining);
    return send_json(req, o, 200);
}

static esp_err_t api_matter_open(httpd_req_t *req)
{
    if (!require_auth(req)) return ESP_OK;
    /* Body is optional JSON { "window_s": 180 }; default 180. Matter spec
     * clamps the basic commissioning window to [180, 900] so we do the same
     * here — and report the clamped value in the response so the UI's
     * countdown matches what the chip stack actually enforces. */
    double w_in = 180;
    cJSON *j = read_json_body(req);
    if (j) {
        cJSON *w = cJSON_GetObjectItem(j, "window_s");
        if (cJSON_IsNumber(w)) w_in = w->valuedouble;
        cJSON_Delete(j);
    }
    if (!(w_in >= 180.0)) w_in = 180.0;   /* catches NaN too */
    if (w_in > 900.0)    w_in = 900.0;
    uint32_t window_s = (uint32_t)w_in;

    esp_err_t err = matter_node_open_pairing(window_s);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* Matter compiled out — let the client hide the UI cleanly. */
        return send_error(req, 503, "matter disabled in this build");
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject  (o, "ok", err == ESP_OK);
    cJSON_AddNumberToObject(o, "window_s", window_s);
    return send_json(req, o, err == ESP_OK ? 200 : 500);
}

/* ---------- WebSocket live state ---------- */

/* Add or replace a slot for this fd. If the slot is already used, refresh
 * the token (e.g. handshake reused on a new tab). Caller must NOT hold s_ws_lock. */
static void ws_table_add(int fd, const char *token)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
        if (s_ws[i].fd == fd) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
            if (s_ws[i].fd < 0) { slot = i; break; }
        }
    }
    if (slot < 0) {
        /* Table full: evict slot 0 (oldest by insertion order is good enough
         * given MAX_WS_CLIENTS=4). The evicted fd's TCP cleanup happens via
         * httpd's own session timeout. */
        slot = 0;
    }
    s_ws[slot].fd = fd;
    strlcpy(s_ws[slot].token, token, sizeof(s_ws[slot].token));
    xSemaphoreGive(s_ws_lock);
}

static void ws_table_drop(int fd)
{
    if (fd < 0) return;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
        if (s_ws[i].fd == fd) {
            s_ws[i].fd = -1;
            s_ws[i].token[0] = '\0';
        }
    }
    xSemaphoreGive(s_ws_lock);
}

void web_server_close_all_ws(void)
{
    if (!s_httpd) return;
    int fds[MAX_WS_CLIENTS];
    int n = 0;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
        if (s_ws[i].fd >= 0) {
            fds[n++] = s_ws[i].fd;
            s_ws[i].fd = -1;
            s_ws[i].token[0] = '\0';
        }
    }
    xSemaphoreGive(s_ws_lock);
    /* Trigger close OUTSIDE the lock — httpd may call back into our
     * handlers as part of teardown. */
    for (int i = 0; i < n; ++i) {
        httpd_sess_trigger_close(s_httpd, fds[i]);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Handshake. Token must be in ?token=... since browser WS API
         * doesn't allow custom headers. When dashboard is unlocked we
         * accept an empty token; ws_pusher_task's per-frame re-auth has
         * the same gate so a sane policy is enforced on every push. */
        char query[128];
        char token[AUTH_TOKEN_STR_LEN + 4] = { 0 };
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            httpd_query_key_value(query, "token", token, sizeof(token));
        }
        bool locked = app_config_dashboard_locked();
        if (locked && !auth_validate_token(token)) {
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid token");
            return ESP_FAIL;
        }
        int fd = httpd_req_to_sockfd(req);
        ws_table_add(fd, token);
        ESP_LOGI(TAG, "WS connected fd=%d %s", fd, locked ? "(auth)" : "(open)");
        return ESP_OK;
    }

    /* Inbound frame. We don't accept client→server messages, but the WS spec
     * requires us to drain frames properly (especially close + control). Peek
     * the length first so an oversize frame doesn't get truncated into our
     * stack buffer; if it's too big, discard cleanly and close the slot. */
    httpd_ws_frame_t frame = { 0 };
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        ws_table_drop(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_table_drop(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.len == 0) return ESP_OK;

    /* Bounded payload sink. We discard the contents — but we MUST consume
     * exactly `frame.len` bytes or the next frame's parse will desync. */
    uint8_t stack_buf[128];
    uint8_t *buf = stack_buf;
    bool heap = false;
    if (frame.len > sizeof(stack_buf)) {
        if (frame.len > 4096) {
            /* Cannot consume `frame.len` bytes safely — close the session so
             * the undrained payload doesn't desync the next frame parse. */
            int fd = httpd_req_to_sockfd(req);
            ws_table_drop(fd);
            httpd_sess_trigger_close(s_httpd, fd);
            return ESP_OK;
        }
        buf = malloc(frame.len);
        if (!buf) {
            /* Same desync risk if we can't allocate to drain — close cleanly. */
            int fd = httpd_req_to_sockfd(req);
            ws_table_drop(fd);
            httpd_sess_trigger_close(s_httpd, fd);
            return ESP_OK;
        }
        heap = true;
    }
    frame.payload = buf;
    httpd_ws_recv_frame(req, &frame, frame.len);
    if (heap) free(buf);
    return ESP_OK;
}

static void ws_pusher_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_httpd) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        cJSON *o = state_json();
        char *payload = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (!payload) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        /* Snapshot current slots under the lock, then push outside it so the
         * httpd handler can keep adding/removing while we send. */
        int     fds[MAX_WS_CLIENTS];
        char    tokens[MAX_WS_CLIENTS][AUTH_TOKEN_STR_LEN + 4];
        int     n = 0;
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; ++i) {
            if (s_ws[i].fd >= 0) {
                fds[n] = s_ws[i].fd;
                strlcpy(tokens[n], s_ws[i].token, sizeof(tokens[n]));
                n++;
            }
        }
        xSemaphoreGive(s_ws_lock);

        bool locked_now = app_config_dashboard_locked();
        for (int i = 0; i < n; ++i) {
            /* Per-frame re-auth — but only when the dashboard is locked.
             * If it's open, sessions can have an empty token (the handshake
             * accepts it). Validating an empty token would always fail and
             * close every open client, so we skip the check in that mode.
             * A revoked token (logout, password change, factory reset) still
             * drops its slot when locked. */
            if (locked_now && !auth_validate_token(tokens[i])) {
                ws_table_drop(fds[i]);
                /* Best-effort close frame so the client knows. */
                httpd_ws_frame_t close = { .type = HTTPD_WS_TYPE_CLOSE };
                httpd_ws_send_frame_async(s_httpd, fds[i], &close);
                continue;
            }
            httpd_ws_frame_t frame = {
                .type    = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)payload,
                .len     = strlen(payload),
            };
            if (httpd_ws_send_frame_async(s_httpd, fds[i], &frame) != ESP_OK) {
                ws_table_drop(fds[i]);
            }
        }
        free(payload);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------- route registration ---------- */

#define ROUTE(uri_, method_, handler_) do { \
    httpd_uri_t _u = { .uri = (uri_), .method = (method_), .handler = (handler_) }; \
    httpd_register_uri_handler(s_httpd, &_u); \
} while (0)

#define WS_ROUTE(uri_, handler_) do { \
    httpd_uri_t _u = { .uri = (uri_), .method = HTTP_GET, .handler = (handler_), \
                       .is_websocket = true }; \
    httpd_register_uri_handler(s_httpd, &_u); \
} while (0)

esp_err_t web_server_start(void)
{
    if (s_httpd) return ESP_OK;

    /* WebSocket client table. Initialised once; slots are recycled. */
    s_ws_lock = xSemaphoreCreateMutex();
    if (!s_ws_lock) return ESP_ERR_NO_MEM;
    for (int i = 0; i < MAX_WS_CLIENTS; ++i) s_ws[i].fd = -1;

    /* Mount the web FS (non-fatal if it fails). */
    web_fs_init();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn   = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 24;
    cfg.stack_size     = 8192;

    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    /* Let wifi_mgr tear our WebSocket sessions down before any radio
     * operation that would invalidate the underlying TCP sockets. */
    wifi_mgr_set_disruption_cb(web_server_close_all_ws);

    /* API routes — register specific before wildcard. */
    ROUTE("/api/auth/status",   HTTP_GET,  api_auth_status);
    ROUTE("/api/auth/setup",    HTTP_POST, api_auth_setup);
    ROUTE("/api/auth/login",    HTTP_POST, api_auth_login);
    ROUTE("/api/auth/logout",   HTTP_POST, api_auth_logout);
    ROUTE("/api/auth/password", HTTP_POST, api_change_password);

    ROUTE("/api/state",         HTTP_GET,  api_state);
    ROUTE("/api/config",        HTTP_GET,  api_config_get);
    ROUTE("/api/config",        HTTP_PUT,  api_config_put);

    ROUTE("/api/heater/target", HTTP_POST, api_heater_target);
    ROUTE("/api/heater/power",  HTTP_POST, api_heater_power);
    ROUTE("/api/heater/mode",   HTTP_POST, api_heater_mode);
    ROUTE("/api/safety/clear",  HTTP_POST, api_safety_clear);

    ROUTE("/api/wifi/scan",     HTTP_GET,  api_wifi_scan);
    ROUTE("/api/wifi/test",     HTTP_POST, api_wifi_test);

    ROUTE("/api/maintenance/reboot",        HTTP_POST, api_reboot);
    ROUTE("/api/maintenance/factory_reset", HTTP_POST, api_factory_reset);
    ROUTE("/api/maintenance/boot",          HTTP_GET,  api_boot_info);
    ROUTE("/api/maintenance/boot_switch",   HTTP_POST, api_boot_switch);

    ROUTE("/api/log", HTTP_GET, api_log);

    ROUTE("/api/matter/code", HTTP_GET,  api_matter_code);
    ROUTE("/api/matter/open", HTTP_POST, api_matter_open);

    ROUTE("/api/ota", HTTP_POST, ota_upload_handler);

    WS_ROUTE("/ws", ws_handler);

    /* Static fallback — wildcard, registered last. */
    ROUTE("/*", HTTP_GET, static_get);

    xTaskCreate(ws_pusher_task, "ws_push", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "HTTP server listening on :80");
    return ESP_OK;
}

void web_server_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}
