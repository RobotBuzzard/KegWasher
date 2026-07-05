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
// The recipe actually progresses through DIRTY_DRAIN → ... → PRESSURE →
// COMPLETE (machineState EXECUTE throughout), with output relays toggling
// visibly.
//
// SPECIFICALLY BYPASSED:
//   - hardware_allSystemsGo() returns true unconditionally
//   - In IDLE_READY, the caustic-temp check is skipped so pressing START
//     goes directly to EXECUTE@DIRTY_DRAIN without MACH_STARTING (heating)
//   - enterPhase(WASHING) skips the precondition caustic-temp check
//   - the WASHING phase skips the per-tick caustic-temp check
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
// 2026-06-10: BENCH MODE IS NOW RUNTIME, decided by SD-card presence at
// boot (operator request): no readable WASHER.CFG -> kwBenchMode = true
// (gates bypassed, stages compressed to 5 s). Card present -> production
// behaviour. The compile-time flag is gone; kwBenchMode lives in
// KegConfig.cpp and is set in config_init().
extern bool kwBenchMode;
// ======================================================================

// ---------- Pin map (mirrors docs/io-table.md) ----------
// Inputs
#define airOk                  DI6
#define co2Ok                  DI7
#define ESTOP                  DI8
// A9 is free — enclosure temp moved off the controller (self-regulating fan with
// its own built-in thermometer). Was the Ender-3 100k NTC thermistor.
#define causticTemp            A10
#define waterOk                A11
#define causticLevelSensor     A12
#define largeKeg               IO0
#define cycleStart             IO1
#define manualDrain            IO2

// Outputs
#define co2Out                 IO3
#define alarmOut               IO4    // RED fault / E-stop stacklight (HIGH = on)
#define readyLedOut            IO5    // GREEN cycle-start / ready indicator (HIGH = on)
#define drainOut               CLEARCORE_PIN_CCIOA0
#define waterOut               CLEARCORE_PIN_CCIOA1
#define airOut                 CLEARCORE_PIN_CCIOA2
#define causticOut             CLEARCORE_PIN_CCIOA3
#define pumpOut                CLEARCORE_PIN_CCIOA4
#define sanitizerOut           CLEARCORE_PIN_CCIOA5
#define causticHeaterOut       CLEARCORE_PIN_CCIOA6
#define kegsDoneOut            CLEARCORE_PIN_CCIOA7  // HIGH while COMPLETE (swap-kegs
                                                     // signal for an external indicator);
                                                     // was free after the fan moved
                                                     // off-controller

// Serial ports
#define CcioPort               ConnectorCOM0
#define displayPort            ConnectorCOM1

// MQTT broker / credentials / topic layout live in KegSecrets.h
// (gitignored — see KegSecrets.h.example for the template). Topics
// published / subscribed are all under MQTT_TOPIC_ROOT.

// ---------- SD config file ----------
// MUST be a strict 8.3 filename (<=8 base, <=3 ext, UPPERCASE). The classic
// Arduino SD library (utility/SdFile.cpp make83Name) has NO long-filename
// support — it rejects any name whose extension exceeds 3 chars, so SD.open()
// silently fails. The old "washer.config" (6-char ext) could never be opened;
// that was the long-standing "Config file missing -> using defaults" bug.
#define settingsFileName       "WASHER.CFG"
#define KEY_MAX_LENGTH         30
#define VALUE_MAX_LENGTH       30

// ---------- State model: PackML machine state × ISA-88 recipe phase ----------
// Two orthogonal axes (canonical model: docs/state-taxonomy.md):
//   machineState — PackML (ISA-TR88.00.02): the control / lifecycle state.
//   recipePhase  — ISA-88: which wash phase; valid ONLY while MACH_EXECUTE.
// The 10 wash phases run *inside* MACH_EXECUTE; they are NOT machine states.
// Caustic and sanitizer are reused, so each chem phase is followed by a RETURN
// phase that blows the chem back to its reservoir (caustic by air, sanitizer by
// CO2); intermediate water is purged to drain. Chem never goes to drain.

// ----- Axis A: PackML machine state -----
#define MACH_IDLE        0   // ready / not-ready; waiting for Start (see idleSub)
#define MACH_STARTING    1   // warm-up: heating caustic to temp before Execute
#define MACH_EXECUTE     2   // running the recipe (recipePhase advances within)
#define MACH_HELD        3   // paused mid-recipe; outputs safely held, timer frozen
#define MACH_COMPLETE    4   // cycle done; alarm; Start=next keg / Reset=park
#define MACH_STOPPING    5   // operator STOP/DRAIN: evacuating keg contents
#define MACH_STOPPED     6   // de-energized halt; Reset (Start) recovers
#define MACH_CLEARING    7   // fault acknowledged, verifying cleared → Stopped
#define MACH_ABORTED     8   // fault/ESTOP landing; alarm; Clear begins recovery
#define NUM_MACH_STATES  9

// ----- Axis B: ISA-88 recipe phase (valid only while MACH_EXECUTE) -----
#define PHASE_NONE             0
#define PHASE_DIRTY_DRAIN      1   // air-push old product out (air + drain)   → drain
#define PHASE_DIRTY_RINSE      2   // pre-rinse water + drain                  → drain
#define PHASE_DIRTY_PURGE      3   // air-blow pre-rinse water + drain         → drain
#define PHASE_WASHING          4   // recirculate hot caustic (caustic + pump)
#define PHASE_CAUSTIC_RETURN   5   // air-blow caustic, drain+pump OFF         → caustic reservoir
#define PHASE_RINSING          6   // post-caustic rinse water + drain         → drain
#define PHASE_RINSE_PURGE      7   // air-blow rinse water + drain             → drain
#define PHASE_SANITIZE         8   // recirculate sanitizer (sanitizer + pump)
#define PHASE_SANI_RETURN      9   // CO2-blow sanitizer, drain+pump OFF       → sani reservoir
#define PHASE_PRESSURE         10  // CO2 charge, valves closed (seal keg)
#define NUM_PHASES             11
#define PHASE_FIRST            PHASE_DIRTY_DRAIN
#define PHASE_LAST             PHASE_PRESSURE

// IDLE sub-state (only meaningful while machineState == MACH_IDLE)
#define IDLE_INIT              0   // one-shot init ceremony on entering IDLE
#define IDLE_NOT_READY         1   // systems offline; shows which are failing
#define IDLE_READY             2   // ready; START begins a cycle
#define IDLE_SETTINGS          3   // on-screen settings editor (Phase 5 — not yet wired)

// ---------- Firmware version (published on kegwasher/firmware) ----------
#define KW_FIRMWARE_VERSION    "1.0.2"

// ---------- ADC ----------
#define adcResolution          12
#define ADC_MAX                4095

// ETS50N 4-20 mA shunt resistor (Ω) — per-unit hardware: set to the MEASURED
// value of the installed resistor (nominal 470, ±5% = 446..494) for best
// accuracy. SD key: etsShuntOhms.
#define DEF_ETS_SHUNT_OHMS     470.0f
extern float etsShuntOhms;

// Large-keg timer multiplier compiled default. SD key: largeKegMod.
#define DEF_LARGE_KEG_MOD      1.5

// ---------- Temperature thresholds (°C) — compiled defaults ----------
// These seed the runtime globals below (minCausticTemp, …), which are
// SD-config-overridable so a unit can be re-tuned without a recompile.
#define DEFAULT_MIN_CAUSTIC_TEMP       50   // below this in WASHING → fault
#define DEFAULT_OPTIMAL_CAUSTIC_TEMP   60   // heater target during STARTING
#define DEFAULT_MAX_CAUSTIC_TEMP       70   // caustic safety cutoff
// (enclosure overtemp + fan on/off thresholds removed — enclosure temp/fan are
//  off-controller now; the fan self-regulates via its own thermometer.)

// ---------- Heater limits (compiled defaults; SD-overridable) ----------
#define DEF_MIN_HEATING_RATE   3         // °C/min minimum during fw-mode heating
#define DEF_MAX_HEATING_MS     900000UL  // 15 min heating cap
extern int           minHeatingRate;     // SD key: minHeatingRate (°C/min)
extern unsigned long maxHeatingMs;       // SD key: maxHeatingMs (STARTING bound, both modes)
// (MIN_CAUSTIC_LEVEL is gone — the caustic "level" is the NC float switch on
// A-12, read directly as the isCausticLevelOk bool.)

// Heater control mode (SD key `heaterMode`, deliberately NOT panel-editable):
//   fw  (default) — firmware bang-bang during STARTING only (today's wiring:
//                   causticHeaterOut drives the contactor directly).
//   ext           — the ETS50N's PNP switch output regulates the tank
//                   (two-point SP/RSP thermostat) and causticHeaterOut becomes
//                   a safety PERMIT in series with it. DO NOT set ext until the
//                   probe→contactor chain is physically wired — a permit with
//                   no thermostat holds the contactor closed, bounded only by
//                   the firmware overtemp backstop.
extern bool heaterExternal;

// ---------- Error codes (canonical home) ----------
#define ERR_NONE              0
#define ERR_SD_INIT           1
#define ERR_CONFIG_FILE       2
#define ERR_WATER_PRESSURE    3
#define ERR_AIR_PRESSURE      4
#define ERR_CO2_PRESSURE      5
#define ERR_CAUSTIC_TEMP      6
// 7 retired (was ERR_ENCLOSURE_TEMP) — enclosure temp moved off-controller. Codes
// are not renumbered so existing dashboard/MQTT mappings stay stable.
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
// DIRTY_DRAIN replacement duration while the latching DRAIN switch (IO2) is on:
// full/spoiled kegs need minutes of air-push to empty, not the dregs-clearing
// seconds of dirtyDrainTimer. Latched at cycle start (drainModeLatched).
extern unsigned long fullDrainTimer;
extern double largeKegMod;
// Max time a cycle may sit PAUSED (HELD) before it aborts (chem may be sitting in
// the keg). Runtime var, seeded from PAUSE_MAX_MS; SD-config-overridable (Phase 3).
extern unsigned long pauseMaxMs;
extern float  cfgTouchCal[6];    // touch calibration affine coeffs (a,b,c,d,e,f)
extern bool   cfgTouchCalValid;  // true once SD config supplied touchCalA..F
extern bool   cfgLoadedFromSD;   // true if WASHER.CFG was read; false = compiled defaults

// ---------- Runtime temperature thresholds (°C) ----------
// Seeded from the DEFAULT_*_TEMP defines above; overridable from SD WASHER.CFG.
// Consumers (KegHardware, KegStateMachine) read these, not the DEFAULT_* macros.
extern int minCausticTemp;       // WASHING abort floor
extern int optimalCausticTemp;   // STARTING heater target
extern int tempCalOffsetC10;     // caustic probe calibration offset, tenths of a degree C (panel-tunable)
extern int maxCausticTemp;       // caustic safety cutoff

// ---------- Runtime MQTT broker config ----------
// Seeded from KegSecrets.h compiled defaults; overridable per-device from SD so a
// shipped binary needs no baked-in broker/creds. Strings are capped at
// VALUE_MAX_LENGTH-1 chars by the config-line parser.
extern char mqttBrokerIp[16];    // dotted-quad string, e.g. "192.168.1.111"
extern int  mqttBrokerPort;
extern char mqttUser[32];
extern char mqttPass[32];
extern char mqttClientId[32];
extern char mqttTopicRoot[32];

// Web-editor HTTP Basic auth (SD keys webUser/webPass). Both non-empty = auth
// required on every route; either empty = editor open (default). Never
// mirrored to cfg/* / cfg.json; values masked in acks and logs.
extern char webUser[24];
extern char webPass[24];

// Network (SD keys netMode/netIp/netMask/netGw). Applied at BOOT ONLY —
// remote edits need a power cycle. static requires a valid netIp or the boot
// falls back to DHCP (logged). Unset mask → 255.255.255.0; unset gw → .1.
extern bool netStaticMode;
extern char netIp[16];
extern char netMask[16];
extern char netGw[16];
bool config_parseIp(const char* s, uint8_t out[4]);

// Raised by config_saveToSD(); the main loop republishes the retained MQTT
// cfg/* mirror and clears it (keeps panel-editor saves in sync remotely).
extern bool kwCfgMirrorDirty;

// ---------- API ----------
// NOTE: config_init() must be called AFTER display_init() so failure
// messages have somewhere to render.
void config_init();
bool config_loadFromSD();
void config_saveToSD();
void config_setDefaults();

// Apply one KEY=VALUE with bounded validation — shared by the SD loader and the
// remote (MQTT cfgset) editor. Return codes:
#define KW_CFG_APPLIED   0
#define KW_CFG_UNKNOWN   1
#define KW_CFG_REJECTED  2
byte config_applyKV(const char* key, const char* value);
bool config_tempsOrdered();   // floor ≤ target < cutoff

#endif // KEG_CONFIG_H
