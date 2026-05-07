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
    display_clear();
    Display.println("DIAGNOSTIC MODE");
    diagnostics_runTest();

    // Block until both buttons are released so we don't immediately re-enter.
    // This blocks the main loop for as long as the operator holds the
    // buttons; tolerable in this mode since no cycle is in progress.
    while (isManualDrainPressed || isCycleStartPressed) {
      delay(50);
      hardware_readInputs();
    }
  }

  if (diagnosticMode) {
    if (isManualDrainPressed) hardware_setDrain(true);
    else                      hardware_setDrain(false);

    if (isCycleStartPressed) {
      diagnosticMode = false;
      display_clear();
      Display.println("Exiting diagnostics");
      delay(1000);
    }
  }
}

void diagnostics_runTest() {
  display_clear();
  Display.println("Testing outputs...");

  Display.println("Testing CO2...");
  hardware_setCo2(true);  delay(1000); hardware_setCo2(false);

  Display.println("Testing fan...");
  hardware_setCabinFan(255); delay(1000); hardware_setCabinFan(0);

  Display.println("Testing alarm...");
  hardware_setAlarm(true); delay(1000); hardware_setAlarm(false);

  Display.println("Testing drain...");
  hardware_setDrain(true); delay(1000); hardware_setDrain(false);

  Display.println("Testing water...");
  hardware_setWater(true); delay(1000); hardware_setWater(false);

  Display.println("Testing air...");
  hardware_setAir(true); delay(1000); hardware_setAir(false);

  Display.println("Testing caustic...");
  hardware_setCaustic(true); delay(1000); hardware_setCaustic(false);

  Display.println("Testing pump...");
  hardware_setPump(true); delay(1000); hardware_setPump(false);

  Display.println("Testing sanitizer...");
  hardware_setSanitizer(true); delay(1000); hardware_setSanitizer(false);

  // Heater is intentionally not exercised here — running it dry or cold-test
  // is unsafe. Use the regular STARTUP heating cycle to validate the heater.

  Display.println("Reading inputs...");
  hardware_readInputs();

  display_clear();
  Display.println("Input status:");
  Display.print("Water: ");        Display.println(isWaterOk ? "OK" : "FAIL");
  Display.print("Air: ");          Display.println(isAirOk ? "OK" : "FAIL");
  Display.print("CO2: ");          Display.println(isCo2Ok ? "OK" : "FAIL");
  Display.print("ESTOP: ");        Display.println(isEstopActive ? "ACTIVE" : "INACTIVE");
  Display.print("Keg size: ");     Display.println(isLargeKeg ? "LARGE" : "SMALL");
  Display.print("Caustic temp: "); Display.println(hardware_getCausticTemp());
  Display.print("Caustic lvl: ");  Display.println(hardware_getCausticLevel());
  Display.print("Enclosure temp: ");Display.println(hardware_getEnclosureTemp());

  Display.println("\nTest complete");
  Display.println("Press START to exit");
}

void diagnostics_logEvent(const char* eventMsg) {
  Serial.print(millis());
  Serial.print(" [");
  Serial.print((int)currentState);
  Serial.print("] ");
  Serial.println(eventMsg);
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
    default:                  return "Unknown error";
  }
}
