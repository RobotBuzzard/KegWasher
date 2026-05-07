// ======================================================================
// KegWasher.ino - Main sketch (setup + loop)
// ======================================================================
// Brewery keg washer for Teknic ClearCore. See README.md for hardware,
// docs/state-table.md for cycle behavior, docs/io-table.md for pinout.
// ======================================================================
#include "KegConfig.h"
#include "KegHardware.h"
#include "KegStateMachine.h"
#include "KegDisplay.h"
#include "KegTimers.h"
#include "KegDiagnostics.h"
#include "KegUtils.h"

// ----- Serial logging -----
// USB Serial is only used for diagnostics_logEvent output. The CCIO
// expansion talks over COM0 and the Goldelox display talks over COM1 —
// neither uses USB.
static const unsigned long DIAG_SERIAL_BAUD = 115200;

// ----- Display refresh -----
// State handlers do their own display calls during transient phases
// (heating progress, air-burst messages). The standard status panel
// (state name / timer / temps) is refreshed here at a moderate cadence
// so the 9600-baud Goldelox link can keep up.
static const unsigned long DISPLAY_INTERVAL_MS = 250;
static unsigned long lastDisplayMs = 0;

void setup() {
  // Diagnostics first so any later init failure can be logged.
  Serial.begin(DIAG_SERIAL_BAUD);

  // Display before config, hardware, etc. — failures in those modules
  // call display_showError() and need somewhere to render. Goldelox boot
  // sequence is the longest single step in setup (~4 s).
  display_init();
  display_showStartup();

  config_init();
  hardware_init();
  timers_init();
  diagnostics_init();
  stateMachine_init();

  // First-pass system check. Any failure pushes us straight into the
  // ERROR state with the appropriate errorCode set by hardware_allSystemsGo.
  if (!hardware_allSystemsGo()) {
    stateMachine_changeState(STATE_ERROR);
  }

  diagnostics_logEvent("Boot complete");
}

void loop() {
  timers_update();
  hardware_readInputs();

  // ESTOP: the ISR has already killed safety-critical outputs. Here we
  // do the non-ISR-safe follow-up: log it, set the error code, and move
  // the state machine to ERROR. Doing this in the ISR would risk dead-
  // locking on Serial / CCIO transactions.
  if (hardware_consumeEstopFlag()) {
    hardware_allStop();
    errorCode = ERR_ESTOP;
    diagnostics_logEvent("E-STOP triggered");
    if (currentState != STATE_ERROR) {
      stateMachine_changeState(STATE_ERROR);
    }
  }

  stateMachine_process();

  // Only refresh the standard status panel during operating states.
  // STARTUP/FINISHED/ERROR each render their own bespoke screens via
  // display_showMessage / display_showProgress / display_showError, and
  // overlaying display_update on top would cause flicker.
  if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    switch (currentState) {
      case STATE_DRAINING:
      case STATE_RINSING:
      case STATE_WASHING:
      case STATE_SANITIZE:
      case STATE_PRESSURE:
        display_update();
        break;
      default:
        break;
    }
    lastDisplayMs = millis();
  }

  diagnostics_process();

  // 10 ms loop pacing keeps the debounce window stable and bounds CPU.
  // TODO: add hardware watchdog reset here once the ClearCore WDT API
  // is verified — today the long delay()s in diagnostics_runTest() would
  // trip a strict watchdog.
  delay(10);
}
