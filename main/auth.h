/* auth — password hashing + session tokens.
 *
 * PBKDF2-SHA256 with a per-install 16-byte salt and configurable iteration
 * count (default 10000 — modest for an ESP32-S3 but enough to deter casual
 * brute force on a LAN device).
 *
 * Session tokens are 32 random bytes, base64url-encoded for the wire. They
 * live in RAM only — a reboot invalidates all sessions.
 *
 * The number of concurrent sessions is small (8) so we use a flat array.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_TOKEN_STR_LEN  44   /* base64url of 32 bytes, no padding */

esp_err_t auth_init(void);

/* Set the admin password. Generates a fresh salt and writes the hash to NVS
 * via app_config. Marks the device "configured". */
esp_err_t auth_set_password(const char *password);

/* First-boot setup: atomically check that the device is not yet configured
 * and set the password. Two concurrent setup POSTs cannot both succeed —
 * the second receives ESP_ERR_INVALID_STATE. */
esp_err_t auth_set_initial_password(const char *password);

/* Returns true if the candidate password matches the stored hash. */
bool auth_verify_password(const char *password);

/* Issue a new session token. Writes a NUL-terminated base64url string of
 * length AUTH_TOKEN_STR_LEN-1 into out_token (buffer >= AUTH_TOKEN_STR_LEN). */
esp_err_t auth_issue_token(char *out_token);

/* Returns true if token is a currently valid session (refreshes activity). */
bool auth_validate_token(const char *token);

/* Revoke a token (logout). No-op if unknown. */
void auth_revoke_token(const char *token);

/* Revoke every issued token (e.g. on password change). */
void auth_revoke_all(void);

#ifdef __cplusplus
}
#endif
