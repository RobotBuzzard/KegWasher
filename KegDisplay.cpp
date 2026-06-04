// ======================================================================
// KegDisplay.cpp - gen4-uLCD-43DT (Diablo16) wrapper, RAW SPE SERIAL backend.
// Implements the display_*() API on top of KegDisplaySerial (KDS::*). No
// ViSi-Genie. The Diablo16 takes ~4.5 s to boot after RESET (KDS::begin pulses
// RTS) before it accepts commands; display_init() runs before the watchdog is
// armed so that delay is fine.
// ======================================================================
#include "KegDisplay.h"
#include "KegDisplaySerial.h"
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegTimers.h"

// IP (or "offline") string for the footer.
static const char* ipStr() {
  static char buf[20];
  if (kwEthernetReady)
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
  else
    snprintf(buf, sizeof(buf), "offline");
  return buf;
}

// Remembered MQTT indicator state so a full screen redraw re-applies the dots.
static bool g_mqttUp = false, g_pub = false, g_sub = false;
static void reapplyMqtt() { KDS::mqttIndicators(g_mqttUp, g_pub, g_sub); }

void display_init() {
  KDS::begin();
  KDS::touchEnable();
  if (cfgTouchCalValid) {
    KDS::setTouchCalibration(cfgTouchCal[0], cfgTouchCal[1], cfgTouchCal[2],
                             cfgTouchCal[3], cfgTouchCal[4], cfgTouchCal[5]);
  }
  KDS::startup("KegWasher", "initializing...");
}

void display_showStartup() {
  KDS::startup("KegWasher", "starting up...");
  delay(2000);   // let the operator see the splash
}

void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk) {
  KDS::notReady(waterOk, airOk, co2Ok, estopOk, ipStr());
  reapplyMqtt();
}

void display_showReadyScreen()    { KDS::ready(ipStr());    reapplyMqtt(); }
void display_showFinishedScreen() { KDS::finished(ipStr()); reapplyMqtt(); }
void display_showError(const char* m)   { KDS::error(m, ipStr());   reapplyMqtt(); }
void display_showMessage(const char* m) { KDS::message(m, ipStr()); reapplyMqtt(); }

void display_showProgress(const char* /*label*/, int percentage,
                          int currentValue, int targetValue, int /*remainingMinutes*/) {
  KDS::heating(currentValue, targetValue, percentage, ipStr());
  reapplyMqtt();
}

void display_updateTimer(unsigned long elapsedMs) {
  unsigned long duration = 0;
  switch (currentState) {
    case STATE_DRAINING: duration = timers_adjustForKegSize(dirtyDrainTimer, kegSizeLatched); break;
    case STATE_RINSING:  duration = timers_adjustForKegSize(rinseTimer,      kegSizeLatched); break;
    case STATE_WASHING:  duration = timers_adjustForKegSize(washTimer,       kegSizeLatched); break;
    case STATE_SANITIZE: duration = timers_adjustForKegSize(saniTimer,       kegSizeLatched); break;
    case STATE_PRESSURE: duration = timers_adjustForKegSize(purgeTimer,      kegSizeLatched); break;
    default: return;
  }
  unsigned long remaining = (elapsedMs < duration) ? (duration - elapsedMs) : 0UL;
  KDS::operatingTimer((int)(remaining / 60000UL), (int)((remaining / 1000UL) % 60UL));
}

void display_updateStatus() {
  KDS::operatingStatus((int)hardware_getCausticTemp(),
                       isWaterOk, isAirOk, isCo2Ok, isEstopActive, kwMqttReady);
}

void display_showOperatingScreen() {
  KDS::operatingFrame(stateNames[currentState], ipStr());
  display_updateTimer(timers_getStateElapsed());
  display_updateStatus();
  reapplyMqtt();
}
void display_update() { display_showOperatingScreen(); }

void display_flashBanner(const char* text, word /*bgColor*/, bool visible) {
  KDS::banner(text, RED, visible);
}

void display_updateFooter() { KDS::footer(ipStr()); reapplyMqtt(); }

void display_setMqttIndicators(bool connected, bool pubActive, bool subActive) {
  g_mqttUp = connected; g_pub = pubActive; g_sub = subActive;
  KDS::mqttIndicators(connected, pubActive, subActive);
}

void display_doEvents() {
  // Pump touch every loop (replaces genie.DoEvents). No on-screen touch
  // actions are wired yet (the genie UI had none either) — map (x,y) to
  // on-screen buttons here when the touch UI is designed.
  int x, y;
  if (KDS::touch(&x, &y)) {
    // touch at (x,y) — hook for future button handling
  }
}
