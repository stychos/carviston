/* app_config — NVS-backed persistent configuration.
 *
 * One struct, one mutex, one save call. Modules read fields directly via
 * app_config_get_*() snapshot copies; writes go through app_config_update()
 * + app_config_save().
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_CFG_PASSWORD_HASH_LEN  32   /* PBKDF2-SHA256 output */
#define APP_CFG_PASSWORD_SALT_LEN  16
#define APP_CFG_TOKEN_BYTES        32   /* session token raw bytes */
#define APP_CFG_WIFI_SSID_MAX      33
#define APP_CFG_WIFI_PASS_MAX      65
#define APP_CFG_WIFI_COUNTRY_MAX   4    /* ISO-3166 alpha-2 + optional env char + NUL; "01" = world */
#define APP_CFG_DEVICE_NAME_MAX    32   /* user-facing display name shown in the UI */
#define APP_CFG_HOSTNAME_MAX       32   /* network hostname (DHCP + mDNS label); empty → "carviston" */
#define APP_CFG_SCHED_MAX          16   /* max scheduled on/off actions */
#define APP_CFG_TZ_MAX             40   /* POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" */

typedef enum {
    HEATING_MODE_SUPER_FAST = 0, /* both heaters continuous until target */
    HEATING_MODE_FAST,           /* both for N min, rest R min */
    HEATING_MODE_OPTIMAL,        /* alternate heaters every N min */
    HEATING_MODE_ECO,            /* alternate + rest R min after each cycle */
    HEATING_MODE_COUNT
} heating_mode_t;

typedef enum {
    POWER_LED_ALWAYS_ON = 0,         /* on whenever MCU is up */
    POWER_LED_HEATER_ACTIVE,         /* on only when any heater is active */
} power_led_mode_t;

typedef enum {
    ECO_LED_WIFI_STATE = 0,          /* indicate wifi connection state */
    ECO_LED_ECO_MODE,                /* on when <=1 heater is active */
} eco_led_mode_t;

typedef enum {
    TEMP_UNIT_CELSIUS = 0,
    TEMP_UNIT_FAHRENHEIT,
} temp_unit_t;

typedef enum {
    WIFI_ROLE_AP = 0,                /* SoftAP only */
    WIFI_ROLE_STA,                   /* join a network; recovery AP via sta_fallback_* */
} wifi_role_t;

/* One scheduled action: at HH:MM on the selected weekdays, turn the heater
 * on or off. Edge-triggered — fires once when local wall-clock crosses the
 * minute (see heater_control). `days` is a bitmask matching struct tm.tm_wday:
 * bit0=Sun, bit1=Mon … bit6=Sat; 0x7F = every day, 0 = never fires. */
typedef struct {
    uint8_t hour;       /* 0-23 */
    uint8_t minute;     /* 0-59 */
    uint8_t action;     /* 0 = turn off, 1 = turn on */
    uint8_t days;       /* weekday bitmask, bit0=Sun … bit6=Sat */
    bool    enabled;    /* per-entry on/off without deleting it */
} sched_entry_t;

/* APPEND-ONLY. This struct is persisted to NVS as a raw byte blob, and the
 * loader migrates across firmware versions (and OTA rollbacks) by copying the
 * overlapping byte-prefix — see CFG_LAYOUT_BASE in app_config.c. That only holds
 * if existing fields NEVER move: add new fields at the END, never reorder or
 * resize one in the middle, and "remove" a field by renaming it to a reserved
 * placeholder of the SAME size (keeping the bytes so an older firmware booted
 * after a rollback still finds its fields). Bump CFG_VERSION on any change. */
typedef struct {
    /* --- versioning --- */
    uint16_t version;                /* config schema version */

    /* --- identity --- */
    char device_name[APP_CFG_DEVICE_NAME_MAX];  /* display name in the web UI; empty → "Carviston" */
    char hostname[APP_CFG_HOSTNAME_MAX];         /* network name: DHCP hostname + mDNS; empty → "carviston" */

    /* --- first-boot / auth --- */
    bool configured;                 /* password has been set */
    uint8_t password_hash[APP_CFG_PASSWORD_HASH_LEN];
    uint8_t password_salt[APP_CFG_PASSWORD_SALT_LEN];
    uint32_t pbkdf2_iterations;
    bool dashboard_locked;           /* false (default): dashboard endpoints (read state,
                                        set target, toggle power, set mode, clear safety)
                                        are reachable without a token. true: every API
                                        call requires auth. Settings endpoints are
                                        ALWAYS password-protected regardless. */

    /* --- boiler --- */
    heating_mode_t heating_mode;
    uint8_t target_temp_c;           /* 40,50,60,70,80 */
    uint8_t shower_ready_c;          /* default 50 */

    /* heating mode timing (minutes) */
    uint8_t fast_on_min;             /* fast: both heaters on */
    uint8_t fast_rest_min;           /* fast: rest period */
    uint8_t optimal_swap_min;        /* optimal: per-heater on time */
    uint8_t eco_on_min;              /* eco: per-heater on time */
    uint8_t eco_rest_min;            /* eco: rest after each cycle */

    /* control band */
    uint8_t hysteresis_c;            /* default 3 */

    /* --- NTC calibration (Beta equation, per-probe in case probes differ) --- */
    uint32_t ntc_r25_ohm;            /* nominal resistance @25C, default 10000 */
    uint16_t ntc_beta;               /* Beta value, default 3950 */
    /* safety: max regulation-vs-safety delta (°C) before a tank's probe is
     * flagged faulted, PER TANK ([0]=inlet, [1]=outlet). The inlet tank
     * stratifies and cools faster during a draw, so its two NTCs legitimately
     * read further apart — it tolerates a wider window (default 20 °C) than the
     * outlet tank (default 10 °C). */
    uint8_t  probe_disagree_c[2];

    /* --- LEDs --- */
    power_led_mode_t power_led_mode;
    eco_led_mode_t   eco_led_mode;

    /* --- dashboard --- */
    temp_unit_t dashboard_unit;

    /* --- wifi --- */
    wifi_role_t wifi_mode;
    char  sta_ssid[APP_CFG_WIFI_SSID_MAX];
    char  sta_pass[APP_CFG_WIFI_PASS_MAX];
    char  ap_ssid[APP_CFG_WIFI_SSID_MAX];   /* if empty, derived from MAC */
    char  ap_pass[APP_CFG_WIFI_PASS_MAX];   /* if empty, AP is open (first boot) */
    char  wifi_country[APP_CFG_WIFI_COUNTRY_MAX]; /* regulatory domain: ISO code (e.g. "US","DE") or "01" world; empty → "US" */
    bool     sta_fallback_enabled;          /* in pure STA mode: bring up the recovery AP after sta_fallback_seconds without GOT_IP */
    uint16_t sta_fallback_seconds;          /* timeout (s) before the fallback AP comes up; default 60 */

    /* --- UX timings --- */
    uint16_t long_press_ms;                 /* button long-press threshold (default 1500) */
    uint16_t preview_release_ms;            /* +/- preview hold-after-release (default 3000) */

    /* --- benchmark --- */
    uint32_t bench_resume_threshold_s;      /* max seconds between reboot and resume; default 600 */

    /* --- power-state restore --- */
    bool restore_power_on_boot;             /* true (default): re-apply last_power_state at boot so
                                               the heater survives a reboot/power cut. false: front
                                               panel stays dark until the user presses POWER. */
    bool last_power_state;                  /* most recent master_enabled, persisted so a restore
                                               brings the heater back to where the user left it. */

    /* --- scheduler --- */
    bool          sched_enabled;            /* master switch for the whole schedule */
    uint8_t       sched_count;              /* number of valid entries [0..APP_CFG_SCHED_MAX] */
    sched_entry_t schedule[APP_CFG_SCHED_MAX];
    /* POSIX TZ string applied with setenv("TZ")+tzset() so the scheduler (and
     * event log / benchmark) reason in LOCAL wall-clock, not UTC. DST-aware when
     * the string carries a rule (e.g. "CET-1CEST,M3.5.0,M10.5.0/3"). Default
     * "UTC0". Without this the schedule fires on UTC — wrong by the user's
     * offset. See app_config_apply_timezone(). */
    char          posix_tz[APP_CFG_TZ_MAX];

    /* --- energy + heat-up tracking --- */
    /* Per-element electrical power (W), used to estimate consumed energy by
     * integrating relay on-time × watts. [0]=inlet, [1]=outlet; default 1500. */
    uint16_t element_watts[2];
    /* Water-draw detector (used to segment heating "programs"): a draw is flagged
     * when the inlet regulation temperature falls by >= draw_detect_drop_c within
     * draw_detect_window_s. A draw ends the active program and arms the next one.
     * Defaults: 5 °C over 90 s — fast/deep enough to separate a real draw from
     * slow passive cooling or the hysteresis maintenance band. */
    uint8_t  draw_detect_drop_c;
    uint16_t draw_detect_window_s;
    /* A heating program is logged only when heating begins with at least this
     * many °C still to go to target — so routine hysteresis reheats (gap ~0)
     * don't spam the log with short runs. Default 5. */
    uint8_t  bench_min_gap_c;

} app_config_t;

/* Load config from NVS, applying defaults for any missing fields.
 * Safe to call once at boot. */
esp_err_t app_config_init(void);

/* Apply the configured POSIX timezone to the C library (setenv("TZ")+tzset()).
 * Called by app_config_init(); call again after the timezone setting changes. */
void app_config_apply_timezone(void);

/* Atomically copy the current config into *out. */
void app_config_get(app_config_t *out);

/* Persist *in to NVS. Updates in-memory snapshot. Synchronous — blocks on
 * the flash sector erase. Use for first-boot / setup paths. */
esp_err_t app_config_save(const app_config_t *in);

/* Same as app_config_save() for the in-memory effect, but the NVS commit
 * is deferred to a low-priority worker task. Multiple rapid calls coalesce
 * into a single flash write after a short idle window, which is what rapid
 * UI updates need when a slider is dragged (rapid setpoint changes). */
esp_err_t app_config_save_deferred(const app_config_t *in);

/* Wipe NVS + restore defaults. Used by "Hardware Reset" in maintenance tab. */
esp_err_t app_config_factory_reset(void);

/* Erase only the main config blob (keeps the network + auth preserve store), so
 * the next boot comes up on defaults but stays connected and authenticated.
 * Used by the OTA "reset settings to defaults" option. */
esp_err_t app_config_erase_feature_blob(void);

/* Convenience helpers — read individual fields under the mutex. */
heating_mode_t app_config_get_heating_mode(void);
uint8_t        app_config_get_target_temp(void);
bool           app_config_is_configured(void);
/* Copy the display name (or "Carviston" if unset) into buf. */
void           app_config_get_device_name(char *buf, size_t len);
void           app_config_get_hostname(char *buf, size_t len);
bool           app_config_dashboard_locked(void);
temp_unit_t    app_config_get_dashboard_unit(void);

/* Returns "carviston-xxxxxx" using last 3 bytes of base MAC.
 * Caller-supplied buf, must be at least 20 bytes. */
void app_config_default_ap_ssid(char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif
