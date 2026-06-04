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
//   - In STARTUP_READY, the caustic-temp check is skipped so pressing
//     START goes directly to STATE_DIRTY_DRAIN without STARTUP_HEATING
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

// MQTT broker / credentials / topic layout live in KegSecrets.h
// (gitignored — see KegSecrets.h.example for the template). Topics
// published / subscribed are all under MQTT_TOPIC_ROOT.

// ---------- SD config file ----------
#define settingsFileName       "washer.config"
#define KEY_MAX_LENGTH         30
#define VALUE_MAX_LENGTH       30

// ---------- State IDs ----------
// Linear 10-stage wash cycle. Caustic and sanitizer are reused, so each chem
// stage is followed by a RETURN stage that blows the chem back to its reservoir
// (caustic by air, sanitizer by CO2); intermediate water is purged to drain.
// Chem never goes to drain. See docs/state-table.md for the full valve table.
#define STATE_STARTUP          0
#define STATE_DIRTY_DRAIN      1   // drain old product + 5s air burst        → drain
#define STATE_DIRTY_RINSE      2   // pre-rinse water + drain                  → drain
#define STATE_DIRTY_PURGE      3   // air-blow pre-rinse water + drain         → drain
#define STATE_WASHING          4   // recirculate hot caustic (caustic + pump)
#define STATE_CAUSTIC_RETURN   5   // air-blow caustic, drain+pump OFF         → caustic reservoir
#define STATE_RINSING          6   // post-caustic rinse water + drain         → drain
#define STATE_RINSE_PURGE      7   // air-blow rinse water + drain             → drain
#define STATE_SANITIZE         8   // recirculate sanitizer (sanitizer + pump)
#define STATE_SANI_RETURN      9   // CO2-blow sanitizer, drain+pump OFF       → sani reservoir
#define STATE_PRESSURE         10  // CO2 charge, valves closed (seal keg)
#define STATE_FINISHED         11
#define STATE_ERROR            12
// STOPPING/HALTED are appended AFTER ERROR so the operating range + every
// existing ID is unchanged. STOPPING runs an operator-STOP evacuation (route
// keg contents to the right place), then HALTED de-energizes and waits.
#define STATE_STOPPING         13
#define STATE_HALTED           14
#define NUM_STATES             15

// Contiguous "operating" range — checked by loop() to decide whether to draw
// the operating screen, and by the state machine for whole-cycle behaviour.
// (STOPPING/HALTED are intentionally NOT in this range — they own their screens.)
#define STATE_OP_FIRST         STATE_DIRTY_DRAIN
#define STATE_OP_LAST          STATE_PRESSURE

// Startup sub-states
#define STARTUP_INIT           0
#define STARTUP_HEATING        1
#define STARTUP_SETTINGS       2   // on-screen settings editor (repurposed unused IO_CHECK slot)
#define STARTUP_READY          3
#define STARTUP_NOT_READY      4   // systems offline; shows which are failing

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
#define ERR_PAUSE_TIMEOUT     16

// Max time a cycle may sit PAUSED before the firmware sounds the alarm and
// aborts to ERROR — caustic/sanitizer can be sitting in the keg. The operator
// can RESUME any time before this expires.
#define PAUSE_MAX_MS          (10UL * 60 * 1000)

// ---------- Runtime config (loaded from SD or defaults) ----------
// Stage durations (ms), one per operating state. dirtyRinse/dirtyPurge were
// previously loaded-but-unused; they (and the three *Rtn/Purge timers) now
// drive the full 10-stage cycle.
extern unsigned long dirtyDrainTimer;   // DIRTY_DRAIN
extern unsigned long dirtyRinseTimer;   // DIRTY_RINSE
extern unsigned long dirtyPurgeTimer;   // DIRTY_PURGE
extern unsigned long washTimer;         // WASHING
extern unsigned long causticRtnTimer;   // CAUSTIC_RETURN (air-blow caustic → reservoir)
extern unsigned long rinseTimer;        // RINSING
extern unsigned long rinsePurgeTimer;   // RINSE_PURGE (air-blow rinse water → drain)
extern unsigned long saniTimer;         // SANITIZE
extern unsigned long saniRtnTimer;      // SANI_RETURN (CO2-blow sanitizer → reservoir)
extern unsigned long purgeTimer;        // PRESSURE (CO2 charge / seal)
extern double largeKegMod;
extern float  cfgTouchCal[6];    // touch calibration affine coeffs (a,b,c,d,e,f)
extern bool   cfgTouchCalValid;  // true once SD config supplied touchCalA..F

// ---------- API ----------
// NOTE: config_init() must be called AFTER display_init() so failure
// messages have somewhere to render.
void config_init();
bool config_loadFromSD();
void config_saveToSD();
void config_setDefaults();

#endif // KEG_CONFIG_H
