// ======================================================================
// KegStateMachine.cpp - State logic, transitions, and per-state timeouts
// ======================================================================
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegDisplay.h"
#include "KegTimers.h"
#include "KegDiagnostics.h"

volatile byte currentState = STATE_STARTUP;

// Indexed by state ID — contiguous 0..NUM_STATES-1, no gaps. Used for logging,
// the MQTT state topic, and the operating-screen title. Kept short so they fit
// the title bar at size-3 font.
const char* stateNames[NUM_STATES] = {
  "STARTUP",
  "DIRTY DRAIN",
  "DIRTY RINSE",
  "DIRTY PURGE",
  "WASHING",
  "CAUSTIC RTN",
  "RINSING",
  "RINSE PURGE",
  "SANITIZE",
  "SANI RTN",
  "PRESSURE",
  "FINISHED",
  "ERROR",
  "STOPPING",
  "HALTED"
};

byte startupSubState = STARTUP_INIT;
bool kegSizeLatched  = false;  // set at START press; used by all timer calculations

// One-time pre-check latch. The systems-go pre-check (NOT_READY screen) and the
// caustic heat-up are an init ceremony, not a per-cycle one: throughput matters,
// so once the washer reaches READY we don't make the operator sit through them
// again between kegs. Cleared on any fault so systems get re-verified on
// recovery. The common between-kegs path (FINISHED→START→DIRTY_DRAIN) bypasses
// STARTUP entirely, so it never sees the pre-check at all.
static bool washerInitialized = false;

bool cyclePaused = false;            // see KegStateMachine.h
static unsigned long pauseStartMs = 0;

// Operator STOP/DRAIN evacuation: what's currently in the keg (captured when
// STOP is pressed) — decides the evacuation routing. Read by
// enterState(STATE_STOPPING). (RESTART does NOT evacuate; it just re-runs the
// current stage.)
enum { EVAC_NONE = 0, EVAC_CAUSTIC, EVAC_SANI, EVAC_WATER };
static byte evacKind = EVAC_NONE;

// Hard upper bounds on time-in-state. Normal completion happens via
// timer-driven transitions; these are last-resort safety nets if a
// transition condition never triggers (sensor stuck, valve failure, etc.).
// 0 = no timeout (operator-paced or self-bounded states).
//
// STARTUP has no state-level timeout — STARTUP_HEATING is bounded by
// hardware_monitorHeating's MAX_HEATING_TIME (15 min), and the other
// substates are operator-paced (READY can wait indefinitely for the
// cycleStart press). A blanket STARTUP timeout would expire on
// operators who set up the bench and then walk away.
static const unsigned long stateMaxDuration[NUM_STATES] = {
  0,                  // STARTUP        — heater bounded internally; READY is operator-paced
  10UL * 60 * 1000,   // DIRTY_DRAIN
  10UL * 60 * 1000,   // DIRTY_RINSE
  10UL * 60 * 1000,   // DIRTY_PURGE
  20UL * 60 * 1000,   // WASHING        — longest stage with size modifier
  10UL * 60 * 1000,   // CAUSTIC_RETURN
  10UL * 60 * 1000,   // RINSING
  10UL * 60 * 1000,   // RINSE_PURGE
  10UL * 60 * 1000,   // SANITIZE
  10UL * 60 * 1000,   // SANI_RETURN
  10UL * 60 * 1000,   // PRESSURE
  0,                  // FINISHED       — operator acknowledge
  0,                  // ERROR          — operator acknowledge
  2UL * 60 * 1000,    // STOPPING       — bounded evacuation (evac is ~20 s)
  0                   // HALTED          — operator acknowledge
};

static const unsigned long AIR_BURST_DURATION = 5000UL;

// Single source of truth for per-stage duration. Maps each operating state to
// its configured timer and applies the latched keg-size modifier. Used by the
// state handlers, the display countdown (KegDisplay), and the MQTT
// remaining-time publish (KegWasher.ino) so they can never disagree.
unsigned long stageTimerFor(byte state) {
  unsigned long base;
  switch (state) {
    case STATE_DIRTY_DRAIN:    base = dirtyDrainTimer; break;
    case STATE_DIRTY_RINSE:    base = dirtyRinseTimer; break;
    case STATE_DIRTY_PURGE:    base = dirtyPurgeTimer; break;
    case STATE_WASHING:        base = washTimer;       break;
    case STATE_CAUSTIC_RETURN: base = causticRtnTimer; break;
    case STATE_RINSING:        base = rinseTimer;      break;
    case STATE_RINSE_PURGE:    base = rinsePurgeTimer; break;
    case STATE_SANITIZE:       base = saniTimer;       break;
    case STATE_SANI_RETURN:    base = saniRtnTimer;    break;
    case STATE_PRESSURE:       base = purgeTimer;      break;
    default:                   return 0;
  }
  return timers_adjustForKegSize(base, kegSizeLatched);
}

// ---------- Shared UI / input helpers (de-dup) ----------
// Flashing alert banner. The five screens that show a blinking banner
// (NOT_READY / READY / HEATING / FINISHED / ERROR) each drove an identical
// 500 ms toggle + redraw; this centralizes the timing/visibility state.
// Only one screen is active at a time and each re-primes on its own (re)draw,
// so sharing the statics is safe.
//   smBannerPrime(): call right after a full (re)draw to paint it immediately.
//   smBannerTick():  call every loop to keep it blinking at `periodMs`.
static unsigned long smBannerLastMs  = 0;
static bool          smBannerVisible = false;

static void smBannerPrime(const char* text, word color) {
  smBannerVisible = true;
  smBannerLastMs  = millis();
  display_flashBanner(text, color, true);
}

static void smBannerTick(const char* text, word color, unsigned long periodMs = 500UL) {
  if (millis() - smBannerLastMs >= periodMs) {
    smBannerVisible = !smBannerVisible;
    display_flashBanner(text, color, smBannerVisible);
    smBannerLastMs = millis();
  }
}

// Edge detect on a (debounced) button flag. The caller owns `prev` and updates
// it once at the end of the handler, so rising and falling can both be queried
// in the same pass (state_error needs both).
static inline bool smRising(bool now, bool prev)  { return now && !prev; }
static inline bool smFalling(bool now, bool prev) { return !now && prev; }

// Bench-gated resource gate. Returns true when ok. On failure it sets the error
// code, shows it, transitions to ERROR, and returns false — the established
// enterState() precondition pattern, centralized. Sensor checks are skipped in
// BENCH_MODE (no plumbing wired), exactly like the legacy WASHING check.
static bool requireResource(bool ok, byte err) {
#ifndef BENCH_MODE
  if (!ok) {
    errorCode = err;
    display_showError(diagnostics_getErrorMessage(err));
    stateMachine_changeState(STATE_ERROR);
    return false;
  }
#else
  (void)ok; (void)err;
#endif
  return true;
}

// ---------- Init ----------
void stateMachine_init() {
  currentState = STATE_STARTUP;
  startupSubState = STARTUP_INIT;
  timers_resetStateTimer();
}

// ---------- Per-loop dispatch ----------
static void checkStateTimeout() {
  if (currentState >= NUM_STATES) return;
  unsigned long maxDur = stateMaxDuration[currentState];
  if (maxDur == 0) return;
  if (timers_getStateElapsed() > maxDur) {
    diagnostics_logEvent("State timeout");
    errorCode = ERR_STATE_TIMEOUT;
    stateMachine_changeState(STATE_ERROR);
  }
}

// Per-tick safety monitor for the operating states. Aborts to ERROR if the
// resource a stage is *actively using* drops out, or the enclosure overheats.
// Runs before the state handler so a fault on the final tick aborts rather than
// advancing the cycle. Sensor-dependent checks are bench-gated (you can't watch
// an unwired sensor); ESTOP/timeouts remain unconditional elsewhere. On the
// prototype (BENCH_MODE off) these go live.
static void monitorActiveResources() {
  if (currentState < STATE_OP_FIRST || currentState > STATE_OP_LAST) return;

#ifndef BENCH_MODE
  // Enclosure overtemp applies to every operating stage (also trips on a
  // faulted enclosure sensor, which reads as over-limit by design).
  if (hardware_getEnclosureTemp() >= MAX_ENCLOSURE_TEMP) {
    errorCode = ERR_ENCLOSURE_TEMP;
    display_showError(diagnostics_getErrorMessage(ERR_ENCLOSURE_TEMP));
    stateMachine_changeState(STATE_ERROR);
    return;
  }

  // Which resource is in use *right now*? (A stage that isn't using a resource
  // must not abort on its loss.)
  bool needAir = false, needWater = false, needCo2 = false, needCaustic = false;
  switch (currentState) {
    case STATE_DIRTY_DRAIN:    needAir   = (timers_getStateElapsed() < AIR_BURST_DURATION); break;
    case STATE_DIRTY_PURGE:
    case STATE_CAUSTIC_RETURN:
    case STATE_RINSE_PURGE:    needAir   = true; break;
    case STATE_DIRTY_RINSE:
    case STATE_RINSING:        needWater = true; break;
    case STATE_WASHING:        needCaustic = true; break;
    case STATE_SANI_RETURN:
    case STATE_PRESSURE:       needCo2   = true; break;
    default: break;  // SANITIZE recirculates only — nothing pressurized to lose
  }

  byte err = ERR_NONE;
  if      (needAir   && !isAirOk)                                    err = ERR_AIR_PRESSURE;
  else if (needWater && !isWaterOk)                                  err = ERR_WATER_PRESSURE;
  else if (needCo2   && !isCo2Ok)                                    err = ERR_CO2_PRESSURE;
  else if (needCaustic && hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) err = ERR_CAUSTIC_TEMP;

  if (err != ERR_NONE) {
    errorCode = err;
    display_showError(diagnostics_getErrorMessage(err));
    stateMachine_changeState(STATE_ERROR);
  }
#endif
}

void stateMachine_process() {
  // Paused mid-cycle: outputs are held safe (set at pause), the stage countdown
  // is frozen. Only the pause-timeout safety bound runs here; ESTOP is handled
  // in loop() independently.
  if (cyclePaused) {
    if (millis() - pauseStartMs > PAUSE_MAX_MS) {
      diagnostics_logEvent("Pause timeout");
      errorCode = ERR_PAUSE_TIMEOUT;
      stateMachine_changeState(STATE_ERROR);  // clears cyclePaused, sounds alarm
    }
    return;
  }

  checkStateTimeout();
  monitorActiveResources();

  switch (currentState) {
    case STATE_STARTUP:        state_startup();       break;
    case STATE_DIRTY_DRAIN:    state_dirtyDrain();    break;
    case STATE_DIRTY_RINSE:    state_dirtyRinse();    break;
    case STATE_DIRTY_PURGE:    state_dirtyPurge();    break;
    case STATE_WASHING:        state_washing();       break;
    case STATE_CAUSTIC_RETURN: state_causticReturn(); break;
    case STATE_RINSING:        state_rinsing();       break;
    case STATE_RINSE_PURGE:    state_rinsePurge();    break;
    case STATE_SANITIZE:       state_sanitize();      break;
    case STATE_SANI_RETURN:    state_saniReturn();    break;
    case STATE_PRESSURE:       state_pressure();      break;
    case STATE_FINISHED:       state_finished();      break;
    case STATE_ERROR:          state_error();         break;
    case STATE_STOPPING:       state_stopping();      break;
    case STATE_HALTED:         state_halted();        break;
    default:
      stateMachine_changeState(STATE_STARTUP);
      break;
  }
}

// ---------- Transition rules ----------
bool canTransitionTo(byte targetState) {
  // Operator STOP/RESTART can be invoked from any operating stage.
  if (targetState == STATE_STOPPING &&
      currentState >= STATE_OP_FIRST && currentState <= STATE_OP_LAST) return true;

  switch (currentState) {
    case STATE_STARTUP:        return targetState == STATE_DIRTY_DRAIN    || targetState == STATE_ERROR;
    case STATE_DIRTY_DRAIN:    return targetState == STATE_DIRTY_RINSE    || targetState == STATE_ERROR;
    case STATE_DIRTY_RINSE:    return targetState == STATE_DIRTY_PURGE    || targetState == STATE_ERROR;
    case STATE_DIRTY_PURGE:    return targetState == STATE_WASHING        || targetState == STATE_ERROR;
    case STATE_WASHING:        return targetState == STATE_CAUSTIC_RETURN || targetState == STATE_ERROR;
    case STATE_CAUSTIC_RETURN: return targetState == STATE_RINSING        || targetState == STATE_ERROR;
    case STATE_RINSING:        return targetState == STATE_RINSE_PURGE    || targetState == STATE_ERROR;
    case STATE_RINSE_PURGE:    return targetState == STATE_SANITIZE       || targetState == STATE_ERROR;
    case STATE_SANITIZE:       return targetState == STATE_SANI_RETURN    || targetState == STATE_ERROR;
    case STATE_SANI_RETURN:    return targetState == STATE_PRESSURE       || targetState == STATE_ERROR;
    case STATE_PRESSURE:       return targetState == STATE_FINISHED       || targetState == STATE_ERROR;
    case STATE_FINISHED:       return targetState == STATE_DIRTY_DRAIN    || targetState == STATE_STARTUP || targetState == STATE_ERROR;
    case STATE_ERROR:          return targetState == STATE_STARTUP;
    case STATE_STOPPING:       return targetState == STATE_HALTED || targetState == STATE_ERROR;
    case STATE_HALTED:         return targetState == STATE_STARTUP || targetState == STATE_ERROR;
    default:                   return false;
  }
}

// ---------- Per-state cleanup / entry ----------
static void exitState(byte s) {
  switch (s) {
    case STATE_STARTUP:
      hardware_setCausticHeater(false);
      break;
    case STATE_DIRTY_DRAIN:
      hardware_setDrain(false);
      hardware_setAir(false);
      break;
    case STATE_DIRTY_RINSE:
      hardware_setWater(false);
      hardware_setDrain(false);
      break;
    case STATE_DIRTY_PURGE:
      hardware_setAir(false);
      hardware_setDrain(false);
      break;
    case STATE_WASHING:
      hardware_setCaustic(false);
      hardware_setPump(false);
      break;
    case STATE_CAUSTIC_RETURN:
      hardware_setAir(false);
      hardware_setCaustic(false);
      break;
    case STATE_RINSING:
      hardware_setWater(false);
      hardware_setDrain(false);
      break;
    case STATE_RINSE_PURGE:
      hardware_setAir(false);
      hardware_setDrain(false);
      break;
    case STATE_SANITIZE:
      hardware_setSanitizer(false);
      hardware_setPump(false);
      break;
    case STATE_SANI_RETURN:
      hardware_setCo2(false);
      hardware_setSanitizer(false);
      break;
    case STATE_PRESSURE:
      hardware_setCo2(false);
      break;
    case STATE_STOPPING:
      // Turn off every evacuation output regardless of which kind ran.
      hardware_setAir(false);
      hardware_setDrain(false);
      hardware_setCaustic(false);
      hardware_setSanitizer(false);
      hardware_setCo2(false);
      hardware_setPump(false);
      break;
    case STATE_FINISHED:
    case STATE_ERROR:
      hardware_setAlarm(false);
      break;
  }
}

// Returns false if the state's preconditions failed and the caller should
// abort the transition (errorCode and an ERROR transition will already
// have been set).
static bool enterState(byte s) {
  switch (s) {
    case STATE_DIRTY_DRAIN:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setDrain(true);
      // Air burst (first 5 s) happens in state_dirtyDrain()
      return true;
    case STATE_DIRTY_RINSE:
      if (!requireResource(isWaterOk, ERR_WATER_PRESSURE)) return false;
      hardware_setWater(true);
      hardware_setDrain(true);
      return true;
    case STATE_DIRTY_PURGE:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setAir(true);
      hardware_setDrain(true);
      return true;
    case STATE_WASHING:
      if (!requireResource(hardware_getCausticTemp() >= MIN_CAUSTIC_TEMP,
                           ERR_CAUSTIC_TEMP)) return false;
      hardware_setCaustic(true);
      hardware_setPump(true);
      return true;
    case STATE_CAUSTIC_RETURN:
      // Air pushes caustic back to its reservoir via the caustic valve.
      // Chem must NEVER reach the drain — drain + pump stay OFF.
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setDrain(false);
      hardware_setPump(false);
      hardware_setCaustic(true);
      hardware_setAir(true);
      return true;
    case STATE_RINSING:
      if (!requireResource(isWaterOk, ERR_WATER_PRESSURE)) return false;
      hardware_setWater(true);
      hardware_setDrain(true);
      return true;
    case STATE_RINSE_PURGE:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setAir(true);
      hardware_setDrain(true);
      return true;
    case STATE_SANITIZE:
      // Sanitizer recirculates from its reservoir via the pump — no pressurized
      // resource to gate on at entry.
      hardware_setSanitizer(true);
      hardware_setPump(true);
      return true;
    case STATE_SANI_RETURN:
      // CO2 pushes sanitizer back to its reservoir via the sanitizer valve.
      // Chem must NEVER reach the drain — drain + pump stay OFF.
      if (!requireResource(isCo2Ok, ERR_CO2_PRESSURE)) return false;
      hardware_setDrain(false);
      hardware_setPump(false);
      hardware_setSanitizer(true);
      hardware_setCo2(true);
      return true;
    case STATE_PRESSURE:
      if (!requireResource(isCo2Ok, ERR_CO2_PRESSURE)) return false;
      hardware_setCo2(true);
      return true;
    case STATE_STOPPING:
      // Evacuate the keg to the correct destination. Heater + pump always off
      // (no dry run / no heat). Chem never reaches the drain.
      hardware_setCausticHeater(false);
      hardware_setPump(false);
      if (evacKind == EVAC_CAUSTIC) {        // air-blow caustic -> caustic reservoir
        hardware_setDrain(false);
        hardware_setCaustic(true);
        hardware_setAir(true);
      } else if (evacKind == EVAC_SANI) {    // CO2-blow sanitizer -> sani reservoir
        hardware_setDrain(false);
        hardware_setSanitizer(true);
        hardware_setCo2(true);
      } else if (evacKind == EVAC_WATER) {   // air-blow water/product -> drain
        hardware_setAir(true);
        hardware_setDrain(true);
      }
      // EVAC_NONE (e.g. STOP during PRESSURE — gas only): nothing to evacuate.
      return true;
    case STATE_HALTED:
      // De-energize everything; the controller + display stay alive. Silent.
      hardware_allStop();          // drops every output (sets alarm HIGH)...
      hardware_setAlarm(false);    // ...but a graceful STOP is silent
      return true;
    case STATE_FINISHED:
    case STATE_ERROR:
      hardware_setAlarm(true);
      return true;
    case STATE_STARTUP:
    default:
      return true;
  }
}

void stateMachine_changeState(byte newState) {
  if (!canTransitionTo(newState)) {
    diagnostics_logEvent("Invalid state transition");
    return;
  }

  exitState(currentState);
  currentState = newState;
  timers_resetStateTimer();
  cyclePaused = false;  // any state change clears a pause (e.g. ESTOP/fault while paused)

  if (newState == STATE_STARTUP) startupSubState = STARTUP_INIT;
  if (newState == STATE_ERROR)   washerInitialized = false;  // re-run pre-check after a fault

  if (!enterState(newState)) {
    // enterState already redirected to ERROR via a recursive
    // changeState call; nothing more to do.
    return;
  }

  diagnostics_logEvent(stateNames[newState]);
}

// ---------- Pause / resume ----------
void stateMachine_setPause(bool wantPaused) {
  // Only an in-progress cycle can pause; ignore elsewhere or if unchanged.
  if (currentState < STATE_OP_FIRST || currentState > STATE_OP_LAST) return;
  if (wantPaused == cyclePaused) return;

  if (wantPaused) {
    cyclePaused = true;
    pauseStartMs = millis();
    // Per-stage safe shutdown: exitState knows each stage's outputs, so the
    // pump can't run dry, CO2 can't keep charging, chem can't reach the drain.
    exitState(currentState);
    diagnostics_logEvent("Cycle paused");
  } else {
    cyclePaused = false;
    timers_shiftStateStart(millis() - pauseStartMs);  // don't count paused time
    diagnostics_logEvent("Cycle resumed");
    // Re-assert the stage's outputs. If a required resource dropped while
    // paused, enterState aborts to ERROR — correct and safe.
    enterState(currentState);
  }
}

void stateMachine_togglePause() { stateMachine_setPause(!cyclePaused); }

// ---------- Operator STOP / RESTART ----------
// What's in the keg during a given stage (decides the evacuation routing).
static byte evacKindForState(byte s) {
  switch (s) {
    case STATE_WASHING:
    case STATE_CAUSTIC_RETURN: return EVAC_CAUSTIC;
    case STATE_SANITIZE:
    case STATE_SANI_RETURN:    return EVAC_SANI;
    case STATE_DIRTY_DRAIN:
    case STATE_DIRTY_RINSE:
    case STATE_DIRTY_PURGE:
    case STATE_RINSING:
    case STATE_RINSE_PURGE:    return EVAC_WATER;
    default:                   return EVAC_NONE;  // PRESSURE: gas only, nothing to evacuate
  }
}

// Evacuation duration = the configured "normal drain" timer for that contents
// (keg-size scaled), not a fixed time.
static unsigned long evacDuration() {
  switch (evacKind) {
    case EVAC_CAUSTIC: return stageTimerFor(STATE_CAUSTIC_RETURN);  // causticRtnTimer
    case EVAC_SANI:    return stageTimerFor(STATE_SANI_RETURN);     // saniRtnTimer
    case EVAC_WATER:   return stageTimerFor(STATE_DIRTY_DRAIN);     // dirtyDrainTimer
    default:           return 0;
  }
}

void stateMachine_stop() {
  if (currentState < STATE_OP_FIRST || currentState > STATE_OP_LAST) return;
  evacKind = evacKindForState(currentState);
  hardware_setCausticHeater(false);  // immediate: heater off
  diagnostics_logEvent("STOP/DRAIN: draining keg");
  stateMachine_changeState(STATE_STOPPING);  // exitState(stage) + enterState(STOPPING) assert evac
}

// RESTART: re-run the current stage from the top of its timer. Same stage, same
// contents — no evacuation. If paused, resume (re-assert the stage outputs).
void stateMachine_restart() {
  if (currentState < STATE_OP_FIRST || currentState > STATE_OP_LAST) return;
  if (cyclePaused) {
    cyclePaused = false;
    if (!enterState(currentState)) return;  // re-assert; may abort to ERROR if a resource dropped
  }
  timers_resetStateTimer();                 // back to the beginning of this stage's timer
  diagnostics_logEvent("Stage restarted");
}

// ---------- STARTUP sub-states ----------
static void showHeatingProgress() {
  int currentTemp = hardware_getCausticTemp();
  int targetTemp  = OPTIMAL_CAUSTIC_TEMP;
  int pct = constrain(map(currentTemp, heatingStartTemp, targetTemp, 0, 100), 0, 100);

  if (!isHeaterActive) {
    display_showProgress("Heating", pct, currentTemp, targetTemp);
    return;
  }
  unsigned long elapsedMs = millis() - heatingStartTime;
  if (elapsedMs > 120000UL && currentTemp > heatingStartTemp) {
    int degPerMin = (int)((currentTemp - heatingStartTemp) * 60000UL / elapsedMs);
    if (degPerMin > 0) {
      int remainMin = (targetTemp - currentTemp) / degPerMin + 1;
      display_showProgress("Heating", pct, currentTemp, targetTemp, remainMin);
      return;
    }
  }
  display_showProgress("Heating", pct, currentTemp, targetTemp);
}

void state_startup() {
  switch (startupSubState) {
    case STARTUP_INIT:
      errorCode = ERR_NONE;
      diagnostics_logEvent("Startup: init");
      // Pre-check runs once, at power-up. After the washer is initialized,
      // returning here (e.g. parked via DRAIN after a cycle) skips straight to
      // READY — no repeat pre-check. A fault clears the flag (see changeState)
      // so the check re-runs after recovery.
      startupSubState = washerInitialized ? STARTUP_READY : STARTUP_NOT_READY;
      break;

    case STARTUP_NOT_READY: {
      bool waterOk = isWaterOk;
      bool airOk   = isAirOk;
      bool co2Ok   = isCo2Ok;
      bool estopOk = !isEstopActive;

      static bool prevWater = false, prevAir = false, prevCo2 = false, prevEstop = false;
      static bool drawn = false;

      bool changed = (!drawn || waterOk != prevWater || airOk != prevAir ||
                      co2Ok != prevCo2 || estopOk != prevEstop);
      if (changed) {
        display_showNotReady(waterOk, airOk, co2Ok, estopOk);
        drawn = true;
        prevWater = waterOk; prevAir = airOk; prevCo2 = co2Ok; prevEstop = estopOk;
        smBannerPrime("NOT READY", RED);
      }
      smBannerTick("NOT READY", RED);

      if (hardware_allSystemsGo()) {
        errorCode = ERR_NONE;
        drawn = false;
        prevWater = prevAir = prevCo2 = prevEstop = true;
        washerInitialized = true;       // pre-check passed — don't repeat it per cycle
        startupSubState = STARTUP_READY;
        diagnostics_logEvent("Systems ready");
      }
      break;
    }

    case STARTUP_READY: {
      static bool drawn = false;

      if (!drawn) {
        display_showReadyScreen();
        drawn = true;
        smBannerPrime("READY", GREEN);
      }
      smBannerTick("READY", GREEN);

      // Drop back to NOT_READY if a system goes offline
      if (!hardware_allSystemsGo()) {
        drawn = false;
        startupSubState = STARTUP_NOT_READY;
        diagnostics_logEvent("Systems lost");
        break;
      }

      if (isCycleStartPressed) {
        kegSizeLatched = isLargeKeg;
        diagnostics_logEvent(kegSizeLatched ? "Cycle start (LARGE)" : "Cycle start (SMALL)");
        drawn = false;
#ifdef BENCH_MODE
        startupSubState = STARTUP_INIT;
        stateMachine_changeState(STATE_DIRTY_DRAIN);
#else
        if (hardware_getCausticTemp() >= OPTIMAL_CAUSTIC_TEMP) {
          startupSubState = STARTUP_INIT;
          stateMachine_changeState(STATE_DIRTY_DRAIN);
        } else {
          startupSubState = STARTUP_HEATING;
          diagnostics_logEvent("Startup: heating");
        }
#endif
      }
      break;
    }

    case STARTUP_HEATING: {
      static bool heatingDrawn = false;
      static unsigned long lastHeatDisplayMs = 0;

      if (hardware_getCausticTemp() >= OPTIMAL_CAUSTIC_TEMP) {
        hardware_setCausticHeater(false);
        heatingDrawn = false;
        diagnostics_logEvent("Heating complete");
        startupSubState = STARTUP_INIT;
        stateMachine_changeState(STATE_DIRTY_DRAIN);
        return;
      }

      if (!isHeaterActive) {
        if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
          errorCode = ERR_CAUSTIC_LEVEL;
          display_showError("Low caustic level");
          stateMachine_changeState(STATE_ERROR);
          return;
        }
        hardware_setCausticHeater(true);
        heatingDrawn = false;
      }

      // Full screen redraw at 3s cadence; restore banner after each clear
      if (!heatingDrawn || millis() - lastHeatDisplayMs >= 3000UL) {
        showHeatingProgress();
        heatingDrawn = true;
        lastHeatDisplayMs = millis();
        smBannerPrime("HEATING", RED);
      }
      smBannerTick("HEATING", RED);
      break;
    }
  }
}

// ---------- Operating states ----------
// Valves for each stage are asserted in enterState() / cleared in exitState();
// these handlers just run the per-stage timer (stageTimerFor) and advance. The
// only in-handler valve nuance is DIRTY_DRAIN's 5 s start air burst.

void state_dirtyDrain() {
  // 5 s air burst at the start forcefully expels old product, then drain only.
  hardware_setAir(timers_getStateElapsed() < AIR_BURST_DURATION);
  if (timers_isStateDone(stageTimerFor(STATE_DIRTY_DRAIN))) {
    hardware_setAir(false);
    stateMachine_changeState(STATE_DIRTY_RINSE);
  }
}

void state_dirtyRinse() {
  if (timers_isStateDone(stageTimerFor(STATE_DIRTY_RINSE)))
    stateMachine_changeState(STATE_DIRTY_PURGE);
}

void state_dirtyPurge() {
  if (timers_isStateDone(stageTimerFor(STATE_DIRTY_PURGE)))
    stateMachine_changeState(STATE_WASHING);
}

void state_washing() {
  // Caustic-temp safety (per-tick, including the final tick) is handled by
  // monitorActiveResources() which runs before this handler — so by here the
  // temp is known-good or we've already aborted to ERROR.
  if (timers_isStateDone(stageTimerFor(STATE_WASHING)))
    stateMachine_changeState(STATE_CAUSTIC_RETURN);
}

void state_causticReturn() {
  // Air pushes caustic back to its reservoir (valves set in enterState).
  if (timers_isStateDone(stageTimerFor(STATE_CAUSTIC_RETURN)))
    stateMachine_changeState(STATE_RINSING);
}

void state_rinsing() {
  if (timers_isStateDone(stageTimerFor(STATE_RINSING)))
    stateMachine_changeState(STATE_RINSE_PURGE);
}

void state_rinsePurge() {
  if (timers_isStateDone(stageTimerFor(STATE_RINSE_PURGE)))
    stateMachine_changeState(STATE_SANITIZE);
}

void state_sanitize() {
  if (timers_isStateDone(stageTimerFor(STATE_SANITIZE)))
    stateMachine_changeState(STATE_SANI_RETURN);
}

void state_saniReturn() {
  // CO2 pushes sanitizer back to its reservoir (valves set in enterState).
  if (timers_isStateDone(stageTimerFor(STATE_SANI_RETURN)))
    stateMachine_changeState(STATE_PRESSURE);
}

void state_pressure() {
  if (timers_isStateDone(stageTimerFor(STATE_PRESSURE)))
    stateMachine_changeState(STATE_FINISHED);
}

void state_finished() {
  static bool drawn = false;
  static bool prevPress = false;

  if (!drawn) {
    display_showFinishedScreen();
    drawn = true;
    smBannerPrime("COMPLETE", GREEN);
  }
  smBannerTick("COMPLETE", GREEN);  // blink + alarm until the operator acts

  bool nowPressed = isCycleStartPressed;

  // One-shot throughput: a single START press silences the alarm AND launches
  // the next cycle. The operator swaps the keg while the alarm sounds, then
  // taps START once. Goes straight to DIRTY_DRAIN — no pre-check, no re-heat.
  if (smRising(nowPressed, prevPress)) {
    kegSizeLatched = isLargeKeg;  // re-latch in case the size switch moved for this keg
    diagnostics_logEvent(kegSizeLatched ? "Next cycle (LARGE)" : "Next cycle (SMALL)");
    drawn = false;
    prevPress = nowPressed;
    stateMachine_changeState(STATE_DIRTY_DRAIN);  // alarm off via exitState(FINISHED)
    return;
  }

  // DRAIN = stop: silence the alarm and park at READY (no new cycle).
  if (isManualDrainPressed) {
    drawn = false;
    prevPress = nowPressed;
    stateMachine_changeState(STATE_STARTUP);  // initialized → READY, no pre-check
    return;
  }

  prevPress = nowPressed;
}

static const char* errorBannerText(byte code) {
  switch (code) {
    case ERR_WATER_PRESSURE:  return "WATER FAULT";
    case ERR_AIR_PRESSURE:    return "AIR FAULT";
    case ERR_CO2_PRESSURE:    return "CO2 FAULT";
    case ERR_CAUSTIC_TEMP:    return "TEMP FAULT";
    case ERR_ENCLOSURE_TEMP:  return "OVERHEAT";
    case ERR_ESTOP:           return "ESTOP";
    case ERR_HEATING_TIMEOUT: return "HEAT TIMEOUT";
    case ERR_HEATING_RATE:    return "HEAT SLOW";
    case ERR_CAUSTIC_LEVEL:   return "CAUSTIC LOW";
    case ERR_STATE_TIMEOUT:   return "TIMEOUT";
    case ERR_SD_INIT:         return "SD FAIL";
    case ERR_CONFIG_FILE:     return "CONFIG FAIL";
    case ERR_HEATER_OVERTEMP: return "HEAT OVERTEMP";
    case ERR_SENSOR_FAULT:    return "SENSOR FAIL";
    case ERR_PAUSE_TIMEOUT:   return "PAUSE TIMEOUT";
    default:                  return "ERROR";
  }
}

void state_error() {
  static byte lastShown = 255;

  // Redraw full screen only when the error code changes
  if (errorCode != lastShown) {
    display_showError(diagnostics_getErrorMessage(errorCode));
    lastShown = errorCode;
    smBannerPrime(errorBannerText(errorCode), RED);
  }
  smBannerTick(errorBannerText(errorCode), RED);

  // Manual-drain (IO2) silences the alarm without resetting
  if (isManualDrainPressed) {
    hardware_setAlarm(false);
  }

  // Cycle-start (IO1): short press = silence alarm, long press = reset to STARTUP
  static bool prevPress = false;
  static unsigned long pressStartMs = 0;
  static bool longFired = false;
  const unsigned long LONG_PRESS_MS = 2000;

  bool nowPressed = isCycleStartPressed;

  if (smRising(nowPressed, prevPress)) {
    pressStartMs = millis();
    longFired = false;
  }

  if (nowPressed && !longFired && (millis() - pressStartMs) >= LONG_PRESS_MS) {
    longFired = true;
    errorCode = ERR_NONE;
    lastShown = 255;
    prevPress = nowPressed;
    stateMachine_changeState(STATE_STARTUP);
    return;
  }

  if (smFalling(nowPressed, prevPress) && !longFired) {
    hardware_setAlarm(false);
  }

  prevPress = nowPressed;
}

// ---------- STOP/DRAIN evacuation + HALTED ----------
void state_stopping() {
  static bool drawn = false;
  unsigned long evac = evacDuration();   // 0 = nothing to evacuate (e.g. PRESSURE)

  if (evac > 0) {
    if (!drawn) { display_showStopping(); drawn = true; }
    if (!timers_isStateDone(evac)) return;   // still evacuating
  }

  drawn = false;
  diagnostics_logEvent("Keg clear -> halt");
  stateMachine_changeState(STATE_HALTED);    // enterState(HALTED) de-energizes, silent
}

void state_halted() {
  static bool drawn = false;
  static bool prevPress = false;

  if (!drawn) { display_showHaltedScreen(); drawn = true; }

  bool nowPressed = isCycleStartPressed;
  if (smRising(nowPressed, prevPress)) {     // START recovers to READY
    drawn = false;
    prevPress = nowPressed;
    stateMachine_changeState(STATE_STARTUP); // initialized -> READY, no pre-check
    return;
  }
  prevPress = nowPressed;
}
