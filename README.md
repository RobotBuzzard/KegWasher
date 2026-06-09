# Keg Washer Control System

Brewery keg washer for the Teknic ClearCore controller. Automates cleaning and sanitization of 1/6 BBL (20L) and 1/2 BBL (50L) kegs.

> ⚠️ **Pre-production. Bench development only.**
>
> Right now, `KegConfig.h` ships with `#define BENCH_MODE` *active* (uncommented). The firmware in this repo is **not safe to run on a real keg-cleaning machine** — bench mode deliberately bypasses sensor checks so the state machine can be exercised on a bare workbench with no plumbing, no kegs, no caustic, no CO2. See [Bench mode](#bench-mode) below.
>
> Nothing here has been wet-tested against actual hardware. No production cycle has ever run. Don't connect chemicals or pressurized lines to this until **all** of the following are true:
>
> 1. `#define BENCH_MODE` is **commented out** in `KegConfig.h`
> 2. Every input listed in [`docs/io-table.md`](docs/io-table.md) is physically wired and verified via diagnostic mode (hold DRAIN + START)
> 3. The E-STOP loop is wired through the front panel and verified to kill outputs at the contactor
> 4. The remaining Priority 0 items in [`docs/reliability-todo.md`](docs/reliability-todo.md) are closed (per-state display pages, watchdog API verification on real hardware, ClearCore reference docs)
> 5. The reliability items below `## Priority 1` are at least partially addressed (output verification once feedback inputs exist, etc.)

## Why This Exists

Manually washing kegs is tedious, inconsistent, and wastes water and chemicals. This system runs a fixed sequence of cycles — drain, rinse, caustic wash, sanitize, pressurize — with timing tuned for keg size and a hot caustic solution that actually cleans. Press one button, walk away, come back to a clean keg.

## Hardware Platform

- **Controller**: Teknic ClearCore + CCIO-8 expansion module
- **Display**: 4D Systems gen4-uLCD-43DT (Diablo16, 480×272, used portrait 272×480) on COM1 — raw SPE serial, no ViSi-Genie
- **Storage**: SD card in ClearCore slot for `WASHER.CFG` (8.3 filename required)
- **Heater**: 240V 5500W immersion heater, ~3 gal caustic reservoir (≈8 min from room temp to 60°C)

### Utility Requirements

| Resource   | Spec               |
|------------|--------------------|
| Electrical | 220V, 15A          |
| Air        | 100 PSI, 6 CFM     |
| CO2        | 60 PSI, 3 CFM     |
| Water      | 50 PSI, 8 GPM      |
| Caustic    | 20L, ~5% by weight |
| Sanitizer  | 20L, ~10% by vol   |

## Cycle Sequence

The wash is a **10-stage recipe with chemical recovery** — caustic and sanitizer are reused, so each is blown back to its reservoir (never to drain) before the next stage. Caustic is heated during start-up; WASHING requires it at ≥ 50 °C.

```
 1  DIRTY DRAIN    drain old product (+ 5s air burst)        → drain
 2  DIRTY RINSE    water + drain                             → drain
 3  DIRTY PURGE    air blow                                  → drain
 4  WASHING        caustic + pump (recirc, requires ≥ 50°C)
 5  CAUSTIC RTN    air blows caustic back                    → caustic tank
 6  RINSING        water + drain
 7  RINSE PURGE    air blow                                  → drain
 8  SANITIZE       sanitizer + pump (recirc)
 9  SANI RTN       CO2 blows sanitizer back                  → sanitizer tank
10  PRESSURE       CO2 charge / seal-test
```

The controller runs a **two-axis state machine**: a PackML machine state (`IDLE → STARTING → EXECUTE → COMPLETE`, plus `HELD`/`STOPPING`/`STOPPED`/`ABORTED`) with the 10 wash phases above running as ISA-88 recipe phases inside `EXECUTE`. Operator controls: one button press per keg to START; PAUSE/RESUME (HELD), RESTART (re-run the current stage), and STOP-DRAIN (evacuate → HALTED). A fault lands in `ABORTED`; recover with a long-press of START once the cause clears. Canonical model in [`docs/state-taxonomy.md`](docs/state-taxonomy.md).

Remote status and control are also available over MQTT (see [Remote operation](#remote-operation)).

## Quick Start

1. Clone the repo, open `KegWasher.ino` in Arduino IDE (or use `arduino-cli`)
2. Install the ClearCore board package — add `https://www.teknic.com/files/downloads/package_clearcore_index.json` to Boards Manager URLs, then install `ClearCore:sam`
3. Install the `Diablo_Serial_4DLib` (4D Systems Diablo16 serial) library, plus `PubSubClient` (MQTT)
4. Copy `KegSecrets.h.example` → `KegSecrets.h` and fill in the MQTT broker IP / user / pass / client id / topic root (required to compile)
5. Compile and upload to the ClearCore (FQBN: `ClearCore:sam:clearcore`)
6. Copy `config/washer.cfg.example` → `WASHER.CFG` (uppercase 8.3 name — required by the SD library) on an SD card, edit timers as needed, insert into ClearCore
7. Power up. In a production build the system heats caustic on first boot — wait for the READY screen

## Configuration

Cycle timing, temperature thresholds, and the MQTT broker block all live in `WASHER.CFG` (key=value) on the SD card. Production defaults total **≈ 4.4 min/cycle** (per-stage timers, not a single flat value); large kegs get 1.5× by default. See `config/washer.cfg.example` for all keys and explanations.

Timers can also be tuned **on the panel** via the on-screen settings editor (the SETTINGS button on the READY screen).

If the SD card or config file is missing/corrupt, the firmware falls back to compiled defaults; the boot screen reports `SD CONFIG: DEFAULT` (vs `READ OK` when `WASHER.CFG` loaded).

## Bench mode

`KegConfig.h` defines `BENCH_MODE` as a compile-time flag (uncommented today, while the project is in pre-production). With it active, the firmware:

- Skips the `STARTING` caustic heat-up (boots directly to `IDLE → READY`)
- Returns `true` from `hardware_allSystemsGo()` regardless of sensor state
- Skips the per-tick and entry caustic-temp checks in the `WASHING` phase
- Compresses every stage timer to **5 sec**, so a full 10-stage cycle runs in **~50 sec** instead of the ≈ 4.4 min production default (validation bounds on SD-loaded values are unchanged)
- Logs a loud `**  BENCH_MODE ACTIVE — DO NOT SHIP THIS  **` banner to USB Serial at every boot

What's **not** bypassed even in bench mode:

- `hardware_setCausticHeater()`'s level / overtemp interlocks (those are hardware-safety, not policy)
- Per-phase hard timeouts (most phases 10 min, WASHING 20 min) and the STOPPING evacuation cap (2 min)
- E-STOP (both the ISR-immediate output kill and the main-loop ABORTED transition)
- The hardware watchdog (8 s timeout, kicks every loop)

**Before any production build**: open `KegConfig.h`, comment out the `#define BENCH_MODE` line, and verify with `grep BENCH_MODE *.h *.cpp` that the only remaining mentions are inside `#ifdef` guards. The shipped firmware in that state is byte-identical to a non-bench build.

## Diagnostic Mode

Hold both DRAIN and START buttons at the same time to enter diagnostic mode. Each output is exercised in sequence (1 second each) and input states are reported on the display. Press START to exit.

## Remote operation

There is **no HTTP server on the controller** — all remote status and control flow through MQTT (broker handles auth; retained topics + LWT give "is it alive / what's it doing" for free). The firmware publishes change-detected, retained status (`kegwasher/state`, `.../phase`, timer, temps, sensors, error, plus a 5 s heartbeat) and subscribes to `kegwasher/cmd/+` (start, pause, resume, restart, stop, reset). Tail it with:

```bash
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'kegwasher/#' -v
```

The on-device front panel (gen4-uLCD-43DT) shows per-phase screens with a mm:ss countdown, the START / PAUSE-RESUME / RESTART / STOP-DRAIN / SETTINGS touch controls, a footer with the device IP + MQTT indicator, and stacklight outputs (RED = fault/E-stop, GREEN = ready). Broker IP/credentials are configurable per device from `WASHER.CFG`, falling back to the compiled `KegSecrets.h`.

## Project Structure

```
KegWasher/
├── KegWasher.ino        # Arduino sketch (entry point)
├── Keg*.h / Keg*.cpp    # firmware modules (config, hardware, state machine,
│                        #   display + display-serial, timers, diagnostics, utils)
├── KegSecrets.h         # gitignored — MQTT broker IP/user/pass/client/topic
├── docs/                # state + I/O tables, state taxonomy, reliability TODO
├── config/              # example SD config (washer.cfg.example)
├── README.md
├── LICENSE              # MIT
└── .gitignore
```

The `.ino` and module sources live at the project root by Arduino sketch convention (the sketch folder name must match the `.ino` filename). All modules are #include'd transitively from `KegWasher.ino`.

## Status

**Bench-verified, pre-production — moving from the bench to the prototype chassis.**

What's verified end-to-end on the bench (no plumbing connected):

- Firmware compiles and flashes via the [`teknic-clearcore-cli`](https://github.com/RobotBuzzard/teknic-clearcore-cli) `flash.sh` workflow
- Boot reaches `IDLE / READY` on the gen4-uLCD-43DT (Diablo16, 272×480) panel, with a Linux-style boot screen confirming the SD-config read
- Full 10-stage cycle runs in BENCH_MODE (`DIRTY DRAIN → … → PRESSURE`, ~50 s), with output relays toggling visibly and per-phase screens updating cleanly on each transition
- Two-axis PackML/ISA-88 state machine + operator controls (START, PAUSE/RESUME, RESTART, STOP-DRAIN) exercised on hardware
- Ethernet/DHCP + MQTT status/command surface live to a Mosquitto broker (retained topics, heartbeat, LWT)
- Hardware watchdog ([`ClearCoreWatchdog`](https://github.com/RobotBuzzard/ClearCoreWatchdog)) bench-validated; deliberate-hang test confirms ~8 s automatic reset
- E-STOP verified for normally-closed wiring (ISR-immediate output kill + main-loop ABORTED transition)
- Physical START button (active-high) + GREEN ready / RED fault stacklight indicators confirmed
- **Caustic temperature probe calibrated** — ProSense ETS50N (4–20 mA on A10) reads true against a reference
- Alarm UX split: DRAIN silences without resetting, long-press START resets

What's **not** verified:

- Anything involving real plumbing — water, air, CO2, caustic, sanitizer, the heater, the pump, the keg (no wet test has been run)
- The remaining sensors against real-world readings — only the A10 caustic-temp probe is calibrated so far; while `BENCH_MODE` is active the sensor gates are bypassed entirely
- Anything that requires the system to run for hours or days (long-term-stability items in the reliability TODO)

> The enclosure temperature input and cooling-fan output were removed from the controller — the cabinet fan is a standalone self-regulating unit, so the firmware no longer senses enclosure temp or drives a fan.

See [`docs/reliability-todo.md`](docs/reliability-todo.md) for the remaining roadmap and priority order.

## Safety

Hot caustic, pressurized gas, 240V, immersion heater. PPE always. The E-STOP loop kills all outputs except the alarm via hardware interrupt — verify it's wired through the front panel before powering on.

## License

MIT — see `LICENSE`.
