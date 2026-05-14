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
#include <ClearCoreWatchdog.h>     // RobotBuzzard/ClearCoreWatchdog

// ----- Serial logging -----
// USB Serial is only used for diagnostics_logEvent output. The CCIO
// expansion talks over COM0 and the Goldelox display talks over COM1 —
// neither uses USB.
static const unsigned long DIAG_SERIAL_BAUD = 115200;

// ----- Display refresh -----
// State handlers do their own display calls during transient phases
// (heating progress, air-burst messages). The standard status panel
// (state name / timer / temps) is refreshed here.
//
// 1000 ms cadence: a full display_update() at 9600 baud takes ~300-500 ms
// (clear + ~7 lines of text, plus Goldelox processing). At the previous
// 250 ms interval we were queuing redraws faster than they could finish,
// catching the screen mid-write — visible as a partial-frame "slow
// repeating refresh" of the status panel.
static const unsigned long DISPLAY_INTERVAL_MS = 1000;
static unsigned long lastDisplayMs = 0;

void setup() {
  // Diagnostics first so any later init failure can be logged.
  Serial.begin(DIAG_SERIAL_BAUD);

#ifdef BENCH_MODE
  // Loud, unmissable warning on the wire so a bench-mode build can
  // never silently end up running a real cleaner. See KegConfig.h.
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("**  BENCH_MODE ACTIVE — DO NOT SHIP THIS  **"));
  Serial.println(F("=============================================="));
#endif

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

  // Log the cause of the most recent reset for postmortem. 0x01=POR
  // (cold boot), 0x10=EXT (button or bossac SYSRESETREQ-equiv after
  // upload), 0x20=WDT (we hung — watchdog recovered us), 0x40=SYST
  // (software reset). Multiple bits can be set if causes pile up.
  {
    char buf[40];
    snprintf(buf, sizeof(buf), "Reset cause=0x%02X", Watchdog.lastResetCause());
    diagnostics_logEvent(buf);
  }

  // Arm the hardware watchdog last — display_init's 4 s of RTS/boot
  // delays and the SD-fail message delays are all over by now. From
  // here on, every loop iteration must kick within 8 seconds or the
  // chip resets. diagnostics_runTest() disables the WDT around its
  // ~10 s of output exercises and re-enables it on exit.
  Watchdog.enable();
  diagnostics_logEvent("Watchdog armed");
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

  // Kick the watchdog after all per-loop work is done. If anything
  // above hangs, the WDT will fire within 8 s and the bootloader
  // brings us back through setup() cleanly.
  Watchdog.kick();

  // 10 ms loop pacing keeps the debounce window stable and bounds CPU.
  delay(10);
}
