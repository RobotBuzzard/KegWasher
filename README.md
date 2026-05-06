# Keg Washer Control System

Brewery keg washer for the Teknic ClearCore controller. Automates cleaning and sanitization of 1/6 BBL (20L) and 1/2 BBL (50L) kegs.

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

1. Clone the repo, open `src/KegWasher.ino` in Arduino IDE
2. Install ClearCore board support and the Goldelox_Serial_4DLib library
3. Compile and upload to the ClearCore
4. Copy `config/washer.config.example` → `washer.config` on an SD card, edit timers as needed, insert into ClearCore
5. Power up. The system heats caustic on first boot — wait for "Press START to begin"

## Configuration

All cycle timing lives in `washer.config` on the SD card. Default cycle is 3 minutes per stage; large kegs get 1.5× by default. See `config/washer.config.example` for all keys and explanations.

If the SD card or config file is missing/corrupt, the firmware falls back to compiled defaults and shows "Using default settings" briefly on the display.

## Diagnostic Mode

Hold both DRAIN and START buttons at the same time to enter diagnostic mode. Each output is exercised in sequence (1 second each) and input states are reported on the display. Press START to exit.

## Project Structure

```
KegWasher/
├── src/                 # Arduino source (8 modules + .ino)
├── docs/                # State table, I/O table, reliability TODO
├── config/              # Example SD config
├── README.md
├── LICENSE              # MIT
└── .gitignore
```

## Status

v0.1 — functional foundation. See `docs/reliability-todo.md` for the queue: input debouncing, watchdog timer, EEPROM state persistence for power-failure recovery, sensor redundancy. The WASHING state currently transitions to ERROR if caustic temperature drops below minimum mid-cycle (was previously just a warning).

## Safety

Hot caustic, pressurized gas, 240V, immersion heater. PPE always. The E-STOP loop kills all outputs except the alarm via hardware interrupt — verify it's wired through the front panel before powering on.

## License

MIT — see `LICENSE`.
