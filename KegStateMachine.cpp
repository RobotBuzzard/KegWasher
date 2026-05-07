// ======================================================================
// KegStateMachine.cpp - State logic, transitions, and per-state timeouts
// ======================================================================
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegDisplay.h"
#include "KegTimers.h"
#include "KegDiagnostics.h"

volatile byte currentState = STATE_STARTUP;

// Indexed by state ID. STATE_ERROR is now 7 (was 9 in older code) so this
// array no longer needs gap padding to be safe.
const char* stateNames[NUM_STATES] = {
  "STARTUP",
  "DRAINING",
  "RINSING",
  "WASHING",
  "SANITIZE",
  "PRESSURE",
  "FINISHED",
  "ERROR"
};

static byte startupSubState = STARTUP_INIT;

// Hard upper bounds on time-in-state. Normal completion happens via
// timer-driven transitions; these are last-resort safety nets if a
// transition condition never triggers (sensor stuck, valve failure, etc.).
// 0 = no timeout (operator-acknowledged states).
static const unsigned long stateMaxDuration[NUM_STATES] = {
  16UL * 60 * 1000,   // STARTUP  — heater max (15 min) + buffer
  10UL * 60 * 1000,   // DRAINING
  10UL * 60 * 1000,   // RINSING
  20UL * 60 * 1000,   // WASHING  — longest stage with size modifier
  10UL * 60 * 1000,   // SANITIZE
  10UL * 60 * 1000,   // PRESSURE
  0,                  // FINISHED — operator acknowledge
  0                   // ERROR    — operator acknowledge
};

static const unsigned long AIR_BURST_DURATION = 5000UL;

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

void stateMachine_process() {
  checkStateTimeout();

  switch (currentState) {
    case STATE_STARTUP:  state_startup();  break;
    case STATE_DRAINING: state_draining(); break;
    case STATE_RINSING:  state_rinsing();  break;
    case STATE_WASHING:  state_washing();  break;
    case STATE_SANITIZE: state_sanitize(); break;
    case STATE_PRESSURE: state_pressure(); break;
    case STATE_FINISHED: state_finished(); break;
    case STATE_ERROR:    state_error();    break;
    default:
      stateMachine_changeState(STATE_STARTUP);
      break;
  }
}

// ---------- Transition rules ----------
bool canTransitionTo(byte targetState) {
  switch (currentState) {
    case STATE_STARTUP:  return targetState == STATE_DRAINING || targetState == STATE_ERROR;
    case STATE_DRAINING: return targetState == STATE_RINSING  || targetState == STATE_ERROR;
    case STATE_RINSING:  return targetState == STATE_WASHING  || targetState == STATE_ERROR;
    case STATE_WASHING:  return targetState == STATE_SANITIZE || targetState == STATE_ERROR;
    case STATE_SANITIZE: return targetState == STATE_PRESSURE || targetState == STATE_ERROR;
    case STATE_PRESSURE: return targetState == STATE_FINISHED || targetState == STATE_ERROR;
    case STATE_FINISHED: return targetState == STATE_STARTUP  || targetState == STATE_ERROR;
    case STATE_ERROR:    return targetState == STATE_STARTUP;
    default:             return false;
  }
}

// ---------- Per-state cleanup / entry ----------
static void exitState(byte s) {
  switch (s) {
    case STATE_STARTUP:
      hardware_setCausticHeater(false);
      break;
    case STATE_DRAINING:
      hardware_setDrain(false);
      hardware_setAir(false);
      break;
    case STATE_RINSING:
      hardware_setWater(false);
      hardware_setDrain(false);
      hardware_setAir(false);
      break;
    case STATE_WASHING:
      hardware_setCaustic(false);
      hardware_setPump(false);
      break;
    case STATE_SANITIZE:
      hardware_setSanitizer(false);
      hardware_setPump(false);
      break;
    case STATE_PRESSURE:
      hardware_setCo2(false);
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
    case STATE_DRAINING:
      hardware_setDrain(true);
      // Air burst happens in state_draining()
      return true;
    case STATE_RINSING:
      hardware_setWater(true);
      hardware_setDrain(true);
      return true;
    case STATE_WASHING:
      if (hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) {
        errorCode = ERR_CAUSTIC_TEMP;
        display_showError("Caustic temp too low");
        stateMachine_changeState(STATE_ERROR);
        return false;
      }
      hardware_setCaustic(true);
      hardware_setPump(true);
      return true;
    case STATE_SANITIZE:
      hardware_setSanitizer(true);
      hardware_setPump(true);
      return true;
    case STATE_PRESSURE:
      hardware_setCo2(true);
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

  if (newState == STATE_STARTUP) startupSubState = STARTUP_INIT;

  if (!enterState(newState)) {
    // enterState already redirected to ERROR via a recursive
    // changeState call; nothing more to do.
    return;
  }

  diagnostics_logEvent(stateNames[newState]);
}

// ---------- STARTUP sub-states ----------
static void showHeatingProgress() {
  int currentTemp = hardware_getCausticTemp();
  int targetTemp  = OPTIMAL_CAUSTIC_TEMP;
  int pct = constrain(map(currentTemp, heatingStartTemp, targetTemp, 0, 100), 0, 100);

  // Without an active heater there's no meaningful elapsed time to base
  // an ETA on — show progress only.
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
      display_showMessage("Initializing...");
      diagnostics_logEvent("Startup: init");
      errorCode = ERR_NONE;
      startupSubState = STARTUP_HEATING;
      break;

    case STARTUP_HEATING:
      if (hardware_getCausticTemp() < OPTIMAL_CAUSTIC_TEMP) {
        if (!isHeaterActive) {
          if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
            errorCode = ERR_CAUSTIC_LEVEL;
            display_showError("Low caustic level");
            stateMachine_changeState(STATE_ERROR);
            return;
          }
          hardware_setCausticHeater(true);
          display_showMessage("Heating caustic...");
        }
        showHeatingProgress();
      } else {
        hardware_setCausticHeater(false);
        startupSubState = STARTUP_IO_CHECK;
        diagnostics_logEvent("Heating complete");
      }
      break;

    case STARTUP_IO_CHECK:
      display_showMessage("Checking systems...");
      if (hardware_allSystemsGo()) {
        startupSubState = STARTUP_READY;
        diagnostics_logEvent("Systems ready");
      } else {
        diagnostics_logEvent("Systems check failed");
        stateMachine_changeState(STATE_ERROR);
      }
      break;

    case STARTUP_READY:
      display_showMessage("Press START to begin");
      if (isCycleStartPressed) {
        diagnostics_logEvent("Cycle start");
        startupSubState = STARTUP_INIT;
        stateMachine_changeState(STATE_DRAINING);
      }
      break;
  }
}

// ---------- Operating states ----------
void state_draining() {
  unsigned long drainTime = isLargeKeg
                          ? timers_adjustForKegSize(dirtyDrainTimer)
                          : dirtyDrainTimer;

  if (timers_getStateElapsed() < AIR_BURST_DURATION) {
    hardware_setAir(true);
  } else {
    hardware_setAir(false);
  }

  if (timers_isStateDone(drainTime)) {
    hardware_setAir(false);
    stateMachine_changeState(STATE_RINSING);
  }
}

void state_rinsing() {
  unsigned long rinseTime = isLargeKeg
                          ? timers_adjustForKegSize(rinseTimer)
                          : rinseTimer;

  unsigned long elapsed    = timers_getStateElapsed();
  unsigned long burstStart = (rinseTime > AIR_BURST_DURATION)
                           ? (rinseTime - AIR_BURST_DURATION)
                           : 0;

  if (elapsed < burstStart) {
    hardware_setWater(true);
    hardware_setDrain(true);
    hardware_setAir(false);
  } else if (elapsed < rinseTime) {
    hardware_setWater(false);
    hardware_setDrain(true);
    hardware_setAir(true);
  }

  if (timers_isStateDone(rinseTime)) {
    hardware_setWater(false);
    hardware_setDrain(false);
    hardware_setAir(false);
    stateMachine_changeState(STATE_WASHING);
  }
}

void state_washing() {
  // Temp check first: a low reading on the final tick should still abort
  // rather than letting the timer advance us into SANITIZE.
  if (hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) {
    errorCode = ERR_CAUSTIC_TEMP;
    display_showError("Low caustic temp");
    stateMachine_changeState(STATE_ERROR);
    return;
  }

  unsigned long washTime = isLargeKeg
                         ? timers_adjustForKegSize(washTimer)
                         : washTimer;
  if (timers_isStateDone(washTime)) {
    stateMachine_changeState(STATE_SANITIZE);
  }
}

void state_sanitize() {
  unsigned long saniTime = isLargeKeg
                         ? timers_adjustForKegSize(saniTimer)
                         : saniTimer;
  if (timers_isStateDone(saniTime)) {
    stateMachine_changeState(STATE_PRESSURE);
  }
}

void state_pressure() {
  unsigned long purgeTime = isLargeKeg
                          ? timers_adjustForKegSize(purgeTimer)
                          : purgeTimer;
  if (timers_isStateDone(purgeTime)) {
    stateMachine_changeState(STATE_FINISHED);
  }
}

void state_finished() {
  if (isCycleStartPressed) {
    stateMachine_changeState(STATE_STARTUP);
  }
}

void state_error() {
  display_showError(diagnostics_getErrorMessage(errorCode));
  if (isCycleStartPressed) {
    errorCode = ERR_NONE;
    stateMachine_changeState(STATE_STARTUP);
  }
}
