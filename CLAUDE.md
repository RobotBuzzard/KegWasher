# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & flash

This is an Arduino sketch for the Teknic ClearCore (SAME53N19A). The canonical compile + upload path is **not** the Arduino IDE — use the `flash.sh` wrapper from the sibling repo:

```bash
~/dev/teknic-clearcore-cli/scripts/flash.sh ~/dev/KegWasher /dev/ttyACM0
```

FQBN is `ClearCore:sam:clearcore`. Direct `arduino-cli compile -b ClearCore:sam:clearcore .` from the sketch root also works for syntax checks. The script handles the 1200-baud touch reset → bossac upload → app-mode verification dance; bypassing it leaves the board stuck in bootloader on transient failures. See `~/dev/teknic-clearcore-cli/README.md` for the Linux gotchas (ModemManager, dialout, udev).

Watching logs: USB serial at 115200 carries `diagnostics_logEvent`. Every log line is **also** mirrored to MQTT `kegwasher/log` once Ethernet + broker come up — tail with:

```bash
mosquitto_sub -h 192.168.1.111 -u rr1 -P <pass> -t 'kegwasher/#' -v
```

Tests live under `tests/DisplayTest/` as a separate sketch — only run when working the display path; there is no unit test framework.

## Required secret file

`KegSecrets.h` is gitignored and must exist for the firmware to compile. Copy `KegSecrets.h.example` → `KegSecrets.h` and fill in MQTT broker IP / user / pass / client id / topic root. Broker credentials for this dev environment live in `~/mosquitto-credentials.txt`.

## Bench mode — read before changing safety code

Bench mode is **runtime**, decided by SD-card presence at boot (since 2026-06-10; the old compile-time `#define BENCH_MODE` is gone): no readable `WASHER.CFG` → `kwBenchMode = true`. When true the firmware:

- `hardware_allSystemsGo()` returns true unconditionally (gates incl. caustic LEVEL bypassed)
- skips `STARTING` (heating) — START goes straight to the recipe
- skips per-phase resource monitors + entry preconditions
- compresses every stage timer to 5 s (~50 s full cycle)

Card present → production behaviour: all gates live (the readiness screen shows a 5th LEVEL row — it's part of `allSystemsGo`). For a cold/dry bench run **with** a card: lower `HEAT TGT` on the settings panel to skip heating, and satisfy the level input. What is **never** bypassed: heater level/overtemp interlocks, per-state hard timeouts, ESTOP (ISR + main-loop), watchdog. Don't gate new safety checks behind `kwBenchMode` — interlocks should be unconditional.

## Architecture

Single Arduino sketch, all sources at the project root (Arduino requires `.ino` filename = folder name). Every module is `#include`'d transitively from `KegWasher.ino`.

```
KegWasher.ino       setup() + loop() — Ethernet/MQTT lives here, NOT in a module
KegConfig.{h,cpp}   Pin map, state IDs, error codes, thresholds, SD config loader
KegHardware.{h,cpp} Debounced inputs, filtered analog reads, output drivers, heater FSM
KegStateMachine.*   Two-axis FSM — PackML machineState (IDLE→STARTING→EXECUTE→…→COMPLETE/STOPPED/ABORTED) × ISA-88 recipePhase (the 10 wash phases, run inside EXECUTE)
KegDisplay.{h,cpp}  gen4-uLCD-43DT (Diablo16) wrapper — raw SPE serial via KegDisplaySerial (KDS::*), NOT genie
KegDisplaySerial.{h,cpp} Firmware-independent serial draw module (screens, partial updates, touch+cal, footer)
KegTimers.*         State-elapsed timing + keg-size duration scaling (`largeKegMod`)
KegDiagnostics.*    Event logging, output-exercise diag mode (DRAIN + START to enter), error strings
KegUtils.*          Small helpers
KegSecrets.h        gitignored — MQTT broker IP/user/pass/client_id/topic root
```

### loop() ordering (KegWasher.ino)

Order in `loop()` is load-bearing — don't reshuffle without understanding why:

1. `timers_update()`, `display_doEvents()` (touch pump), `hardware_readInputs()` — read state.
2. `mqtt_applyCmdFlags()` — **must** run after `hardware_readInputs()` (which overwrites button flags) and before state processing. MQTT commands set `isCycleStartPressed` / `isManualDrainPressed` as one-tick pulses so they flow through the existing button paths.
3. `hardware_consumeEstopFlag()` → log + abort to `MACH_ABORTED` (`ERR_ESTOP`). ISR has already killed outputs; this is the non-ISR-safe follow-up.
4. `stateMachine_process()`.
5. Display: the operating screen (EXECUTE/HELD) draws once on entry and on `recipePhase` change, then partial updates at 1 Hz (timer) / 5 Hz (status). IDLE/COMPLETE/ABORTED/STOPPING/STOPPED manage their own screen transitions from within their state handlers.
6. `Ethernet.maintain()`, `mqtt_loop()`, `mqtt_publishStatus()`, `mqtt_publishHeartbeat()`.
7. `Watchdog.kick()` — **last**, so any hang above triggers an 8 s reset.
8. `delay(10)` paces the loop for debounce stability.

### State machine

The FSM has **two orthogonal axes** (canonical model: `docs/state-taxonomy.md`; as-built I/O + transitions: `docs/state-table.md`). All IDs are numeric `#define`s in `KegConfig.h`:

- **`machineState`** — PackML control state (`MACH_IDLE`/`STARTING`/`EXECUTE`/`HELD`/`COMPLETE`/`STOPPING`/`STOPPED`/`CLEARING`/`ABORTED`). `volatile byte` because the ESTOP path touches it. `loop()` draws the operating screen while `EXECUTE`/`HELD`.
- **`recipePhase`** — ISA-88 wash phase (`PHASE_DIRTY_DRAIN`..`PHASE_PRESSURE`, 10 phases). Valid **only** while `machineState == MACH_EXECUTE`; forced to `PHASE_NONE` otherwise. Advancing the phase is *not* a machine-state change — `machineState` stays `EXECUTE` across the whole recipe.

`MACH_IDLE` has sub-states (`IDLE_INIT`/`NOT_READY`/`READY`/`SETTINGS`) exposed via `idleSub`. PAUSE/RESUME is the `MACH_HELD` state (PackML Hold/Unhold). Per-phase duration comes from `stageTimerFor(phase)` — the single source shared by the phase handlers, the display countdown, and the MQTT remaining-time publish. Abort (`stateMachine_abort()`) is reachable from any state → `MACH_ABORTED`; recovery (`abortRecover()`) lands at `HELD` (cycle preserved) or `IDLE→READY`, gated on the fault condition having cleared. Keg size is **latched** on cycle-start press (`kegSizeLatched`); flipping the selector mid-cycle does not affect timers, but `isLargeKeg` still reflects the live pin state for display/MQTT.

### MQTT layer

All of the MQTT plumbing lives in `KegWasher.ino` itself (not a separate module). Three things to know:

1. **Never publish from inside `mqtt_callback`.** PubSubClient shares one buffer between send/recv; publishing mid-receive corrupts the parser. Callback only sets pending flags; `mqtt_applyCmdFlags()` (called from `loop()`) does the actual work + logging. This includes anything transitively calling `diagnostics_logEvent` (which calls `net_log_send` → `kwMqtt.publish`).
2. Status publishes are **change-detected** against `MqttStatusCache` sentinels initialized to impossible values so the first call flushes everything. Throttled to 250 ms.
3. LWT publishes `kegwasher/online=false` on disconnect; on connect we publish `online=true`, `ip`, and `firmware` (all retained) so a fresh subscriber lands hot.

### Display

The display is a **gen4-uLCD-43DT (Diablo16)** driven by **raw SPE serial graphics commands** over COM1 — **no ViSi-Genie, no Workshop4** (it uses the panel's factory SPE2 runtime). `KegDisplay.{h,cpp}` is a thin wrapper mapping the `display_*()` API onto the `KegDisplaySerial` module (`KDS::*`, at the project root; `tools/*` sketches symlink it). The genie path was dropped 2026-06-03. Hard-won details (full detail in the `kegwasher_display_serial` memory):

- **Portrait 272×480**, RGB565. `KDS::begin()` RTS-resets the panel (adapter RESET-EN is wired), handshakes at 9600, then bumps the link to **115200** (~12× faster redraws) via the setbaud command + RTS-safe `ConnectorCOM1.Speed()`. NOTE: the 4D lib's `setbaudWait` is broken on ClearCore (host `begin()` commented out, and `begin()` floats RTS=RESET) — use `ConnectorCOM1.Speed()`, never `Serial1.begin()`.
- Draw with **write-only (GetAck) commands + RX drain before each**; text via `putCH` per char (NOT `putstr`). Never `gfx_Get(GFX_XMAX)` or `txt_Set(TEXT_XPOS/YPOS)` — invalid args that HALT the runtime ("Bad … command number"). Font cell ≈ 12px×8px for `txt_MoveCursor`.
- Touch: read raw-framed (the lib's `touch_Get`/GetAckResp desyncs); host-side **affine calibration** applied in `KDS::touch()` (coeffs baked in; SD `touchCalA..F` overrides). Re-cal via `tools/diablo_touch_cal` (tap 3 targets).
- Footer = IP + MQTT pub/sub dots (`display_setMqttIndicators`, P=publish/S=receive). On-screen **START** button on READY/FINISHED (`display_takeTouchStart` → `isCycleStartPressed`, ESTOP-gated).
- Recover a halted/stuck panel: **double-tap the on-board RESET** (forces the bootloader) or just re-flash (`begin()` RTS-resets it).

### Hardware specifics

- Inputs: `airOk`, `co2Ok`, `waterOk` are software-debounced. `ESTOP` is **not** debounced (NC wiring, fastest response wins). The ESTOP ISR is bare-metal — no Serial, no CCIO writes, no display calls; it sets `estopFlag` + kills outputs and that's it. Main loop drains the flag via `hardware_consumeEstopFlag()`.
- CCIO-8 expansion module is on COM0; the display is on COM1. USB Serial is diagnostics only.
- SD card holds `WASHER.CFG` (timer/threshold/MQTT overrides). **Filename must be 8.3/UPPERCASE** — the classic Arduino SD library has no long-filename support, so a name like `washer.config` silently fails to open (this was a real bug). Schema is the `KEY=VALUE` format under `config/washer.cfg.example`. If the SD is missing/corrupt, compiled defaults from `KegConfig.cpp` are used and a banner is shown.
- Watchdog: armed in `setup()` after `setupEthernet()` because DHCP can legitimately take longer than the 8 s WDT timeout. `diagnostics_runTest()` disables WDT around its ~10 s of output exercises and re-enables on exit.

### Reference docs (in `docs/`)

- `io-table.md` — pin assignments, active states, error code → I/O mapping
- `state-taxonomy.md` — canonical two-axis model (PackML machineState × ISA-88 recipePhase)
- `state-table.md` — as-built per-state/phase inputs/outputs/preconditions/error conditions
- `reliability-todo.md` — prioritized hardening roadmap (referenced from TODO.md)

(The display path is documented in the Display section above + the `kegwasher_display_serial` memory; the old Goldelox/genie `display-guide.md` was removed.)

`TODO.md` at the root is the live roadmap (phases 0-5); `reliability-todo.md` has deeper per-item rationale.

### External dependencies

- `ClearCore:sam` board package (Teknic) — adds `https://www.teknic.com/files/downloads/package_clearcore_index.json` to `arduino-cli` board manager URLs
- `ClearCoreWatchdog` library — sibling repo at `github.com/RobotBuzzard/ClearCoreWatchdog`, also available via Arduino Library Manager
- `Diablo_Serial_4DLib` — 4D Systems Diablo16 serial-command library (the display backend; genieArduinoDEV is no longer used)
- `PubSubClient` — MQTT
- `Ethernet`, `SPI`, `SD` — Arduino core libraries
