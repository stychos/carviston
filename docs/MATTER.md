# Matter integration

Carviston exposes itself to Matter as a **Water Heater** (Matter ≥ 1.3 device
type `0x050F`). Phase 2 wires the three core clusters; later phases add the
heater-mode/management clusters, per-probe sensors, and fault events.

## Status

| Phase | Scope | State |
|-------|-------|-------|
| 1 | Partition table reshape (3 MB → **4 MB** OTA slots; web bundle was SPIFFS, since embedded in-firmware — no web partition) | done |
| 2 | Build flag, matter_node skeleton, OnOff + Thermostat + TempMeas bridge to heater_control | done |
| 3 | WaterHeaterMode + WaterHeaterManagement clusters | done |
| 4 | Probe-sensor endpoints (EP 2-5) | done |
| 5 | Generic Switch fault events (EP 6) | done |
| 6 | Commissioning UX (long-press POWER+ECO, LED state, web-UI QR) | done (firmware), Vue card is the only pending bit |

Matter is **ON by default** as of this build. The integration adds ~1.8 MB
to the binary and the first-time build pulls ~500 MB of chip-project source
through the managed-component system. To turn Matter off (and strip BLE
with it) for a strictly-local build, set `CARVISTON_MATTER_ENABLE=n` in
`menuconfig`.

## Build prerequisites

1. **Install esp-matter** alongside ESP-IDF (one-time):

   ```sh
   cd ~/.espressif
   git clone --depth 1 https://github.com/espressif/esp-matter.git
   cd esp-matter
   git submodule update --init --depth 1
   ./install.sh
   ```

   Then in every shell where you build Carviston:

   ```sh
   source ~/.espressif/v6.0.1/esp-idf/export.sh
   source ~/.espressif/esp-matter/export.sh
   ```

   `main/idf_component.yml` already declares `espressif/esp_matter ^1.4.0`,
   so `idf.py build` pulls the component on first build. The Kconfig
   bundle (`CONFIG_BT_ENABLED`, `CONFIG_BT_NIMBLE_ENABLED`,
   `CONFIG_ENABLE_CHIPOBLE`, `CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING`,
   `CONFIG_FACTORY_PARTITION_NAME`, `CONFIG_USE_MINIMAL_MDNS=n`) is already
   in `sdkconfig.defaults`.

2. **Generate factory data** (Vendor ID, Product ID, DAC, CD):

   ```sh
   # Use Espressif's helper from esp-matter/tools/mfg_tool:
   esp-matter-mfg-tool \
     --vendor-id 0xFFF1 \
     --product-id 0x8001 \
     --vendor-name "Carviston" \
     --product-name "Carviston Boiler Controller" \
     --hw-ver 1 --hw-ver-str "Rev C" \
     --serial-num CV-001 \
     --discriminator 1234 \
     --passcode 20202021 \
     --pai ~/.espressif/esp-matter/credentials/test/pai/Chip-Test-PAI-FFF1-8000-Cert.pem \
     --pai-key ~/.espressif/esp-matter/credentials/test/pai/Chip-Test-PAI-FFF1-8000-Key.pem \
     --cd ~/.espressif/esp-matter/credentials/test/cd/Chip-Test-CD-FFF1-8001.der
   ```

   The tool emits a `<UUID>-partition.bin` (~16 KB) and a QR/manual code in
   the console. Flash the partition to `matter_fac`:

   ```sh
   esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x830000 <UUID>-partition.bin
   # (0x830000 = matter_fac offset from partitions.csv)
   ```

   (Offset is the `matter_fac` partition offset from `partitions.csv`.)

3. **Build & flash** as usual:

   ```sh
   idf.py build
   idf.py -p /dev/cu.usbserial-XXXX flash monitor
   ```

   First build takes ~10-15 min because esp-matter compiles the chip stack.
   Subsequent incremental builds are ~30 s.

## Cluster map (phases 2 + 3)

The water-heater endpoint (EP 1) currently exposes:

| Cluster                            | Attribute / Command                          | Backing                                                |
|------------------------------------|----------------------------------------------|--------------------------------------------------------|
| OnOff (`0x0006`)                   | `OnOff` (RW), `On`/`Off`/`Toggle`            | `heater_set_master_enabled` / `master_enabled`         |
| Thermostat (`0x0201`)              | `LocalTemperature` (R)                       | `temp.water_c` (×100, int16)                           |
|                                    | `OccupiedHeatingSetpoint` (RW)               | `heater_set_target` (snapped to 10 °C step)            |
|                                    | `SystemMode` Off/Heat (RW)                   | `heater_set_master_enabled`                            |
|                                    | `Abs/Min/Max HeatSetpointLimit` = 4000/8000  | constants (40 / 80 °C)                                 |
|                                    | `ControlSequenceOfOperation` = HeatingOnly   | constant                                               |
| TemperatureMeasurement (`0x0402`)  | `MeasuredValue`                              | mirror of `LocalTemperature`                           |
| **WaterHeaterMode** (`0x009E`)     | `CurrentMode` (RW) + `ChangeToMode` cmd      | `heater_set_mode` / `state.mode`                       |
|                                    | `SupportedModes` (R)                         | static list of 4 modes (below)                         |
| **WaterHeaterManagement** (`0x0094`) | `HeatDemand` bitmap8 (R)                   | `state.heater_active[]` → bit 0 / bit 1                |
|                                    | `BoostState` (R, RW via Boost/CancelBoost)   | tracked in `s_boost_active`; boost forces SUPER_FAST   |
|                                    | `Boost(BoostInfo)` / `CancelBoost`           | route through `BoostState` attribute write             |

`SupportedModes` published to controllers:

| CurrentMode | Label        | Carviston enum             | Matter ModeTags             |
|-------------|--------------|----------------------------|-----------------------------|
| 1           | Super-fast   | `HEATING_MODE_SUPER_FAST`  | Manual, Max, Quick          |
| 2           | Fast         | `HEATING_MODE_FAST`        | Manual, Quick               |
| 3           | Optimal      | `HEATING_MODE_OPTIMAL`     | Auto (default)              |
| 4           | ECO          | `HEATING_MODE_ECO`         | Auto, LowEnergy             |

Writes from the controller hit `heater_set_*()` via the
`attribute_update_cb`. Reads are pushed by a 1 Hz sync task that polls
`heater_get_state()` and only forwards diffs to keep fabric chatter low.

### Safety endpoint (EP 6)

A `GenericSwitch` (`0x000F`) latching switch with **3 positions** mapping
one-to-one onto `safety_status_t`:

| CurrentPosition | safety_status_t            | Meaning                                           |
|-----------------|----------------------------|---------------------------------------------------|
| 0               | `SAFETY_OK`                | Heater is normal                                  |
| 1               | `SAFETY_FAULT_OVERTEMP`    | A tank exceeded the 90 °C firmware soft limit     |
| 2               | `SAFETY_FAULT_NO_PROBES`   | Every tank is faulted, no valid reading           |

A single tank's probe failing is handled per-tank (the healthy tank keeps
heating) and shows up as that tank's `TemperatureSensor` endpoint going null,
not as a safety position. Faults auto-recover once the condition clears.

The cluster is configured with `FeatureMap = 0x01` (LatchingSwitch) so a
`SwitchLatched` event fires on every transition. Apple Home / Google Home
let users wire these events into automations:

- *"If Carviston safety latches to anything except 0 → push notification."*
- *"If safety position is 1 (over-temp) for more than 30 min → email me."*

A Fixed Label cluster (`name = "Safety status"`) keeps the endpoint legible
in app UIs. The current latched position is mirrored in `CurrentPosition`
for controllers that prefer to poll rather than subscribe to events.

### Probe sensors (EP 2-5)

Four read-only `TemperatureSensor` (`0x0302`) endpoints sit alongside the
main water-heater endpoint, one per NTC:

| Endpoint | Fixed Label name      | Source field in `temperature_reading_t`           |
|----------|-----------------------|--------------------------------------------------|
| EP 2     | `Probe A regulation`  | `temp.probe[0].regulation_c`                     |
| EP 3     | `Probe A safety`      | `temp.probe[0].safety_c`                         |
| EP 4     | `Probe B regulation`  | `temp.probe[1].regulation_c`                     |
| EP 5     | `Probe B safety`      | `temp.probe[1].safety_c`                         |

Each endpoint exposes a `TemperatureMeasurement::MeasuredValue` (nullable
int16, centi-°C, range −40 → 125 °C). The sync task pushes the value when
either (a) the fault/NaN state of the probe changes, or (b) the temperature
moves by more than 0.5 °C since the last push. Faulted/NaN probes are sent
as the Matter null marker so phones can render "—" rather than a fake zero.

Each endpoint also has a **Fixed Label** cluster (`0x0040`) carrying a
single `name` label with the row's text — Apple Home and Google Home pick
that up so the four sensors get readable names instead of "Sensor 1..4".

### Boost handling

When a controller invokes `Boost(BoostInfo)`, the chip layer writes
`BoostState=Active` and our attribute callback intercepts:

1. If `s_boost_active` is already true, ignore (idempotent).
2. Otherwise snapshot the current `heating_mode_t` into
   `s_mode_before_boost`, flip `s_boost_active=true`, and call
   `heater_set_mode(HEATING_MODE_SUPER_FAST)` (unless we're already there).

`CancelBoost` writes `BoostState=Inactive`; if `s_boost_active` is true
we clear it and restore `s_mode_before_boost`. The sync task publishes
`BoostState` from `s_boost_active` (not `shower_ready`) so a phone-side
read always reflects user intent and there's no read/write semantic
collision. `BoostInfo.Duration` is currently ignored — the heater holds
SUPER_FAST until the controller cancels or the user presses SHOWER on
the front panel. Adding a firmware-side boost timer is a small
follow-up if needed.

## Commissioning UX

Triggering the commissioning window:

- **Long-press POWER + ECO** simultaneously for ≥ 3 s (combo timer in the
  1 Hz `control_task` loop). The SHOWER LED begins a fast double-blink
  heartbeat (200 ms on / 100 ms gap / 200 ms on / 600 ms dark, 1 s period).
  ECO's solo long-press AP-mode action is suppressed while POWER is also
  held so the combo wins cleanly.
- Or POST `/api/matter/open` with optional body `{"window_s": 180}` (also
  available from `curl`, or wired into a button in the web UI).

While the window is open, GET `/api/matter/code` returns:

```json
{
  "active": true,
  "qr": "MT:<base38 payload>",
  "manual": "12345678901",
  "window_s_remaining": 0
}
```

`qr` is the chip-spec QR payload (Apple/Google Home will render it from
the string directly). `manual` is the 11-digit decimal pairing code for
typing into the controller app.

When the window closes (timeout, successful pairing, or new boot) the
`control_task` polls `matter_node_get_pairing_info()` once per second and
calls `leds_set_matter_pairing(false)` — SHOWER LED returns to normal.

## Endpoints quick reference

| Method | Path                  | Purpose                                                  |
|--------|-----------------------|----------------------------------------------------------|
| `GET`  | `/api/matter/code`    | Snapshot of pairing window: active, QR, manual code      |
| `POST` | `/api/matter/open`    | Open a basic commissioning window (`window_s` 180-900)   |

Both require auth (same Bearer token as the rest of the REST API).

## What doesn't change

- The existing web UI, REST API, OTA flow and front-panel buttons are
  unaffected. Matter is purely additive.
- All safety-critical state still lives in `heater_control` /
  `safety.c`. Matter is a UI surface, not a control path — a write from
  a phone goes through the same `heater_set_*()` calls as a web POST, which
  in turn defers to the 1 Hz control tick and respects safety latches.
