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
- **Display**: 4D Systems Goldelox 128x128 (portrait, on COM1)
- **Storage**: SD card in ClearCore slot for `washer.config`
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

```
STARTUP   → heat caustic to 60°C, validate I/O, wait for START button
DRAINING  → open drain + 5s air burst to expel old product
RINSING   → water + drain open continuously, 5s air burst at end
WASHING   → caustic + pump (requires caustic ≥ 50°C)
SANITIZE  → sanitizer + pump
PRESSURE  → CO2 to pressurize and seal-test the keg
FINISHED  → alarm, wait for button press
```

ERROR is reachable from any state; clear and acknowledge with the START button to return to STARTUP.

## Quick Start

1. Clone the repo, open `KegWasher.ino` in Arduino IDE (or use `arduino-cli`)
2. Install the ClearCore board package — add `https://www.teknic.com/files/downloads/package_clearcore_index.json` to Boards Manager URLs, then install `ClearCore:sam`
3. Install the `Goldelox-Serial-Arduino-Library` from the Library Manager
4. Compile and upload to the ClearCore (FQBN: `ClearCore:sam:clearcore`)
4. Copy `config/washer.config.example` → `washer.config` on an SD card, edit timers as needed, insert into ClearCore
5. Power up. The system heats caustic on first boot — wait for "Press START to begin"

## Configuration

All cycle timing lives in `washer.config` on the SD card. Default cycle is 3 minutes per stage; large kegs get 1.5× by default. See `config/washer.config.example` for all keys and explanations.

If the SD card or config file is missing/corrupt, the firmware falls back to compiled defaults and shows "Using default settings" briefly on the display.

## Bench mode

`KegConfig.h` defines `BENCH_MODE` as a compile-time flag (uncommented today, while the project is in pre-production). With it active, the firmware:

- Skips `STARTUP_HEATING` (goes directly INIT → IO_CHECK → READY)
- Returns `true` from `hardware_allSystemsGo()` regardless of sensor state
- Skips the per-tick and entry caustic-temp checks in `STATE_WASHING`
- Overrides the compiled-default stage timers from 3 min to **5 sec each**, so a full cycle runs in ~25 sec instead of 15 min (validation bounds on SD-loaded values are unchanged)
- Logs a loud `*** BENCH_MODE ACTIVE — DO NOT SHIP THIS ***` banner to USB Serial at every boot
- Renders a persistent `** BENCH MODE **` line on the boot splash, the READY screen, and the FINISHED screen

What's **not** bypassed even in bench mode:

- `hardware_setCausticHeater()`'s level / overtemp interlocks (those are hardware-safety, not policy)
- Per-state hard timeouts (DRAINING 10 min, WASHING 20 min, etc.)
- E-STOP (both the ISR-immediate output kill and the main-loop ERROR transition)
- The hardware watchdog (8 s timeout, kicks every loop)

**Before any production build**: open `KegConfig.h`, comment out the `#define BENCH_MODE` line, and verify with `grep BENCH_MODE *.h *.cpp` that the only remaining mentions are inside `#ifdef` guards. The shipped firmware in that state is byte-identical to a non-bench build.

## Diagnostic Mode

Hold both DRAIN and START buttons at the same time to enter diagnostic mode. Each output is exercised in sequence (1 second each) and input states are reported on the display. Press START to exit.

## Project Structure

```
KegWasher/
├── KegWasher.ino        # Arduino sketch (entry point)
├── Keg*.h / Keg*.cpp    # 8 firmware modules
├── docs/                # State table, I/O table, reliability TODO
├── config/              # Example SD config
├── README.md
├── LICENSE              # MIT
└── .gitignore
```

The `.ino` and module sources live at the project root by Arduino sketch convention (the sketch folder name must match the `.ino` filename). All modules are #include'd transitively from `KegWasher.ino`.

## Status

**v0.1-bench — works on a workbench, not yet on a real machine.**

What's verified end-to-end on the bench (no plumbing connected):

- Firmware compiles and flashes via the [`teknic-clearcore-cli`](https://github.com/RobotBuzzard/teknic-clearcore-cli) `flash.sh` workflow
- Boot reaches `STARTUP_READY` on the Goldelox 128x128 display
- Full state-machine cycle runs in BENCH_MODE: `STARTUP → DRAINING → RINSING → WASHING → SANITIZE → PRESSURE → FINISHED`, with output relays toggling visibly and the display updating cleanly on each transition
- Hardware watchdog ([`ClearCoreWatchdog`](https://github.com/RobotBuzzard/ClearCoreWatchdog)) bench-validated; deliberate-hang test confirms ~8 s automatic reset
- Display rendering pattern resolved (once-on-entry for STARTUP_READY, FINISHED, ERROR; 1 Hz refresh for operating states)
- Alarm UX split: DRAIN silences without resetting, long-press START resets

What's **not** verified:

- Anything involving real plumbing — water, air, CO2, caustic, sanitizer, the heater, the pump, the keg
- Anything involving real sensors — the firmware currently can't even tell whether they're connected because `BENCH_MODE` bypasses the sensor gates entirely
- Anything that requires the system to run for hours or days (long-term-stability items in the reliability TODO)
- Sensor calibration / drift / sanity ranges against real-world readings

See [`docs/reliability-todo.md`](docs/reliability-todo.md) for the remaining roadmap and priority order.

## Safety

Hot caustic, pressurized gas, 240V, immersion heater. PPE always. The E-STOP loop kills all outputs except the alarm via hardware interrupt — verify it's wired through the front panel before powering on.

## License

MIT — see `LICENSE`.
