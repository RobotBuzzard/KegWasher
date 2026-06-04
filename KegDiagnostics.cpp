// ======================================================================
// KegDiagnostics.cpp
// ======================================================================
// diagnostics_logEvent() goes to USB Serial, not the operator display.
// The state machine and hardware modules call it on every transition and
// heating event — flooding the LCD with that would clobber the operator-
// facing UI. Connect a USB cable to the ClearCore for live log monitoring.
//
// Note: KegWasher.ino must call Serial.begin() in setup() for these logs
// to be visible. Without it the writes are discarded silently.
// ======================================================================
#include "KegDiagnostics.h"
#include "KegHardware.h"
#include "KegDisplay.h"
#include "KegStateMachine.h"
#include <ClearCoreWatchdog.h>    // RobotBuzzard/ClearCoreWatchdog

bool diagnosticMode = false;
byte errorCode = ERR_NONE;

void diagnostics_init() {
  diagnosticMode = false;
  errorCode = ERR_NONE;
}

void diagnostics_process() {
  // Diagnostic-mode entry is gated to safe states. Don't let an operator
  // accidentally jump into output-toggling mode mid-wash.
  bool safeToEnter = (currentState == STATE_STARTUP) || (currentState == STATE_FINISHED);

  if (!diagnosticMode && safeToEnter
      && isManualDrainPressed && isCycleStartPressed) {
    diagnosticMode = true;
    display_showMessage("DIAGNOSTIC MODE");

    // diagnostics_runTest() does ~10 s of delay()-driven output
    // exercises, plus the button-release wait below can hold for
    // as long as the operator does. Both exceed the 8 s WDT timeout,
    // so disable the watchdog around them and re-arm on exit.
    bool wdtWasEnabled = Watchdog.isEnabled();
    if (wdtWasEnabled) Watchdog.disable();

    diagnostics_runTest();

    // Block until both buttons are released so we don't immediately re-enter.
    while (isManualDrainPressed || isCycleStartPressed) {
      delay(50);
      hardware_readInputs();
    }

    if (wdtWasEnabled) Watchdog.enable();
  }

  if (diagnosticMode) {
    if (isManualDrainPressed) hardware_setDrain(true);
    else                      hardware_setDrain(false);

    if (isCycleStartPressed) {
      diagnosticMode = false;
      display_showMessage("Exiting diagnostics");
      delay(1000);
    }
  }
}

void diagnostics_runTest() {
  // Each display_showMessage() call updates the FORM_MESSAGE STRINGS object.
  // Detailed per-output progress is also logged via diagnostics_logEvent()
  // (USB Serial) for any connected monitor.

  display_showMessage("Testing outputs...");

  display_showMessage("Testing CO2...");
  hardware_setCo2(true);  delay(1000); hardware_setCo2(false);

  display_showMessage("Testing fan...");
  hardware_setCabinFan(255); delay(1000); hardware_setCabinFan(0);

  display_showMessage("Testing alarm...");
  hardware_setAlarm(true); delay(1000); hardware_setAlarm(false);

  display_showMessage("Testing drain...");
  hardware_setDrain(true); delay(1000); hardware_setDrain(false);

  display_showMessage("Testing water...");
  hardware_setWater(true); delay(1000); hardware_setWater(false);

  display_showMessage("Testing air...");
  hardware_setAir(true); delay(1000); hardware_setAir(false);

  display_showMessage("Testing caustic...");
  hardware_setCaustic(true); delay(1000); hardware_setCaustic(false);

  display_showMessage("Testing pump...");
  hardware_setPump(true); delay(1000); hardware_setPump(false);

  display_showMessage("Testing sanitizer...");
  hardware_setSanitizer(true); delay(1000); hardware_setSanitizer(false);

  // Heater is intentionally not exercised here — running it dry or cold-test
  // is unsafe. Use the regular STARTUP heating cycle to validate the heater.

  display_showMessage("Reading inputs...");
  hardware_readInputs();

  // Compact sensor summary — all key readings in one STRINGS object.
  char status[64];
  snprintf(status, sizeof(status),
    "W:%s A:%s C:%s ES:%s L:%d%% T:%d",
    isWaterOk     ? "OK" : "NO",
    isAirOk       ? "OK" : "NO",
    isCo2Ok       ? "OK" : "NO",
    isEstopActive ? "ACT" : "OK",
    hardware_getCausticLevel(),
    hardware_getCausticTemp()
  );
  display_showMessage(status);

  delay(3000);
  display_showMessage("Complete. START exits");
}

// Defined in KegWasher.ino. Sends one UDP packet to the LAN broadcast
// address if the network log mirror is up; no-op otherwise. We don't
// pull <Ethernet.h> in here just to declare it — extern is enough.
extern void net_log_send(const char* msg);

void diagnostics_logEvent(const char* eventMsg) {
  // Format once into a stack buffer so both sinks see identical bytes
  // and the network packet doesn't have a partial timestamp.
  char buf[160];
  int n = snprintf(buf, sizeof(buf), "%lu [%u] %s\n",
                   millis(), (unsigned)currentState, eventMsg);
  if (n < 0) return;
  if ((size_t)n >= sizeof(buf)) n = sizeof(buf) - 1;

  Serial.write(reinterpret_cast<const uint8_t*>(buf), n);

  // Mirror to UDP. Short-circuits cheaply if Ethernet isn't ready;
  // safe to call from anywhere except an ISR.
  net_log_send(buf);
}

const char* diagnostics_getErrorMessage(byte code) {
  switch (code) {
    case ERR_NONE:            return "No error";
    case ERR_SD_INIT:         return "SD card error";
    case ERR_CONFIG_FILE:     return "Config file error";
    case ERR_WATER_PRESSURE:  return "Water pressure low";
    case ERR_AIR_PRESSURE:    return "Air pressure low";
    case ERR_CO2_PRESSURE:    return "CO2 pressure low";
    case ERR_CAUSTIC_TEMP:    return "Caustic temp low";
    case ERR_ENCLOSURE_TEMP:  return "Cabinet overheated";
    case ERR_ESTOP:           return "Emergency stop";
    case ERR_INVALID_STATE:   return "Invalid state";
    case ERR_HEATING_TIMEOUT: return "Heater timeout";
    case ERR_HEATING_RATE:    return "Heater too slow";
    case ERR_CAUSTIC_LEVEL:   return "Caustic level low";
    case ERR_HEATER_OVERTEMP: return "Heater overtemp";
    case ERR_STATE_TIMEOUT:   return "State timeout";
    case ERR_SENSOR_FAULT:    return "Sensor fault";
    case ERR_PAUSE_TIMEOUT:   return "Paused too long";
    default:                  return "Unknown error";
  }
}
