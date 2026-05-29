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

byte startupSubState = STARTUP_INIT;
bool kegSizeLatched  = false;  // set at START press; used by all timer calculations

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
  0,                  // STARTUP  — heater bounded internally; READY is operator-paced
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
#ifndef BENCH_MODE
      if (hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) {
        errorCode = ERR_CAUSTIC_TEMP;
        display_showError("Caustic temp too low");
        stateMachine_changeState(STATE_ERROR);
        return false;
      }
#endif
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
      startupSubState = STARTUP_NOT_READY;
      break;

    case STARTUP_NOT_READY: {
      bool waterOk = isWaterOk;
      bool airOk   = isAirOk;
      bool co2Ok   = isCo2Ok;
      bool estopOk = !isEstopActive;

      static bool prevWater = false, prevAir = false, prevCo2 = false, prevEstop = false;
      static bool drawn = false;
      static unsigned long lastBannerMs = 0;
      static bool bannerVisible = false;

      bool changed = (!drawn || waterOk != prevWater || airOk != prevAir ||
                      co2Ok != prevCo2 || estopOk != prevEstop);
      if (changed) {
        display_showNotReady(waterOk, airOk, co2Ok, estopOk);
        drawn = true;
        prevWater = waterOk; prevAir = airOk; prevCo2 = co2Ok; prevEstop = estopOk;
        display_flashBanner("NOT READY", RED, bannerVisible);
      }

      if (millis() - lastBannerMs >= 500UL) {
        bannerVisible = !bannerVisible;
        display_flashBanner("NOT READY", RED, bannerVisible);
        lastBannerMs = millis();
      }

      if (hardware_allSystemsGo()) {
        errorCode = ERR_NONE;
        drawn = false;
        prevWater = prevAir = prevCo2 = prevEstop = true;
        startupSubState = STARTUP_READY;
        diagnostics_logEvent("Systems ready");
      }
      break;
    }

    case STARTUP_READY: {
      static bool drawn = false;
      static unsigned long lastBannerMs = 0;
      static bool bannerVisible = false;

      if (!drawn) {
        display_showReadyScreen();
        drawn = true;
        lastBannerMs = millis();
        bannerVisible = true;
        display_flashBanner("READY", GREEN, true);
      }

      if (millis() - lastBannerMs >= 500UL) {
        bannerVisible = !bannerVisible;
        display_flashBanner("READY", GREEN, bannerVisible);
        lastBannerMs = millis();
      }

      // Drop back to NOT_READY if a system goes offline
      if (!hardware_allSystemsGo()) {
        drawn = false;
        bannerVisible = false;
        startupSubState = STARTUP_NOT_READY;
        diagnostics_logEvent("Systems lost");
        break;
      }

      if (isCycleStartPressed) {
        kegSizeLatched = isLargeKeg;
        diagnostics_logEvent(kegSizeLatched ? "Cycle start (LARGE)" : "Cycle start (SMALL)");
        drawn = false;
        bannerVisible = false;
#ifdef BENCH_MODE
        startupSubState = STARTUP_INIT;
        stateMachine_changeState(STATE_DRAINING);
#else
        if (hardware_getCausticTemp() >= OPTIMAL_CAUSTIC_TEMP) {
          startupSubState = STARTUP_INIT;
          stateMachine_changeState(STATE_DRAINING);
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
      static unsigned long lastBannerMs = 0;
      static bool bannerVisible = false;

      if (hardware_getCausticTemp() >= OPTIMAL_CAUSTIC_TEMP) {
        hardware_setCausticHeater(false);
        heatingDrawn = false;
        diagnostics_logEvent("Heating complete");
        startupSubState = STARTUP_INIT;
        stateMachine_changeState(STATE_DRAINING);
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
        display_flashBanner("HEATING", RED, bannerVisible);
      }

      // Flash HEATING banner at 500ms
      if (millis() - lastBannerMs >= 500UL) {
        bannerVisible = !bannerVisible;
        display_flashBanner("HEATING", RED, bannerVisible);
        lastBannerMs = millis();
      }
      break;
    }
  }
}

// ---------- Operating states ----------
void state_draining() {
  unsigned long drainTime = timers_adjustForKegSize(dirtyDrainTimer, kegSizeLatched);

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
  unsigned long rinseTime = timers_adjustForKegSize(rinseTimer, kegSizeLatched);

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
#ifndef BENCH_MODE
  // Temp check first: a low reading on the final tick should still abort
  // rather than letting the timer advance us into SANITIZE. Skipped in
  // bench mode since the caustic-temp sensor isn't wired.
  if (hardware_getCausticTemp() < MIN_CAUSTIC_TEMP) {
    errorCode = ERR_CAUSTIC_TEMP;
    display_showError("Low caustic temp");
    stateMachine_changeState(STATE_ERROR);
    return;
  }
#endif

  unsigned long washTime = timers_adjustForKegSize(washTimer, kegSizeLatched);
  if (timers_isStateDone(washTime)) {
    stateMachine_changeState(STATE_SANITIZE);
  }
}

void state_sanitize() {
  unsigned long saniTime = timers_adjustForKegSize(saniTimer, kegSizeLatched);
  if (timers_isStateDone(saniTime)) {
    stateMachine_changeState(STATE_PRESSURE);
  }
}

void state_pressure() {
  unsigned long purgeTime = timers_adjustForKegSize(purgeTimer, kegSizeLatched);
  if (timers_isStateDone(purgeTime)) {
    stateMachine_changeState(STATE_FINISHED);
  }
}

void state_finished() {
  static bool drawn = false;
  static bool alarmAcked = false;
  static bool prevPress = false;
  static unsigned long lastBannerMs = 0;
  static bool bannerVisible = false;

  if (!drawn) {
    display_showFinishedScreen();
    drawn = true;
    alarmAcked = false;
    lastBannerMs = millis();
    bannerVisible = true;
    display_flashBanner("COMPLETE", GREEN, true);
  }

  bool nowPressed = isCycleStartPressed;

  if (!alarmAcked) {
    // DRAIN button or first START press silences alarm and switches to READY banner
    if (isManualDrainPressed || (nowPressed && !prevPress)) {
      hardware_setAlarm(false);
      alarmAcked = true;
      display_flashBanner("READY", GREEN, true);
    } else if (millis() - lastBannerMs >= 500UL) {
      bannerVisible = !bannerVisible;
      display_flashBanner("COMPLETE", GREEN, bannerVisible);
      lastBannerMs = millis();
    }
  } else {
    // Alarm acked: next START press begins the next cycle
    if (nowPressed && !prevPress) {
      drawn = false;
      alarmAcked = false;
      stateMachine_changeState(STATE_STARTUP);
    }
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
    default:                  return "ERROR";
  }
}

void state_error() {
  static byte lastShown = 255;
  static unsigned long lastBannerMs = 0;
  static bool bannerVisible = false;

  // Redraw full screen only when the error code changes
  if (errorCode != lastShown) {
    display_showError(diagnostics_getErrorMessage(errorCode));
    lastShown = errorCode;
    display_flashBanner(errorBannerText(errorCode), RED, bannerVisible);
  }

  // Flash fault banner at 500ms
  if (millis() - lastBannerMs >= 500UL) {
    bannerVisible = !bannerVisible;
    display_flashBanner(errorBannerText(errorCode), RED, bannerVisible);
    lastBannerMs = millis();
  }

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

  if (nowPressed && !prevPress) {
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

  if (!nowPressed && prevPress && !longFired) {
    hardware_setAlarm(false);
  }

  prevPress = nowPressed;
}
