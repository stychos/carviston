# Carviston controller — Bill of Materials

Replacement logic board for Ariston VLS Evo 100 EU storage water heater.
Rev B — 2026-05-26.

## 1. Active components

| Ref   | Qty | Part                                   | Notes |
|-------|-----|----------------------------------------|-------|
| U3    | 1   | **ESP32-S3-N16R8** dev module          | 16 MB flash, 8 MB octal PSRAM. WROOM-1 or compatible. |
| U4    | 1   | **ADS1115** I²C ADC breakout            | 16-bit Σ-Δ, addr 0x48 (ADDR→GND). Many breakouts already include 4.7 kΩ pull-ups — check before adding R5/R6. |
| U5    | 1   | **2-channel opto-isolated relay module** | SongLe SRD-05VDC-SL-C based (or equivalent), 5 V coil, active-LOW IN, on-board NPN drivers + flyback diodes + optocouplers. JD-VCC jumper kept installed (VCC↔JD-VCC). |

## 2. Power supply

| Ref   | Qty | Part                       | Notes |
|-------|-----|----------------------------|-------|
| U1    | 1   | **HLK-15M05C** AC→5 V SMPS  | Single 15 W / 3 A AC→5 V brick — covers the relay coils + elements' control draw with margin. **+3V3 is not generated here:** +5 V feeds the ESP32-S3 board's 5V-in and its onboard LDO supplies +3V3 to the logic / ADS1115 / NTC dividers. |
| C1    | 1   | 470 µF / 16 V electrolytic | Bulk decoupling on +5 V rail. |
| C3-C5 | 3   | 100 nF / 50 V ceramic       | Local decoupling near ESP32, ADS1115, relay module. |

## 3. Sensors

| Ref     | Qty | Part                         | Notes |
|---------|-----|------------------------------|-------|
| —       | 2   | Ariston **dual-NTC immersion probe** | OEM, one per tank (inlet, outlet). red = common (GND), black = NTC reg, yellow = NTC safety; the two NTCs in a tank are cross-checked against each other. ~10 kΩ @ 25 °C, β TBD (configured at runtime via NVS). |
| R1-R4   | 4   | 10 kΩ 1 % 0.25 W             | Divider tops: 3V3 → 10 kΩ → AINx → NTC → GND. |
| RT1-RT4 | 4   | NTC thermistors              | Inside the two probes above (×2 NTCs per probe). |
| R5, R6  | 2   | 4.7 kΩ 0.25 W                | I²C SDA/SCL pull-ups to +3V3. **Skip if your ADS1115 breakout already has them**. |

## 4. Safety

No hardware thermal cutoff. Over-temp protection is firmware-only: the soft
limit in `safety.c` (90 °C) drops both relays when any tank crosses it. IO21 is
free (was the cutoff-sense input).

## 5. User interface

| Ref      | Qty | Part                                  | Notes |
|----------|-----|---------------------------------------|-------|
| D3-D10   | 8   | 5 mm LED                              | POWER, TEMP 40/50/60/70/80 °C, SHOWER-READY, ECO. Pick colour to taste. |
| R9-R16   | 8   | 330 Ω 0.25 W                          | LED current limit, sized for ~10 mA from 3.3 V. |
| SW2-SW6  | 5   | **Capacitive touch sensor** (active-HIGH) | POWER, ECO, SHOWER, PLUS, MINUS. Output goes HIGH when touched; the MCU's internal pull-down holds the pin LOW when idle (no external resistor). PLUS→IO41, MINUS→IO40 (swapped to match the as-built wiring). |

## 6. Loads

| Ref      | Qty | Part                                  | Notes |
|----------|-----|---------------------------------------|-------|
| HE1, HE2 | 2   | AC heating elements (existing tank)   | Fused independently. |

## 7. Mechanical / wiring

- Enclosure (replacing OEM logic-board housing)
- Mains-rated terminal blocks, ferrules, heat-shrink
- Ribbon / DuPont harness for front-panel I/O (8 LEDs + 5 buttons + GND)
- Internal jumper wires (DuPont) between MCU board ↔ ADS1115 ↔ U5
- Mounting standoffs / nylon spacers

## What changed from Rev A

- ❌ Removed `Q1`, `Q2` (2N2222 NPN drivers)
- ❌ Removed `RB1`, `RB2` (1 kΩ base resistors)
- ❌ Removed `D1`, `D2` (1N4148 flyback diodes)
- ❌ Removed `K1`, `K2` (discrete relays)
- ✅ Added `U5` (2-channel optocoupler relay module — module integrates all of the above)
- ⚙ Firmware: `relays.c` now drives active-LOW (`0 = ON`, `1 = OFF`), with a defensive pull-up + pre-config level set so the line never glitches LOW during boot

## Notes on U5 wiring

```
   +5V ──┬─── JD-VCC  (coil supply)
         └─── VCC      (logic supply, jumpered to JD-VCC)

   IN1  ← ESP32 IO47   (active-LOW)
   IN2  ← ESP32 IO42   (active-LOW)
   GND  → system GND   (single point at SMPS)

   K1 COM → AC L      K1 NO → HE1 hot side  (HE1 cold → AC N)
   K2 COM → AC L      K2 NO → HE2 hot side  (HE2 cold → AC N)
```

**Keep the on-board VCC↔JD-VCC jumper installed.** If you ever want true
isolation between the MCU and coil supplies, remove the jumper and feed
`JD-VCC` from a separate (KSD-protected) 5 V source; only GND will then be
shared with the logic side via the opto LEDs.

**3.3 V vs 5 V on IN pins.** The opto LED + series-R on the module are
sized for 5 V (~3.8 mA). With 3.3 V (direct from ESP32) the current drops
to ~2 mA — typically enough for PC817-class optos but marginal on cheap
clones. If switching is flaky, lower the on-board series resistor (R1/R2
*on the module*) or buffer the IN pins through an open-drain stage driven
from 5 V.
