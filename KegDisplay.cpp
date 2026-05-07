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

// Goldelox library wants a Stream*. ClearCore exposes Serial1 (Uart) on
// COM1; ConnectorCOM1 is the underlying SerialDriver, used here only for
// the RTS reset toggle.
Goldelox_Serial_4DLib Display(&Serial1);

void display_println(const char* s) {
  Display.print(' ');
  Display.println(s);
}

void display_println() {
  Display.println(' ');
}

void display_init() {
  Serial1.begin(9600);

  // Hardware reset via the COM1 RTS line
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);
  delay(1000);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
  delay(3000);

  Display.gfx_ScreenMode(2);   // portrait
  Display.SSTimeout(0);
  Display.SSSpeed(0);
  Display.SSMode(0);
  Display.gfx_BGcolour(BLACK);
  Display.gfx_Cls();
}

void display_showStartup() {
  display_clear();
  display_println("MadMoon Keg Washer\n");
  display_println("Reading settings");
  display_println("Checking Controls");
  display_println("Checking Temps");
  display_println("Checking Inputs\n");
  Display.print("Checking Outputs\n");
  delay(2000);
}

void display_update() {
  display_clear();

  display_println("MadMoon Keg Washer");
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
}

void display_showState(byte state) {
  Display.print("State: ");
  display_println(stateNames[state]);
}

void display_showError(const char* errorMsg) {
  display_clear();
  display_println("ERROR:");
  display_println(errorMsg);
  display_println("\nPress START to reset");
}

void display_showMessage(const char* message) {
  display_clear();
  display_println("MadMoon Keg Washer");
  display_println("");
  display_println(message);
}

void display_showProgress(const char* label, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes) {
  display_clear();
  display_println("MadMoon Keg Washer");
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
