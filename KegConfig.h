// ======================================================================
// KegConfig.h - Pin map, state IDs, error codes, thresholds, SD config
// ======================================================================
// Single source of truth for compile-time constants. Other modules
// must NOT redefine these — KegDiagnostics.h previously duplicated the
// error codes; that duplication has been removed.
// ======================================================================
#ifndef KEG_CONFIG_H
#define KEG_CONFIG_H

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

// ======================================================================
// BENCH_MODE — DEVELOPMENT-ONLY FLAG. **NEVER SHIP DEFINED.**
// ======================================================================
// When defined, the firmware bypasses the sensor-driven gates that
// otherwise refuse to run a cycle without real plumbing connected.
// The state machine actually progresses through DRAINING → RINSING →
// WASHING → SANITIZE → PRESSURE → FINISHED, with output relays
// toggling visibly.
//
// SPECIFICALLY BYPASSED:
//   - hardware_allSystemsGo() returns true unconditionally
//   - STARTUP_HEATING substate is skipped (INIT → IO_CHECK direct)
//   - enterState(WASHING) skips the precondition caustic-temp check
//   - state_washing skips the per-tick caustic-temp check
//
// NOT BYPASSED (these are hardware-safety, not operating-policy):
//   - hardware_setCausticHeater() interlocks (level, overtemp) — but
//     unreachable here because heating is skipped entirely in bench mode
//   - state timeouts (still fire if a stage runs too long)
//   - ESTOP (still works — both ISR and main-loop paths)
//   - Watchdog (still fires on hangs)
//
// Every BENCH_MODE boot logs a loud banner to USB Serial AND shows a
// "BENCH MODE" notice on the boot splash. Grep the .ino for
// "BENCH_MODE" before any release to confirm it's commented out.
//
// Uncomment for dev builds; leave commented for production:
#define BENCH_MODE
// ======================================================================

// ---------- Pin map (mirrors docs/io-table.md) ----------
// Inputs
#define airOk                  DI6
#define co2Ok                  DI7
#define ESTOP                  DI8
#define enclosureTemp          A9
#define causticTemp            A10
#define waterOk                A11
#define causticLevelSensor     A12
#define largeKeg               IO0
#define cycleStart             IO1
#define manualDrain            IO2

// Outputs
#define co2Out                 IO3
// Swapped from the original io-table to match as-wired bench hardware:
// the cabinet fan is on IO5 and the alarm is on IO4. Both pins support
// PWM via the SAME53's TCC channels — the choice is wiring-driven.
#define alarmOut               IO4
#define cabinFanPWM            IO5
#define drainOut               CLEARCORE_PIN_CCIOA0
#define waterOut               CLEARCORE_PIN_CCIOA1
#define airOut                 CLEARCORE_PIN_CCIOA2
#define causticOut             CLEARCORE_PIN_CCIOA3
#define pumpOut                CLEARCORE_PIN_CCIOA4
#define sanitizerOut           CLEARCORE_PIN_CCIOA5
#define causticHeaterOut       CLEARCORE_PIN_CCIOA6

// Serial ports
#define CcioPort               ConnectorCOM0
#define displayPort            ConnectorCOM1

// ---------- SD config file ----------
#define settingsFileName       "washer.config"
#define KEY_MAX_LENGTH         30
#define VALUE_MAX_LENGTH       30

// ---------- State IDs ----------
#define STATE_STARTUP          0
#define STATE_DRAINING         1
#define STATE_RINSING          2
#define STATE_WASHING          3
#define STATE_SANITIZE         4
#define STATE_PRESSURE         5
#define STATE_FINISHED         6
#define STATE_ERROR            7
#define NUM_STATES             8

// Startup sub-states
#define STARTUP_INIT           0
#define STARTUP_HEATING        1
#define STARTUP_IO_CHECK       2
#define STARTUP_READY          3

// ---------- ADC ----------
#define adcResolution          12
#define ADC_MAX                4095

// ---------- Temperature thresholds (°C) ----------
#define MIN_CAUSTIC_TEMP       50      // below this in WASHING → ERROR
#define OPTIMAL_CAUSTIC_TEMP   60      // heater target during STARTUP
#define MAX_CAUSTIC_TEMP       70      // safety cutoff
#define MAX_ENCLOSURE_TEMP     60
#define FAN_ON_TEMP            40
#define FAN_OFF_TEMP           35

// ---------- Heater limits ----------
#define MIN_HEATING_RATE       3       // °C/min minimum during heating
#define MAX_HEATING_TIME       900000UL  // 15 min cap
#define MIN_CAUSTIC_LEVEL      25      // % — refuse heating below this

// ---------- Error codes (canonical home) ----------
#define ERR_NONE              0
#define ERR_SD_INIT           1
#define ERR_CONFIG_FILE       2
#define ERR_WATER_PRESSURE    3
#define ERR_AIR_PRESSURE      4
#define ERR_CO2_PRESSURE      5
#define ERR_CAUSTIC_TEMP      6
#define ERR_ENCLOSURE_TEMP    7
#define ERR_ESTOP             8
#define ERR_INVALID_STATE     9
#define ERR_HEATING_TIMEOUT   10
#define ERR_HEATING_RATE      11
#define ERR_CAUSTIC_LEVEL     12
#define ERR_HEATER_OVERTEMP   13
#define ERR_STATE_TIMEOUT     14
#define ERR_SENSOR_FAULT      15

// ---------- Runtime config (loaded from SD or defaults) ----------
extern unsigned long dirtyDrainTimer;
extern unsigned long dirtyRinseTimer;   // reserved for planned dirty-rinse path
extern unsigned long dirtyPurgeTimer;   // reserved for planned dirty-purge path
extern unsigned long rinseTimer;
extern unsigned long purgeTimer;
extern unsigned long washTimer;
extern unsigned long saniTimer;
extern double largeKegMod;

// ---------- API ----------
// NOTE: config_init() must be called AFTER display_init() so failure
// messages have somewhere to render.
void config_init();
bool config_loadFromSD();
void config_saveToSD();
void config_setDefaults();

#endif // KEG_CONFIG_H
