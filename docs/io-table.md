# Keg Washer I/O Table

> Pin assignments are authoritative from `KegConfig.h`. State/phase semantics live
> in `state-taxonomy.md` (the canonical model). This table is the hardware map.

## Digital Inputs

| Name | Pin | Description | Active | Debounced | Critical | Notes |
|------|-----|-------------|--------|-----------|----------|-------|
| `airOk` | DI6 | Air pressure switch | HIGH | Yes | Yes | compressed-air availability |
| `co2Ok` | DI7 | Keg/line pressure switch (**downstream** of `co2Out`) | HIGH | Yes | Yes | reads keg-side pressure, NOT CO₂ supply (supply unmonitored/assumed connected). Ends the PRESSURE charge on trip; never tripping before `purgeTimer` → `ERR_CO2_PRESSURE`. Not a readiness gate (always open at rest). Display label `PRES`, grey when open |
| `ESTOP` | DI8 | Emergency stop / safety chain | LOW=tripped | **No** | Yes | Negative-true SINKING input (internal 3.3V/10k pull-up). NC chain from GND through drive-OK contacts + E-stop → DI8: healthy(closed)=GND=`digitalRead` HIGH=OK; open(trip/break)=pull-up=`digitalRead` LOW=ACTIVE. Not debounced. FALLING-edge ISR + polled backstop → PackML `Abort`. |
| `largeKeg` | IO0 | Keg-size selector | HIGH | Yes | No | small (20 L) vs large (50 L); latched at cycle start |
| `cycleStart` | IO1 | START button | HIGH | Yes | No | PackML `Start`/`Reset` (overloaded) |
| `manualDrain` | IO2 | FULL-DRAIN latching illuminated switch | HIGH (latched) | Yes | No | latched ON = full-keg drain mode: DIRTY_DRAIN runs `fullDrainTimer` (mins, for spoiled/undersold kegs) instead of `dirtyDrainTimer`; captured at cycle start (`drainModeLatched`). In diag mode it drives the drain valve directly. Old park/silence role moved to the touch SILENCE button (COMPLETE) + `cmd/silence`; old DRAIN+START diag chord replaced by the INFO-page DIAG button |

## Analog Inputs

| Name | Pin | Description | Bits | Range | Filter | Notes |
|------|-----|-------------|------|-------|--------|-------|
| *(free)* | A9 | — | — | — | — | enclosure temp moved off-controller (self-regulating fan w/ own thermometer); A9 unused |
| `causticTemp` | A10 | Caustic solution temp | 12 | 0–100 °C | low-pass | ProSense ETS50N 4–20 mA across a 470 Ω shunt; wash gate ≥ 50 °C; heater target 60 °C |
| `waterOk` | A11 | Water pressure (threshold) | 12 | — | low-pass | analog pin read as a water-available gate |
| `causticLevelSensor` | A12 | Caustic reservoir level — NC float switch S↔G, sinking digital read (closed/HIGH = level OK; open = low/broken wire, fail-safe) | 12 | OK / LOW | debounced 50 ms | heating + readiness refused when LOW |

> **Was missing from the prior table:** `causticLevelSensor` (A12).

## Digital Outputs

| Name | Pin | Description | Active | Notes |
|------|-----|-------------|--------|-------|
| `co2Out` | IO3 | CO₂ solenoid | HIGH | SANI_RETURN push-gas + PRESSURE charge |
| `alarmOut` | IO4 | RED fault / E-stop stacklight | HIGH | fault / E-stop indicator |
| `readyLedOut` | IO5 | GREEN cycle-start / ready indicator | HIGH | solid in IDLE-READY; **breathes (PWM) in COMPLETE** ("swap kegs, START for next"); breathes alternating with red in HELD |
| `drainOut` | CCIO‑A0 | Drain valve | HIGH | **forced OFF during RETURN phases — chem never to drain** |
| `waterOut` | CCIO‑A1 | Water valve | HIGH | FLOW phases |
| `airOut` | CCIO‑A2 | Air valve | HIGH | DIRTY_DRAIN, PURGE, CAUSTIC_RETURN push-gas |
| `causticOut` | CCIO‑A3 | Caustic valve | HIGH | WASHING + CAUSTIC_RETURN |
| `pumpOut` | CCIO‑A4 | Recirc pump | HIGH | RECIRC phases; **forced OFF during RETURN (no dry run)** |
| `sanitizerOut` | CCIO‑A5 | Sanitizer valve | HIGH | SANITIZE + SANI_RETURN |
| `causticHeaterOut` | CCIO‑A6 | Caustic heater | HIGH | `heaterMode=fw` (default): firmware bang-bang during STARTING, level/overtemp interlocked. `heaterMode=ext`: a safety **PERMIT** in series with the ETS50N PNP thermostat (always-hot tank); permit = level OK + no E-stop + no sensor fault + not ABORTED + below overtemp (2 °C re-arm hysteresis) |
| `kegsDoneOut` | CCIO‑A7 | COMPLETE / swap-kegs signal | HIGH | high while `MACH_COMPLETE` for an external done-indicator; cleared on COMPLETE exit + all-stop (pin was freed when the fan moved off-controller) |

> **Was missing from the prior table:** `causticHeaterOut` (CCIO‑A6).

## PWM Outputs

None. The cabinet cooling fan (formerly `cabinFanPWM` on IO5, later CCIO‑A7) is now a
standalone self-regulating unit with its own thermometer — not driven by the controller.
**CCIO‑A7 is free.**

## Communication Ports

| Name | Connector | Description | Notes |
|------|-----------|-------------|-------|
| `CcioPort` | COM0 | CCIO‑8 expansion | the CCIO‑A0..A6 outputs above |
| `displayPort` | COM1 | gen4‑uLCD‑43DT (Diablo16) | **272×480 RGB565, raw SPE serial** (not the old 128×128 mono; not ViSi‑Genie) |

> **Corrected:** COM1 was described as "128×128 monochrome." It's the 272×480
> colour Diablo16 panel driven by raw serial graphics (see `kegwasher_display_serial`).

## Outputs active per PackML state / ISA‑88 phase

| State / Phase | Active outputs |
|---------------|----------------|
| STARTUP (heating) | `causticHeaterOut` |
| DIRTY_DRAIN | `airOut`, `drainOut` |
| DIRTY_RINSE | `waterOut`, `drainOut` |
| DIRTY_PURGE | `airOut`, `drainOut` |
| WASHING | `causticOut`, `pumpOut` |
| CAUSTIC_RETURN | `causticOut`, `airOut` *(drain/pump OFF)* |
| RINSING | `waterOut`, `drainOut` |
| RINSE_PURGE | `airOut`, `drainOut` |
| SANITIZE | `sanitizerOut`, `pumpOut` |
| SANI_RETURN | `sanitizerOut`, `co2Out` *(drain/pump OFF)* |
| PRESSURE | `co2Out` |
| STOPPING | evac outputs per `evacKind` (caustic+air \| sani+CO₂ \| air+drain) |
| FINISHED / ERROR | `alarmOut` |
| HALTED | none (all-stop, silent) |

## Error code → I/O

| Code | `ERR_*` | Related I/O |
|------|---------|-------------|
| 0 | NONE | — |
| 1 | SD_INIT | SD interface |
| 2 | CONFIG_FILE | SD interface |
| 3 | WATER_PRESSURE | `waterOk` |
| 4 | AIR_PRESSURE | `airOk` |
| 5 | CO2_PRESSURE | `co2Ok` — keg failed to reach pressure before `purgeTimer` (no CO₂ / unsealed keg / leak) |
| 6 | CAUSTIC_TEMP | `causticTemp` | *(retired 2026-07-04: low temp is warn-only — amber LOW TEMP banner + `warn/lowtemp` — never raised as a fault)*
| 7 | *(retired)* | was ENCLOSURE_TEMP — enclosure temp off-controller; code not reused |
| 8 | ESTOP | `ESTOP` |
| 9 | INVALID_STATE | — |
| 10 | HEATING_TIMEOUT | `causticHeaterOut`/`causticTemp` |
| 11 | HEATING_RATE | `causticHeaterOut`/`causticTemp` |
| 12 | CAUSTIC_LEVEL | `causticLevelSensor` |
| 13 | HEATER_OVERTEMP | `causticTemp` |
| 14 | STATE_TIMEOUT | per-state timer |
| 15 | SENSOR_FAULT | analog inputs |
| 16 | PAUSE_TIMEOUT | (Held too long) |

## Interrupt capability (ClearCore)

Per Teknic's ClearCore library: **only connectors `DI-6` through `A-12` can trigger
hardware interrupts** (`InterruptHandlerSet()`, RISING/FALLING via
`InputManager::InterruptTrigger`). The multi-function **`IO-0`…`IO-5` are poll-only**.

| Signal | Pin | Interrupt-capable | Notes |
|--------|-----|-------------------|-------|
| `airOk` | DI6 | ✅ | currently polled+debounced |
| `co2Ok` | DI7 | ✅ | currently polled+debounced |
| `ESTOP` | DI8 | ✅ | **FALLING-edge ISR** (trip = HIGH→LOW) + polled backstop — correct placement for fastest abort |
| *(free)* | A9 | ✅ (pin) | unused — enclosure temp moved off-controller |
| `causticTemp` | A10 | ✅ (pin) | analog read |
| `waterOk` | A11 | ✅ | currently polled |
| `causticLevelSensor` | A12 | ✅ (pin) | digital read (debounced) |
| `largeKeg` | IO0 | ❌ | poll-only |
| `cycleStart` | IO1 | ❌ | poll-only |
| `manualDrain` | IO2 | ❌ | poll-only |

> ESTOP landed on DI8 (interrupt-capable) — that's *why* the bare-metal ESTOP ISR
> works. Buttons on IO0–IO2 are inherently poll-only (fine — they're debounced).
> Candidate for instant-abort sensors (air/CO₂/water) if ever needed: DI6/DI7/A11.
> Caveat: SAME53 routes EXTINT through a 16-channel EIC; verify no channel-sharing
> conflicts against the datasheet before enabling multiple ISRs at once.

## Wiring Notes

- Solenoid valves include flyback diodes for inductive-spike protection.
- **ESTOP / safety chain (fail-safe):** DI6–DI8 are **negative-true SINKING** inputs with an internal **3.3 V / 10 kΩ pull-up** (`INPUT_PULLUP` ≡ `INPUT` on ClearCore — the pull-up is fixed, not software-selectable). The E-stop is a **normally-closed loop from GND** through the drive-OK contacts (pump, etc.) and the E-stop switch into DI8. Healthy → DI8 sunk to GND → `digitalRead` **HIGH** = OK. Any break (E-stop pressed, drive fault, severed wire) → internal pull-up → `digitalRead` **LOW** = TRIP. Firmware: `isEstopActive = (digitalRead(ESTOP)==LOW)`, FALLING-edge ISR + polled backstop. The firmware read is supervisory — the NC loop also hard-cuts the drives directly.
- `airOk` (DI6), `co2Ok` (DI7), `ESTOP` (DI8) are all **negative-true sinking inputs** (closed = sunk to GND = `digitalRead` HIGH). For the "OK" inputs the variable means *closed*, so `isAirOk`/`isCo2Ok` = `debounceRead` (HIGH=true) is **already correct** — a break/loss opens the loop → LOW (fail-safe). For `airOk` LOW = supply FAIL; for `co2Ok` (keg-side, closes on pressure) LOW just means *unpressurized* — a broken wire surfaces as a spurious "Keg not at pressure" timeout at end of cycle, never a false pass. Only `ESTOP` (variable means *tripped*, the inverse) needed the active-low flip. Verified on hardware: healthy chain → air/co2 OK, estop INACTIVE.
- IO0/IO1/IO2 (largeKeg/cycleStart/manualDrain): ClearCore IO pins have no internal pull — wire SPDT so the pin is actively driven in both positions, never floating.
- **Chem-to-drain interlock:** caustic/sanitizer valve enables are ganged so a chemical can never be routed to the drain; firmware mirrors this (drain+pump OFF in RETURN phases).
- All low-voltage control optically isolated from main power.
