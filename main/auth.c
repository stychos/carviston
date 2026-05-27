#include "auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#include "app_config.h"

static const char *TAG = "auth";

#define MAX_SESSIONS         8
#define SESSION_TIMEOUT_US   (30ULL * 60ULL * 1000000ULL)  /* 30 min idle */

typedef struct {
    char     token[AUTH_TOKEN_STR_LEN];
    uint64_t last_seen_us;
    bool     in_use;
} session_t;

static session_t s_sessions[MAX_SESSIONS];
static SemaphoreHandle_t s_lock;

/* base64url encode without padding. dst must hold >= 4*ceil(len/3) + 1 bytes. */
static void b64url_encode(const uint8_t *src, size_t n, char *dst)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t i = 0, o = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)src[i] << 16) | ((uint32_t)src[i + 1] << 8) | src[i + 2];
        dst[o++] = alphabet[(v >> 18) & 0x3f];
        dst[o++] = alphabet[(v >> 12) & 0x3f];
        dst[o++] = alphabet[(v >> 6) & 0x3f];
        dst[o++] = alphabet[v & 0x3f];
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < n) v |= (uint32_t)src[i + 1] << 8;
        dst[o++] = alphabet[(v >> 18) & 0x3f];
        dst[o++] = alphabet[(v >> 12) & 0x3f];
        if (i + 1 < n) dst[o++] = alphabet[(v >> 6) & 0x3f];
    }
    dst[o] = '\0';
}

/* Setup mutex serialises auth_set_initial_password so two concurrent first-
 * boot POSTs to /api/auth/setup can't both pass the !configured check. */
static SemaphoreHandle_t s_setup_lock;

esp_err_t auth_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_setup_lock = xSemaphoreCreateMutex();
    if (!s_setup_lock) return ESP_ERR_NO_MEM;
    memset(s_sessions, 0, sizeof(s_sessions));
    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init failed: %d", (int)st);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t auth_set_initial_password(const char *password)
{
    /* Non-blocking acquire: a second concurrent setup POST sees the lock
     * held and gets INVALID_STATE rather than racing through. */
    if (xSemaphoreTake(s_setup_lock, 0) != pdTRUE) return ESP_ERR_INVALID_STATE;

    app_config_t cfg;
    app_config_get(&cfg);
    if (cfg.configured) {
        xSemaphoreGive(s_setup_lock);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = auth_set_password(password);
    xSemaphoreGive(s_setup_lock);
    return err;
}

/* PBKDF2-HMAC-SHA256 via PSA crypto API. mbedtls 4.x makes the legacy md/sha
 * symbols private; PSA is the supported public path. */
static int derive(const char *password, const uint8_t *salt, size_t salt_len,
                  uint32_t iters, uint8_t out[APP_CFG_PASSWORD_HASH_LEN])
{
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    psa_status_t st;

    st = psa_key_derivation_setup(&op, PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256));
    if (st != PSA_SUCCESS) goto fail;
    st = psa_key_derivation_set_capacity(&op, APP_CFG_PASSWORD_HASH_LEN);
    if (st != PSA_SUCCESS) goto fail;
    st = psa_key_derivation_input_integer(&op, PSA_KEY_DERIVATION_INPUT_COST, iters);
    if (st != PSA_SUCCESS) goto fail;
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT,
                                        salt, salt_len);
    if (st != PSA_SUCCESS) goto fail;
    st = psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_PASSWORD,
                                        (const uint8_t *)password, strlen(password));
    if (st != PSA_SUCCESS) goto fail;
    st = psa_key_derivation_output_bytes(&op, out, APP_CFG_PASSWORD_HASH_LEN);

fail:
    psa_key_derivation_abort(&op);
    return (st == PSA_SUCCESS) ? 0 : -1;
}

esp_err_t auth_set_password(const char *password)
{
    if (!password || strlen(password) < 4) return ESP_ERR_INVALID_ARG;

    app_config_t cfg;
    app_config_get(&cfg);

    /* fresh salt + iteration count */
    esp_fill_random(cfg.password_salt, APP_CFG_PASSWORD_SALT_LEN);
    if (cfg.pbkdf2_iterations < 1000) cfg.pbkdf2_iterations = 10000;

    if (derive(password, cfg.password_salt, APP_CFG_PASSWORD_SALT_LEN,
               cfg.pbkdf2_iterations, cfg.password_hash) != 0) {
        ESP_LOGE(TAG, "PBKDF2 derive failed");
        return ESP_FAIL;
    }
    cfg.configured = true;
    esp_err_t err = app_config_save(&cfg);
    if (err == ESP_OK) {
        auth_revoke_all();
        ESP_LOGI(TAG, "password set; all sessions revoked");
    }
    return err;
}

bool auth_verify_password(const char *password)
{
    if (!password) return false;
    app_config_t cfg;
    app_config_get(&cfg);
    if (!cfg.configured) return false;

    uint8_t cand[APP_CFG_PASSWORD_HASH_LEN];
    if (derive(password, cfg.password_salt, APP_CFG_PASSWORD_SALT_LEN,
               cfg.pbkdf2_iterations, cand) != 0) {
        return false;
    }
    /* constant-time compare */
    uint8_t diff = 0;
    for (size_t i = 0; i < APP_CFG_PASSWORD_HASH_LEN; ++i) {
        diff |= cand[i] ^ cfg.password_hash[i];
    }
    return diff == 0;
}

esp_err_t auth_issue_token(char *out_token)
{
    if (!out_token) return ESP_ERR_INVALID_ARG;

    uint8_t raw[APP_CFG_TOKEN_BYTES];
    esp_fill_random(raw, sizeof(raw));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int slot = -1;
    uint64_t oldest = UINT64_MAX;
    int oldest_slot = 0;
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (!s_sessions[i].in_use) { slot = i; break; }
        if (s_sessions[i].last_seen_us < oldest) {
            oldest = s_sessions[i].last_seen_us;
            oldest_slot = i;
        }
    }
    if (slot < 0) slot = oldest_slot;   /* evict oldest */
    b64url_encode(raw, sizeof(raw), s_sessions[slot].token);
    s_sessions[slot].last_seen_us = esp_timer_get_time();
    s_sessions[slot].in_use = true;
    strncpy(out_token, s_sessions[slot].token, AUTH_TOKEN_STR_LEN);
    out_token[AUTH_TOKEN_STR_LEN - 1] = '\0';
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

bool auth_validate_token(const char *token)
{
    if (!token || !*token) return false;
    bool ok = false;
    uint64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (s_sessions[i].in_use &&
            now - s_sessions[i].last_seen_us < SESSION_TIMEOUT_US &&
            strncmp(s_sessions[i].token, token, AUTH_TOKEN_STR_LEN) == 0) {
            s_sessions[i].last_seen_us = now;
            ok = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

void auth_revoke_token(const char *token)
{
    if (!token) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_SESSIONS; ++i) {
        if (s_sessions[i].in_use &&
            strncmp(s_sessions[i].token, token, AUTH_TOKEN_STR_LEN) == 0) {
            memset(&s_sessions[i], 0, sizeof(s_sessions[i]));
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

void auth_revoke_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    xSemaphoreGive(s_lock);
}
