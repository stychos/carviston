# Carviston

A custom open-source controller that replaces the failed OEM logic board on an
**Ariston VLS Evo 100 EU** storage water heater. Built on an **ESP32-S3-N16R8**,
it drives the original sensors and heating elements. Replicates indicator LEDs and capacitive
buttons, and adds a self-hosted web UI for configuration, scheduling, OTA
updates and live monitoring.

> ⚠️ This is a personal repair project shared as-is — build and run it at your own risk!

---

## Features

- **Dual-tank regulation** — two dual-NTC immersion probes (inlet + outlet),
  each tank regulated independently. A tank whose two NTCs disagree, read
  open/short, or exceed the over-temp limit is dropped on its own; the healthy
  tank keeps heating.
- **Redundant temperature sensing** — 4 NTCs read through an external 16-bit
  ADS1115 ADC (the ESP32 internal ADC suffered WiFi-induced noise), with a
  trimmed-mean filter and per-probe cross-checking.
- **Staggered relay control** — two AC elements switched via an opto-isolated
  relay module with an enforced 300 ms turn-on stagger (never inrush both).
- **Replicated front panel** — 8 indicator LEDs, 5 capacitive buttons (On/Off, ECO,
  Shower-Ready, +, −) reproducing the original UI, including mode-cycle and
  target-preview animations.
- **Self-hosted web UI** — Vue 3 + PrimeVue dashboard served straight from
  flash. Boiler config, heating scheduler, Wi-Fi, logs, maintenance, and live
  state over WebSocket (with polling fallback).
- **OTA updates** — A/B firmwazre partitions, the UI is embedded in the image
  so an OTA always carries its matching frontend and rolls back with it.
- **Append-only config migration** — settings survive firmware upgrades *and*
  rollbacks; a version-independent preserve store keeps Wi-Fi + auth across even
  an incompatible reset (anti-lockout).
- **Auditability** — event log and heating-session benchmarks, persisted to NVS,
  wall-clock stamped once SNTP syncs.

---

## Hardware

Target board: **ESP32-S3-N16R8** (16 MB flash, 8 MB PSRAM). Full parts list in
[`docs/BOM.md`](docs/BOM.md); schematic and pinout in [`docs/`](docs/).

| Block | Part | Notes |
|-------|------|-------|
| MCU | ESP32-S3-N16R8 | WROOM-1 or compatible dev module |
| ADC | ADS1115 @ `0x48` | 16-bit Σ-Δ over I²C, AIN0..3 = the 4 NTCs |
| Relays | 2-ch opto-isolated module | SongLe SRD-05VDC-SL-C, active-LOW IN, on-board drivers + flyback |
| PSU | HLK-15M05C | AC→5 V; board LDO derives 3V3 |
| Sensors | 2× Ariston dual-NTC probes | red=GND common, black=regulation NTC, yellow=safety NTC |

### Pin map

```
I²C bus     SDA=GPIO6  SCL=GPIO7        (4.7 kΩ external pull-ups required)
  ADS1115   AIN0=inlet reg   AIN1=inlet safety
            AIN2=outlet reg  AIN3=outlet safety
LEDs        GPIO8=POWER  GPIO9..13=TEMP40..80  GPIO14=SHOWER  GPIO15=ECO
Buttons     GPIO16=POWER GPIO17=ECO GPIO18=SHOWER GPIO41=PLUS GPIO40=MINUS
Relays      IN1=GPIO47   IN2=GPIO42   (active-LOW)
```

NTC divider topology: `3V3 → 10 kΩ → AINx → NTC → GND`. R25 + Beta are stored in
NVS and tuned at runtime.

---

## Firmware architecture (`main/`)

A 1 Hz `heater_control` tick is the heartbeat: read temperature → evaluate
safety → drain button events → run the mode state machine → command relays →
push panel state to the LEDs → update logs and benchmarks.

| Module | Responsibility |
|--------|----------------|
| `app_config` | Single mutex-guarded struct, NVS-backed, append-only with cross-version migration |
| `temperature` | ADS1115 single-shot reads, trimmed mean, Beta equation, per-tank cross-check |
| `safety` | Over-temp + total-sensor-loss aggregator; latches but auto-recovers |
| `heater_control` | Mode state machine, ties everything together each tick |
| `relays` | 300 ms staggered turn-on, glitch-free boot, immediate turn-off |
| `leds` / `buttons` | 10 Hz render task / 20 ms debounced input events |
| `wifi_mgr` | AP / STA / HYBRID, first-boot AP, mDNS `carviston.local`, SNTP |
| `auth` | PBKDF2-SHA256 (PSA crypto), 8-slot RAM session tokens |
| `web_server` | `esp_http_server` REST + WebSocket `/ws`, SPA fallback |
| `web_assets` | Parses the firmware-embedded web archive into a path→bytes table |
| `ota` | Firmware `.bin` → inactive partition → verify → set-boot → reboot |
| `event_log` / `benchmark` | NVS-persisted ring buffer / heating sessions |

---

## Safety rules

- Soft over-temp limit (90 °C) is the sole over-temp protection. Keep it conservative
  and add a hardware cutoff if you have any around.
- Soft over-temp is applied **per tank**: a tank with either NTC ≥ 90 °C is
  faulted immediately (never debounced) and its element dropped, while the other
  tank keeps heating; it auto-recovers once it cools.

---

## Frontend (`web/`)

Vue 3 + Vite + PrimeVue (Aura dark preset). One module = one component;
composables for `api`, `auth` and `liveState`. The build is invoked by CMake on
every `idf.py build`, packed by [`tools/pack-web.mjs`](tools/pack-web.mjs), and
embedded directly into the firmware image — there is **no SPIFFS web
partition**. Requires Node.js on `PATH`.

For frontend-only iteration:

```bash
cd web && npm install && npm run dev
```

---

## Build & flash

Uses **ESP-IDF v6.0.1**.

```bash
source ~/.espressif/tools/activate_idf_v6.0.1.sh   # activate per-shell
idf.py build                                        # builds firmware + Vue UI
idf.py flash monitor
```

`idf.py build` compiles the firmware, builds the Vue frontend, packs it into one
archive and embeds it in the app binary. `idf.py flash` writes everything.

### Factory flash

Boot as if never configured (clears app_config + Wi-Fi):

```bash
./tools/flash-fresh.sh -p /dev/cu.usbserial-XXXX monitor
```

---

## First-boot behavior

- Forces **AP mode**, AP SSID is derived from the MAC: `carviston-xxxxxx`.
  Open AP until an `ap_pass` is set.
- Web UI redirects to a **setup-password** flow.
- Master state is **Off** — the front panel stays dark until you press POWER
  (don't run heaters before the user acknowledges).
- Once on the network, reachable via mDNS.

---

## Partition table (16 MB)

`nvs` / `otadata` / `phy_init` / **`ota_0` (4 MB)** / **`ota_1` (4 MB)** /
`storage`. The Vue bundle (~0.35 MB packed) is embedded in the app image, so OTA
slots are sized for comfortable growth. ~7.8 MB at the tail is intentionally
unallocated. See [`partitions.csv`](partitions.csv).

---

## Repository layout

```
main/            firmware modules (C, ESP-IDF)
web/             Vue 3 + Vite + PrimeVue frontend
tools/           pack-web.mjs (UI embed), flash-fresh.sh
docs/            BOM, schematic, pinout
partitions.csv   16 MB partition table
CLAUDE.md        detailed architecture & conventions
```
