// ======================================================================
// KegDisplay.cpp - Goldelox driver wrapper, screen layout
// ======================================================================
// Init dependency: KegWasher.ino calls display_init() *before* config_init()
// and stateMachine_init() so error messages from those have a screen to
// land on. The Goldelox boot sequence takes ~4s (RTS toggle + boot delay),
// which dominates total cold-start time.
// ======================================================================
#include "KegDisplay.h"
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegTimers.h"

// MCU-side baud-rate handler used by Display.setbaudWait().
//
// The library's default hwSetBaudRateHndl calls Serial1.end() → PortClose(),
// which on ClearCore *switches RTS from output to input* (see
// SerialBase::PortClose() at libClearCore/src/SerialBase.cpp:125). RTS is
// wired to the Goldelox RESET line — when it floats, the display resets
// and reboots at its 9600 default while the MCU has already moved to the
// new rate. Result: silent baud mismatch and a dead display until power
// cycle. Bench-confirmed failure mode 2026-05-14.
//
// ConnectorCOM1.Speed() changes the SERCOM baud rate via PortDisable
// (peripheral-only) rather than PortClose (peripheral + RTS-as-input),
// keeping RTS driven the whole time. The two-arg Goldelox_Serial_4DLib
// constructor below routes setbaudWait through this callback instead of
// the library's default.
static void displaySetBaudRate(unsigned long newRate) {
  Serial1.flush();
  ConnectorCOM1.Speed(newRate);
  delay(50);  // Goldelox sleeps ~100 ms while switching; GetAck handles the rest
}

Goldelox_Serial_4DLib Display(&Serial1, displaySetBaudRate);

void display_println(const char* s) {
  Display.print(' ');
  Display.println(s);
}

void display_println() {
  Display.println(' ');
}

void display_init() {
  // Goldelox boots at 9600 default after RTS reset; bring up the link
  // there, then negotiate up to 115200 to drop full-screen redraw time
  // from ~400 ms (UART-bound at 9600) to ~35 ms.
  Serial1.begin(9600);

  // Hardware reset via the COM1 RTS line
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);
  delay(1000);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
  delay(3000);

  // Bump the link to 115200. Routes through displaySetBaudRate() above
  // — the library's default handler closes Serial1 and that floats RTS,
  // which the Goldelox interprets as RESET. See the comment block on
  // displaySetBaudRate for the full forensics.
  Display.setbaudWait(BAUD_115200);

  Display.gfx_ScreenMode(2);   // portrait
  Display.SSTimeout(0);
  Display.SSSpeed(0);
  Display.SSMode(0);
  Display.gfx_BGcolour(BLACK);
  Display.gfx_Cls();
}

void display_showStartup() {
  display_clear();
  display_println("Robot Keg Washer");
  display_println();
#ifdef BENCH_MODE
  display_println("** BENCH MODE **");
  display_println();
#endif
  display_println("Reading settings");
  display_println("Checking Controls");
  display_println("Checking Temps");
  display_println("Checking Inputs");
  display_println();
  display_println("Checking Outputs");
  delay(2000);
}

void display_update() {
  display_clear();

  display_println("Robot Keg Washer");
  Display.print("Mode: ");
  display_println(stateNames[currentState]);

  Display.print("Keg: ");
  display_println(isLargeKeg ? "LARGE" : "SMALL");

  display_showTimer(timers_getStateElapsed());
  display_showStatus(isWaterOk, isAirOk, isCo2Ok, isEstopActive, hardware_getCausticTemp());

  if (currentState == STATE_STARTUP) {
    Display.print("Caustic Lvl: ");
    Display.print(hardware_getCausticLevel());
    display_println("%");
  }
  display_footer();
}

void display_showState(byte state) {
  Display.print("State: ");
  display_println(stateNames[state]);
}

void display_showError(const char* errorMsg) {
  display_clear();
  display_println("ERROR:");
  display_println(errorMsg);
  // Embedded "\n" inside a single println re-triggers the Goldelox first-
  // char drop on whatever follows the newline, defeating the wrapper's
  // sacrificial space. Split the blank line + the prompt into their own
  // calls so each is independently protected.
  // Default Goldelox text size in portrait fits ~17 chars/line. Split
  // the prompt so it doesn't get truncated, and document both buttons.
  display_println();
  display_println("DRAIN: silence");
  display_println("Hold START: reset");
  display_footer();
}

void display_showMessage(const char* message) {
  display_clear();
  display_println("Robot Keg Washer");
  display_println("");
  display_println(message);
  display_footer();
}

void display_showProgress(const char* label, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes) {
  display_clear();
  display_println("Robot Keg Washer");
  display_println("");
  display_println(label);

  Display.print(currentValue);
  Display.print("/");
  Display.print(targetValue);
  display_println("C");

  Display.print(percentage);
  display_println("% complete");

  // Progress bar (100px wide on a 128px display)
  const int barW = 100, barH = 10, barX = 14, barY = 60;
  Display.gfx_Rectangle(barX, barY, barX + barW, barY + barH, WHITE);
  int fillW = map(percentage, 0, 100, 0, barW);
  Display.gfx_RectangleFilled(barX + 1, barY + 1, barX + fillW - 1, barY + barH - 1, WHITE);

  if (remainingMinutes >= 0) {
    Display.print("Est. time: ");
    Display.print(remainingMinutes);
    display_println(" min");
  }

  Display.print("Level: ");
  Display.print(hardware_getCausticLevel());
  display_println("%");
  display_footer();
}

void display_showTimer(unsigned long elapsedTime) {
  unsigned long duration = 0;
  switch (currentState) {
    case STATE_DRAINING: duration = isLargeKeg ? timers_adjustForKegSize(dirtyDrainTimer) : dirtyDrainTimer; break;
    case STATE_RINSING:  duration = isLargeKeg ? timers_adjustForKegSize(rinseTimer)      : rinseTimer; break;
    case STATE_WASHING:  duration = isLargeKeg ? timers_adjustForKegSize(washTimer)       : washTimer; break;
    case STATE_SANITIZE: duration = isLargeKeg ? timers_adjustForKegSize(saniTimer)       : saniTimer; break;
    case STATE_PRESSURE: duration = isLargeKeg ? timers_adjustForKegSize(purgeTimer)      : purgeTimer; break;
    default: return;
  }

  unsigned long remaining = (elapsedTime < duration) ? (duration - elapsedTime) : 0;
  unsigned long seconds = remaining / 1000;

  Display.print("Time: ");
  Display.print(seconds / 60);
  Display.print(":");
  if ((seconds % 60) < 10) Display.print("0");
  Display.println(seconds % 60);   // numeric end-of-line; not first char of a line
}

void display_showStatus(bool waterStatus, bool airStatus, bool co2Status,
                        bool estopStatus, int causticTemp) {
  Display.print("Temp: ");
  Display.print(causticTemp);
  display_println("C");

  Display.print("Water: ");
  display_println(waterStatus ? "OK" : "FAIL");

  Display.print("Air: ");
  display_println(airStatus ? "OK" : "FAIL");

  Display.print("CO2: ");
  display_println(co2Status ? "OK" : "FAIL");

  if (estopStatus) display_println("ESTOP ACTIVE!");
}

void display_clear() {
  Display.gfx_Cls();
}

void display_footer() {
  char buf[20];
  if (kwEthernetReady) {
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             kwLocalIP[0], kwLocalIP[1],
             kwLocalIP[2], kwLocalIP[3]);
  } else {
    snprintf(buf, sizeof(buf), "offline");
  }
  display_println(buf);

  // Status icons in the bottom-right corner. Overlays via the text
  // character grid with the default Goldelox font (cell height appears
  // to be ~12 px in this build, giving ~10 visible rows on the 128x128
  // panel — row 9 is a safe last-on-screen line). Column 14 of the
  // 16-column-wide grid leaves the rightmost two cells for a 2-char
  // indicator. Drawn AFTER the IP println so the text cursor advance
  // from println doesn't disturb the overlay.
  //
  // VIOLET "M*" = MQTT log mirror is connected to the broker. (The
  // named PURPLE 0x8010 is half-brightness and tends to disappear on
  // camera against a green-tinted enclosure; VIOLET reads as purple
  // to the eye but stays visible.) Absence means we're either offline
  // or the broker is unreachable — in either case kwMqttReady was
  // cleared by mqtt_loop().
  if (kwMqttReady) {
    Display.txt_MoveCursor(9, 14);
    Display.txt_FGcolour(VIOLET);
    Display.putstr((char *)"M*");
    Display.txt_FGcolour(WHITE);
  }
}
