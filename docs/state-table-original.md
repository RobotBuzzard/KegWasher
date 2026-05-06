# Keg Washer State Table (Original)

This is the original state table from before the enhanced startup/heating, drain air burst, and rinse-with-drain changes were added. Kept for diff reference against `state-table.md`.

## Basic State Mapping

| State ID | Name              | Description           | Active Outputs              | Duration (Small) | Duration (Large) |
|----------|-------------------|-----------------------|-----------------------------|------------------|------------------|
| 0        | `STATE_STARTUP`   | Initializing          | None                        | Until button     | Until button     |
| 1        | `STATE_DRAINING`  | Draining keg          | `drainOut`                  | 3 min            | 4.5 min          |
| 2        | `STATE_RINSING`   | Rinsing with water    | `waterOut`                  | 3 min            | 4.5 min          |
| 3        | `STATE_WASHING`   | Caustic wash          | `causticOut`, `pumpOut`     | 3 min            | 4.5 min          |
| 4        | `STATE_SANITIZE`  | Sanitizing            | `sanitizerOut`, `pumpOut`   | 3 min            | 4.5 min          |
| 5        | `STATE_PRESSURE`  | CO2 pressurize        | `co2Out`                    | 3 min            | 4.5 min          |
| 6        | `STATE_FINISHED`  | Cycle complete        | `alarmOut`                  | Until button     | Until button     |
| 9        | `STATE_ERROR`     | Error condition       | `alarmOut`                  | Until button     | Until button     |

## STATE_STARTUP (Original)

- **Entry**: Power-up, completion of previous cycle, or reset from error
- **Behavior**: Wait for `cycleStart` button press; check all systems ready
- **Exit to DRAINING**: All resources OK + button pressed
- **Exit to ERROR**: Any required resource unavailable
- **Active outputs**: None
- **No heating, no caustic temperature check, no progress feedback**

## STATE_DRAINING (Original)

- **Entry**: From STARTUP after button press
- **Behavior**: Open `drainOut` only
- **Exit to RINSING**: Timer expires
- **Exit to ERROR**: ESTOP or resource failure
- **No air burst — relies on gravity to drain**

## STATE_RINSING (Original)

- **Entry**: From DRAINING after timer expires
- **Behavior**: Open `waterOut` only (drain valve closed during rinse)
- **Exit to WASHING**: Timer expires
- **Exit to ERROR**: ESTOP or water pressure loss
- **Drain stays closed — water fills keg, then drains separately later**
- **No end-of-rinse air burst — water residue carries over to wash**

## STATE_WASHING (Original)

- **Entry**: From RINSING after timer expires
- **Behavior**: Open `causticOut` and `pumpOut`
- **Caustic temp check**: Displays warning if `causticTemp < MIN_CAUSTIC_TEMP`
  but **does not transition to ERROR** — cycle continues
- **Exit to SANITIZE**: Timer expires
- **Exit to ERROR**: ESTOP only

## STATE_SANITIZE, STATE_PRESSURE, STATE_FINISHED, STATE_ERROR

Behaviorally unchanged in the updated table.

## What Changed in the Updated Version

| State    | Original                          | Updated                                                     |
|----------|-----------------------------------|-------------------------------------------------------------|
| STARTUP  | Wait for button                   | Sub-states: INIT → HEATING → IO_CHECK → READY               |
| DRAINING | Drain only                        | Drain + 5s air burst at start                               |
| RINSING  | Water only, drain closed          | Water + drain open throughout, 5s air burst at end          |
| WASHING  | Caustic temp = warning only       | Caustic temp < MIN → transitions to ERROR                   |

Diffing against `state-table.md` will show the operational/safety implications of each change.
