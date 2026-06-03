/* matter_node — Matter over WiFi implementation.
 *
 * When CONFIG_CARVISTON_MATTER_ENABLE=y this file is the real bridge between
 * the heater_control public API and esp-matter clusters. When disabled it
 * collapses to a single ESP_OK-returning stub so the rest of the firmware
 * builds without esp-matter installed.
 */

#include "matter_node.h"

#include <string.h>

#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "matter";

#ifdef CONFIG_CARVISTON_MATTER_ENABLE

#include <math.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "heater_control.h"

#include <esp_matter.h>
#include <esp_matter_attribute_utils.h>
#include <esp_matter_cluster.h>
#include <esp_matter_endpoint.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

/* --- Stack-wide handles -------------------------------------------------- */
static node_t     *s_node = nullptr;
static endpoint_t *s_wh_ep = nullptr;
static uint16_t    s_wh_endpoint_id = 0;
static heating_mode_t s_mode_before_boost = HEATING_MODE_OPTIMAL;
static bool          s_boost_active        = false;
/* Set true at the very end of matter_node_start(). All public APIs that
 * touch the chip stack must short-circuit until this flips, because
 * heater_control's task runs at higher priority and can call us before
 * esp_matter::start() has initialised Server::GetInstance(). */
static volatile bool s_matter_started = false;

/* Four per-NTC temperature sensor endpoints (EP 2-5).
 * Each mirrors one float in temperature_reading_t into a TemperatureSensor
 * device so phones can graph individual probes and spot drift. */
#define PROBE_EP_COUNT 4
static endpoint_t *s_probe_ep[PROBE_EP_COUNT]    = { nullptr, nullptr, nullptr, nullptr };
static uint16_t    s_probe_ep_id[PROBE_EP_COUNT] = { 0, 0, 0, 0 };

static const struct probe_meta_t {
    const char *label;       /* user-visible name (Fixed Label cluster) */
    uint8_t     tank_idx;    /* TANK_INLET = 0, TANK_OUTLET = 1 */
    bool        is_safety;   /* false = regulation NTC, true = safety NTC */
} s_probe_meta[PROBE_EP_COUNT] = {
    { "Inlet tank regulation",  TANK_INLET,  false },
    { "Inlet tank safety",      TANK_INLET,  true  },
    { "Outlet tank regulation", TANK_OUTLET, false },
    { "Outlet tank safety",     TANK_OUTLET, true  },
};

/* Pull the (signed) reading for NTC endpoint i out of a temperature_reading_t,
 * honouring the per-tank fault flag and NaN. Returns false if no valid value
 * should be published (caller must use the null marker). */
static bool probe_value_c(const temperature_reading_t &t, int i, float *out)
{
    const tank_reading_t &p = t.tank[s_probe_meta[i].tank_idx];
    float c = s_probe_meta[i].is_safety ? p.safety_c : p.regulation_c;
    if (p.fault || isnan(c)) return false;
    *out = c;
    return true;
}

/* EP 6 — GenericSwitch (latching) exposing safety_status_t (0..2).
 *
 *   Position 0  SAFETY_OK
 *   Position 1  SAFETY_FAULT_OVERTEMP     (a tank > soft 90 °C limit)
 *   Position 2  SAFETY_FAULT_NO_PROBES    (all tanks faulted, no reading)
 *
 * A single tank's probe failing is handled per-tank (the healthy tank keeps
 * running) and surfaces on that tank's TemperatureSensor endpoint going null,
 * not here. Each transition fires a SwitchLatched event so controllers can
 * wire "if heater faults → notify me" automations.
 */
static endpoint_t *s_safety_ep = nullptr;
static uint16_t    s_safety_ep_id = 0;

static const char *const s_safety_label[] = {
    "OK",
    "Over-temperature",
    "All tanks faulted",
};
#define SAFETY_POSITIONS ((uint8_t)(sizeof(s_safety_label) / sizeof(s_safety_label[0])))

/* --- Helpers ------------------------------------------------------------- */

/* Round/clamp a Matter setpoint (centi-°C, int16) to our 10 °C step set. */
static uint8_t setpoint_to_target_c(int16_t centi_c)
{
    int v = (centi_c + 500) / 1000 * 10;     /* nearest 10 °C */
    if (v < 40) v = 40;
    if (v > 80) v = 80;
    return (uint8_t)v;
}

/* Carviston heating_mode_t (0..3) ↔ WaterHeaterMode CurrentMode (1..4).
 * The Matter mode values are arbitrary — we publish them via SupportedModes
 * along with ModeTags so the controller knows which tag (Auto/Manual/etc.)
 * each option belongs to.
 *
 *   Matter CurrentMode   Carviston enum            Recommended ModeTags
 *   ──────────────────   ──────────────────────    ─────────────────────────
 *   1  "Super-fast"      HEATING_MODE_SUPER_FAST   Manual, Max, Quick
 *   2  "Fast"            HEATING_MODE_FAST         Manual, Quick
 *   3  "Optimal"         HEATING_MODE_OPTIMAL      Auto
 *   4  "ECO"             HEATING_MODE_ECO          Auto, LowEnergy
 */
static inline uint8_t matter_mode_from_carviston(heating_mode_t m)
{
    if (m >= HEATING_MODE_COUNT) m = HEATING_MODE_OPTIMAL;
    return (uint8_t)(m + 1);
}
static inline heating_mode_t carviston_mode_from_matter(uint8_t m)
{
    if (m < 1 || m > HEATING_MODE_COUNT) return HEATING_MODE_OPTIMAL;
    return (heating_mode_t)(m - 1);
}

/* --- Attribute write callback (Matter → heater_control) ------------------ */
static esp_err_t attribute_update_cb(attribute::callback_type_t type,
                                     uint16_t  endpoint_id,
                                     uint32_t  cluster_id,
                                     uint32_t  attribute_id,
                                     esp_matter_attr_val_t *val,
                                     void     *priv_data)
{
    if (type != attribute::PRE_UPDATE)        return ESP_OK;
    if (endpoint_id != s_wh_endpoint_id)      return ESP_OK;

    if (cluster_id == OnOff::Id) {
        if (attribute_id == OnOff::Attributes::OnOff::Id) {
            heater_set_master_enabled(val->val.b);
            ESP_LOGI(TAG, "OnOff ← %d", val->val.b);
        }
    } else if (cluster_id == Thermostat::Id) {
        if (attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
            uint8_t c = setpoint_to_target_c(val->val.i16);
            heater_set_target(c);
            ESP_LOGI(TAG, "Setpoint ← %d (snap %u°C)", val->val.i16, c);
        } else if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
            /* SystemMode: 0 = Off, 4 = Heat. */
            heater_set_master_enabled(val->val.u8 == 4);
            ESP_LOGI(TAG, "SystemMode ← %u", val->val.u8);
        }
    } else if (cluster_id == WaterHeaterMode::Id) {
        if (attribute_id == WaterHeaterMode::Attributes::CurrentMode::Id) {
            heating_mode_t m = carviston_mode_from_matter(val->val.u8);
            heater_set_mode(m);
            ESP_LOGI(TAG, "WH mode ← Matter=%u carviston=%d", val->val.u8, (int)m);
        }
    } else if (cluster_id == WaterHeaterManagement::Id) {
        if (attribute_id == WaterHeaterManagement::Attributes::BoostState::Id) {
            /* Boost / CancelBoost commands are routed through this attribute
             * by the chip layer. 0 = Inactive, 1 = Active.
             *
             * We track boost state in s_boost_active rather than reading it
             * back from heater_state, because (a) we need a place to remember
             * the pre-boost mode even if the user was already in SUPER_FAST,
             * and (b) the sync_task must publish BoostState from this flag
             * — not from shower_ready, which is a different signal (water
             * temperature-met indicator). */
            if (val->val.u8 == 1 && !s_boost_active) {
                /* Atomic swap means we cannot race against a concurrent
                 * panel button or web write that changes mode between the
                 * read and the set. */
                heating_mode_t prev;
                heater_atomic_swap_mode(HEATING_MODE_SUPER_FAST, &prev);
                s_mode_before_boost = prev;
                s_boost_active      = true;
                ESP_LOGI(TAG, "Boost ← Active (saved mode %d)", (int)s_mode_before_boost);
            } else if (val->val.u8 == 0 && s_boost_active) {
                s_boost_active = false;
                heater_atomic_swap_mode(s_mode_before_boost, NULL);
                ESP_LOGI(TAG, "Boost ← Inactive (restore mode %d)", (int)s_mode_before_boost);
            }
        }
    }
    return ESP_OK;
}

/* --- Identify callback (no LED effect yet; phase-6 work) ---------------- */
static esp_err_t identification_cb(identification::callback_type_t /*type*/,
                                   uint16_t /*endpoint_id*/,
                                   uint8_t  /*effect_id*/,
                                   uint8_t  /*effect_variant*/,
                                   void    *  /*priv_data*/)
{
    return ESP_OK;
}

/* Runs on the chip dispatcher thread (via PlatformMgr().ScheduleWork) so
 * we don't fire cluster events from the sync_task FreeRTOS context. The
 * arg carries the new safety-switch position in the low byte. */
static void safety_switch_latched_worker(intptr_t arg)
{
    uint8_t pos = (uint8_t)(arg & 0xff);
    switch_cluster::event::send_switch_latched(s_safety_ep_id, pos);
}

/* --- 1 Hz sync task (heater_control → Matter) ----------------------------
 *
 * Seed `prev` from the current heater state BEFORE entering the diff loop,
 * so the first iteration is just a plain diff. The earlier `bool first`
 * sentinel pushed every attribute unconditionally on tick 1, which echoed
 * back through attribute_update_cb (PRE_UPDATE fires on server-side writes)
 * and produced phantom event-log entries, NVS commits and LED flashes on
 * every reboot. */
static void sync_task(void * /*arg*/)
{
    heater_state_t prev;
    heater_get_state(&prev);
    bool prev_boost_active = s_boost_active;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        heater_state_t cur;
        heater_get_state(&cur);

        esp_matter_attr_val_t v;

        if (cur.master_enabled != prev.master_enabled) {
            v = esp_matter_bool(cur.master_enabled);
            attribute::update(s_wh_endpoint_id, OnOff::Id,
                              OnOff::Attributes::OnOff::Id, &v);

            uint8_t sysmode = cur.master_enabled ? 4 : 0;
            v = esp_matter_enum8(sysmode);
            attribute::update(s_wh_endpoint_id, Thermostat::Id,
                              Thermostat::Attributes::SystemMode::Id, &v);
        }

        if (cur.target_c != prev.target_c) {
            int16_t setpoint = (int16_t)cur.target_c * 100;
            v = esp_matter_int16(setpoint);
            attribute::update(s_wh_endpoint_id, Thermostat::Id,
                              Thermostat::Attributes::OccupiedHeatingSetpoint::Id, &v);
        }

        const bool temp_changed = (isnan(prev.temp.water_c) != isnan(cur.temp.water_c) ||
                                   (!isnan(cur.temp.water_c) &&
                                    fabsf(cur.temp.water_c - prev.temp.water_c) > 0.5f));
        if (temp_changed) {
            if (isnan(cur.temp.water_c)) {
                v = esp_matter_nullable_int16(nullable<int16_t>());
            } else {
                int16_t local = (int16_t)(cur.temp.water_c * 100.0f);
                v = esp_matter_nullable_int16(local);
            }
            attribute::update(s_wh_endpoint_id, Thermostat::Id,
                              Thermostat::Attributes::LocalTemperature::Id, &v);
            attribute::update(s_wh_endpoint_id, TemperatureMeasurement::Id,
                              TemperatureMeasurement::Attributes::MeasuredValue::Id, &v);
        }

        if (cur.mode != prev.mode) {
            v = esp_matter_enum8(matter_mode_from_carviston(cur.mode));
            attribute::update(s_wh_endpoint_id, WaterHeaterMode::Id,
                              WaterHeaterMode::Attributes::CurrentMode::Id, &v);
        }

        if (cur.heater_active[0] != prev.heater_active[0] ||
            cur.heater_active[1] != prev.heater_active[1]) {
            uint8_t hd = (uint8_t)((cur.heater_active[0] ? 1u : 0u) |
                                   (cur.heater_active[1] ? 2u : 0u));
            v = esp_matter_bitmap8(hd);
            attribute::update(s_wh_endpoint_id, WaterHeaterManagement::Id,
                              WaterHeaterManagement::Attributes::HeatDemand::Id, &v);
        }

        if (s_boost_active != prev_boost_active) {
            v = esp_matter_enum8(s_boost_active ? 1 : 0);
            attribute::update(s_wh_endpoint_id, WaterHeaterManagement::Id,
                              WaterHeaterManagement::Attributes::BoostState::Id, &v);
            prev_boost_active = s_boost_active;
        }

        if (cur.safety != prev.safety) {
            uint8_t pos = (uint8_t)cur.safety;
            if (pos >= SAFETY_POSITIONS) pos = 0;
            v = esp_matter_uint8(pos);
            attribute::update(s_safety_ep_id, Switch::Id,
                              Switch::Attributes::CurrentPosition::Id, &v);
            /* Event emission goes through the chip dispatcher — see
             * safety_switch_latched_worker above for the rationale. */
            chip::DeviceLayer::PlatformMgr().ScheduleWork(
                safety_switch_latched_worker, (intptr_t)pos);
            ESP_LOGW(TAG, "safety ← %u (%s)", pos, s_safety_label[pos]);
        }

        for (int i = 0; i < PROBE_EP_COUNT; ++i) {
            float cur_c, prev_c;
            bool cur_valid  = probe_value_c(cur.temp,  i, &cur_c);
            bool prev_valid = probe_value_c(prev.temp, i, &prev_c);

            bool changed = (cur_valid != prev_valid) ||
                           (cur_valid && fabsf(cur_c - prev_c) > 0.5f);
            if (!changed) continue;

            if (cur_valid) {
                int16_t cv = (int16_t)(cur_c * 100.0f);
                v = esp_matter_nullable_int16(cv);
            } else {
                v = esp_matter_nullable_int16(nullable<int16_t>());
            }
            attribute::update(s_probe_ep_id[i], TemperatureMeasurement::Id,
                              TemperatureMeasurement::Attributes::MeasuredValue::Id, &v);
        }

        prev = cur;
    }
}

/* --- Endpoint construction ----------------------------------------------- */
static esp_err_t build_water_heater(node_t *node)
{
    thermostat::config_t cfg;
    cfg.thermostat.local_temperature           = 2200;  /* 22 °C */
    cfg.thermostat.occupied_heating_setpoint   = 5000;  /* 50 °C */
    cfg.thermostat.system_mode                 = 0;     /* off */
    cfg.thermostat.control_sequence_of_operation = 2;   /* Heating only */

    s_wh_ep = thermostat::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
    if (!s_wh_ep) return ESP_FAIL;
    s_wh_endpoint_id = endpoint::get_id(s_wh_ep);

    /* OnOff cluster (master power) */
    on_off::config_t onoff_cfg;
    onoff_cfg.on_off = false;
    on_off::create(s_wh_ep, &onoff_cfg, CLUSTER_FLAG_SERVER, 0);

    /* TemperatureMeasurement (mirror of LocalTemperature) */
    temperature_measurement::config_t tm_cfg;
    tm_cfg.temperature_measurement.measured_value     = 2200;
    tm_cfg.temperature_measurement.min_measured_value = 0;
    tm_cfg.temperature_measurement.max_measured_value = 10000;
    temperature_measurement::create(s_wh_ep, &tm_cfg, CLUSTER_FLAG_SERVER);

    /* Tighten heating limits — Matter int16 in centi-°C */
    cluster_t *therm = cluster::get(s_wh_ep, Thermostat::Id);
    if (therm) {
        esp_matter_attr_val_t lo = esp_matter_int16(4000);
        esp_matter_attr_val_t hi = esp_matter_int16(8000);
        attribute::set_default_value(
            attribute::get(therm, Thermostat::Attributes::AbsMinHeatSetpointLimit::Id), &lo);
        attribute::set_default_value(
            attribute::get(therm, Thermostat::Attributes::AbsMaxHeatSetpointLimit::Id), &hi);
        attribute::set_default_value(
            attribute::get(therm, Thermostat::Attributes::MinHeatSetpointLimit::Id), &lo);
        attribute::set_default_value(
            attribute::get(therm, Thermostat::Attributes::MaxHeatSetpointLimit::Id), &hi);
    }

    /* WaterHeaterMode — the four heating modes (CurrentMode 1..4) */
    water_heater_mode::config_t mode_cfg;
    mode_cfg.current_mode = matter_mode_from_carviston(HEATING_MODE_OPTIMAL);
    cluster_t *mode_cl = water_heater_mode::create(s_wh_ep, &mode_cfg, CLUSTER_FLAG_SERVER);
    if (!mode_cl) return ESP_FAIL;

    /* SupportedModes — published as a static list of ModeOptionStruct on the
     * cluster. Each entry: { Label, Mode, ModeTags[] }. esp-matter's helper
     * api accepts a constexpr list at cluster creation; if the SDK signature
     * differs slightly, drop these into the right field per
     * components/esp_matter/cluster/water_heater_mode.h in your installed
     * esp-matter checkout — the values themselves are the contract. */
    static const ModeBase::Structs::ModeTagStruct::Type tags_super[] = {
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kManual },
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kMax },
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kQuick },
    };
    static const ModeBase::Structs::ModeTagStruct::Type tags_fast[] = {
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kManual },
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kQuick },
    };
    static const ModeBase::Structs::ModeTagStruct::Type tags_optimal[] = {
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kAuto },
    };
    static const ModeBase::Structs::ModeTagStruct::Type tags_eco[] = {
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kAuto },
        { chip::Optional<chip::VendorId>{}, ModeBase::ModeTag::kLowEnergy },
    };
    static const ModeBase::Structs::ModeOptionStruct::Type supported_modes[] = {
        { chip::CharSpan::fromCharString("Super-fast"), 1, chip::Span<const ModeBase::Structs::ModeTagStruct::Type>(tags_super)   },
        { chip::CharSpan::fromCharString("Fast"),       2, chip::Span<const ModeBase::Structs::ModeTagStruct::Type>(tags_fast)    },
        { chip::CharSpan::fromCharString("Optimal"),    3, chip::Span<const ModeBase::Structs::ModeTagStruct::Type>(tags_optimal) },
        { chip::CharSpan::fromCharString("ECO"),        4, chip::Span<const ModeBase::Structs::ModeTagStruct::Type>(tags_eco)     },
    };
    water_heater_mode::set_supported_modes(mode_cl, supported_modes,
                                           sizeof(supported_modes) / sizeof(supported_modes[0]));

    /* WaterHeaterManagement — HeatDemand bitmap + BoostState enum.
     * Optional attributes (TankVolume / TankPercentage / EstimatedHeatRequired)
     * are not exposed; we don't know the tank volume reliably for the OEM
     * boiler and the heater_control state machine doesn't model fill level. */
    water_heater_management::config_t mgmt_cfg;
    mgmt_cfg.heat_demand = 0;
    mgmt_cfg.boost_state = 0;  /* Inactive */
    if (!water_heater_management::create(s_wh_ep, &mgmt_cfg, CLUSTER_FLAG_SERVER)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* --- Probe endpoints (EP 2-5) -------------------------------------------- */
static esp_err_t build_probe_endpoints(node_t *node)
{
    for (int i = 0; i < PROBE_EP_COUNT; ++i) {
        temperature_sensor::config_t cfg;
        /* Nullable int16, centi-°C. Sized for our 10 kΩ NTC range. */
        cfg.temperature_measurement.measured_value     = 2200;
        cfg.temperature_measurement.min_measured_value = -4000;   /* -40 °C */
        cfg.temperature_measurement.max_measured_value = 12500;   /* 125 °C */

        endpoint_t *ep = temperature_sensor::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
        if (!ep) {
            ESP_LOGE(TAG, "probe EP[%d] create failed", i);
            return ESP_FAIL;
        }
        s_probe_ep[i]    = ep;
        s_probe_ep_id[i] = endpoint::get_id(ep);

        /* Attach a Fixed Label cluster so controllers display a meaningful
         * name. The list-of-struct attribute is set after cluster creation
         * via esp-matter's helper, which serialises CharSpan pairs.
         * If the helper signature differs in your installed esp-matter,
         * fall back to attribute::set_default_value() on the LabelList
         * attribute with a manually built TLV list. */
        fixed_label::config_t flcfg;
        cluster_t *flcl = fixed_label::create(ep, &flcfg, CLUSTER_FLAG_SERVER);
        if (flcl) {
            FixedLabel::Structs::LabelStruct::Type labels[1] = {
                {
                    chip::CharSpan::fromCharString("name"),
                    chip::CharSpan::fromCharString(s_probe_meta[i].label),
                },
            };
            fixed_label::set_label_list(flcl, labels, 1);
        }

        ESP_LOGI(TAG, "probe EP[%d] id=%u  %s", i, s_probe_ep_id[i], s_probe_meta[i].label);
    }
    return ESP_OK;
}

/* --- Safety endpoint (EP 6) ---------------------------------------------- */
static esp_err_t build_safety_endpoint(node_t *node)
{
    generic_switch::config_t cfg;
    cfg.switch_cluster.number_of_positions = SAFETY_POSITIONS;
    cfg.switch_cluster.current_position    = 0;       /* OK at boot */
    cfg.switch_cluster.multi_press_max     = 0;       /* unused for latching */

    s_safety_ep = generic_switch::create(node, &cfg, ENDPOINT_FLAG_NONE, nullptr);
    if (!s_safety_ep) return ESP_FAIL;
    s_safety_ep_id = endpoint::get_id(s_safety_ep);

    /* Switch cluster default is momentary; flip to latching (LS = bit 0). */
    cluster_t *sw = cluster::get(s_safety_ep, Switch::Id);
    if (sw) cluster::set_feature_map(sw, 0x01);

    /* Fixed Label so phones display "Safety status" instead of "Switch 1". */
    fixed_label::config_t flcfg;
    cluster_t *flcl = fixed_label::create(s_safety_ep, &flcfg, CLUSTER_FLAG_SERVER);
    if (flcl) {
        FixedLabel::Structs::LabelStruct::Type labels[1] = {
            { chip::CharSpan::fromCharString("name"),
              chip::CharSpan::fromCharString("Safety status") },
        };
        fixed_label::set_label_list(flcl, labels, 1);
    }

    ESP_LOGI(TAG, "safety EP id=%u (%u-position latching)",
             s_safety_ep_id, SAFETY_POSITIONS);
    return ESP_OK;
}

/* --- Public entrypoint --------------------------------------------------- */
extern "C" esp_err_t matter_node_start(void)
{
    if (!s_pairing_cache_lock) {
        s_pairing_cache_lock = xSemaphoreCreateMutex();
        if (!s_pairing_cache_lock) return ESP_ERR_NO_MEM;
    }

    node::config_t node_cfg;
    s_node = node::create(&node_cfg, attribute_update_cb, identification_cb);
    if (!s_node) {
        ESP_LOGE(TAG, "node::create failed");
        return ESP_FAIL;
    }

    esp_err_t err = build_water_heater(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build_water_heater failed: %s", esp_err_to_name(err));
        return err;
    }

    err = build_probe_endpoints(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build_probe_endpoints failed: %s", esp_err_to_name(err));
        return err;
    }

    err = build_safety_endpoint(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "build_safety_endpoint failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_matter::start(nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(sync_task, "matter_sync", 4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "sync_task create failed");
        return ESP_ERR_NO_MEM;
    }

    /* Publish *after* every chip-side handle exists. Heater_control polls
     * matter_node_get_pairing_info() each tick and would otherwise touch an
     * uninitialised Server singleton. */
    s_matter_started = true;
    ESP_LOGI(TAG, "Matter water-heater node up (EP %u)", s_wh_endpoint_id);
    return ESP_OK;
}

/* --- Commissioning window (phase 6) ------------------------------------- */

#include <app/server/Server.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <platform/CHIPDeviceLayer.h>

/* Pairing strings are constant for the lifetime of a commissioning window
 * (they're derived from factory data, not from the window's countdown).
 * We cache them at open-time and serve from cache, so the heater task's
 * per-tick matter_node_get_pairing_info() is a fast lock-take-and-copy
 * instead of rebuilding a SetupPayload + Base38 encoding every second. */
typedef struct {
    bool active;
    char qr[64];
    char manual[12];
} pairing_cache_t;

static pairing_cache_t   s_pairing_cache  = { false, {0}, {0} };
static SemaphoreHandle_t s_pairing_cache_lock = nullptr;

/* Window deadline for window_s_remaining computation. chip's
 * CommissioningWindowManager keeps timeout state private, so we record
 * the window's expected close time here when opening it. */
static int64_t  s_window_close_us = 0;
static uint32_t s_window_total_s  = 0;

static bool rebuild_pairing_strings(char *qr_out, size_t qr_n,
                                    char *manual_out, size_t manual_n)
{
    using namespace chip;
    SetupPayload payload;
    auto &mgr = DeviceLayer::ConfigurationMgr();
    uint16_t discriminator = 0;
    uint32_t passcode      = 0;
    uint16_t vendor_id     = 0;
    uint16_t product_id    = 0;
    (void)mgr.GetSetupDiscriminator(discriminator);
    (void)mgr.GetSetupPasscode(passcode);
    (void)mgr.GetVendorId(vendor_id);
    (void)mgr.GetProductId(product_id);

    payload.version           = 0;
    payload.vendorID          = vendor_id;
    payload.productID         = product_id;
    payload.commissioningFlow = CommissioningFlow::kStandard;
    payload.rendezvousInformation.SetValue(RendezvousInformationFlag::kOnNetwork);
    if (chip::DeviceLayer::ConnectivityMgr().IsBLEAdvertisingEnabled()) {
        payload.rendezvousInformation.SetValue(RendezvousInformationFlag::kBLE);
    }
    payload.discriminator.SetLongValue(discriminator);
    payload.setUpPINCode = passcode;

    bool ok = true;
    {
        std::string s;
        if (ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(s) == CHIP_NO_ERROR) {
            strncpy(manual_out, s.c_str(), manual_n - 1);
            manual_out[manual_n - 1] = '\0';
        } else {
            ok = false;
        }
    }
    {
        std::string s;
        if (QRCodeSetupPayloadGenerator(payload).payloadBase38Representation(s) == CHIP_NO_ERROR) {
            strncpy(qr_out, s.c_str(), qr_n - 1);
            qr_out[qr_n - 1] = '\0';
        } else {
            ok = false;
        }
    }
    return ok;
}

static void refresh_pairing_cache(void)
{
    char qr[sizeof(s_pairing_cache.qr)];
    char manual[sizeof(s_pairing_cache.manual)];
    memset(qr, 0, sizeof(qr));
    memset(manual, 0, sizeof(manual));
    bool ok = rebuild_pairing_strings(qr, sizeof(qr), manual, sizeof(manual));

    xSemaphoreTake(s_pairing_cache_lock, portMAX_DELAY);
    s_pairing_cache.active = ok;
    memcpy(s_pairing_cache.qr,     qr,     sizeof(s_pairing_cache.qr));
    memcpy(s_pairing_cache.manual, manual, sizeof(s_pairing_cache.manual));
    xSemaphoreGive(s_pairing_cache_lock);
}

static void invalidate_pairing_cache(void)
{
    xSemaphoreTake(s_pairing_cache_lock, portMAX_DELAY);
    s_pairing_cache.active = false;
    memset(s_pairing_cache.qr,     0, sizeof(s_pairing_cache.qr));
    memset(s_pairing_cache.manual, 0, sizeof(s_pairing_cache.manual));
    xSemaphoreGive(s_pairing_cache_lock);
}

extern "C" esp_err_t matter_node_open_pairing(uint32_t window_s)
{
    using namespace chip;
    if (!s_matter_started) return ESP_ERR_INVALID_STATE;
    if (window_s < 180) window_s = 180;
    if (window_s > 900) window_s = 900;

    auto &cwm = Server::GetInstance().GetCommissioningWindowManager();
    CHIP_ERROR err = cwm.OpenBasicCommissioningWindow(
        System::Clock::Seconds16(static_cast<uint16_t>(window_s)));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "OpenBasicCommissioningWindow failed: %s", ErrorStr(err));
        return ESP_FAIL;
    }
    s_window_total_s  = window_s;
    s_window_close_us = esp_timer_get_time() + (int64_t)window_s * 1000000LL;
    /* Populate cache once per window — pairing strings are constant for the
     * factory-data values until next boot. */
    refresh_pairing_cache();
    ESP_LOGI(TAG, "Commissioning window opened (%lu s)", (unsigned long)window_s);
    return ESP_OK;
}

extern "C" esp_err_t matter_node_get_pairing_info(matter_pairing_info_t *out)
{
    using namespace chip;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (!s_matter_started) return ESP_OK;  /* leaves out->active = false */

    bool is_open = Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();

    if (!is_open) {
        /* Window closed (timed out or pairing completed) — drop the cache
         * so a subsequent open re-derives fresh strings. */
        if (s_pairing_cache.active) invalidate_pairing_cache();
        s_window_close_us = 0;
        s_window_total_s  = 0;
        return ESP_OK;
    }

    /* Cache may be empty if commissioning auto-opened at boot (esp-matter
     * does this on first-ever boot before any open_pairing call). Fill it
     * lazily on first observation. */
    xSemaphoreTake(s_pairing_cache_lock, portMAX_DELAY);
    bool cache_ready = s_pairing_cache.active;
    xSemaphoreGive(s_pairing_cache_lock);
    if (!cache_ready) refresh_pairing_cache();

    xSemaphoreTake(s_pairing_cache_lock, portMAX_DELAY);
    out->active = s_pairing_cache.active;
    memcpy(out->qr,     s_pairing_cache.qr,     sizeof(out->qr));
    memcpy(out->manual, s_pairing_cache.manual, sizeof(out->manual));
    xSemaphoreGive(s_pairing_cache_lock);

    if (s_window_close_us > 0) {
        int64_t left_us = s_window_close_us - esp_timer_get_time();
        out->window_s_remaining = left_us > 0 ? (uint32_t)(left_us / 1000000) : 0;
    } else if (out->active) {
        /* Window auto-opened at boot — close time unknown, fall back to
         * the chip-spec basic-commissioning default of 900 s. */
        out->window_s_remaining = 900;
    }
    return ESP_OK;
}

#else  /* CONFIG_CARVISTON_MATTER_ENABLE not defined */

extern "C" esp_err_t matter_node_start(void)
{
    ESP_LOGI(TAG, "Matter disabled at build time (CONFIG_CARVISTON_MATTER_ENABLE=n)");
    return ESP_OK;
}

extern "C" esp_err_t matter_node_open_pairing(uint32_t /*window_s*/)
{
    /* Surface as not-supported so the web handler can return 503 and the
     * Vue dashboard hides the Matter card on this build. */
    return ESP_ERR_NOT_SUPPORTED;
}

extern "C" esp_err_t matter_node_get_pairing_info(matter_pairing_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    return ESP_OK;
}

#endif
