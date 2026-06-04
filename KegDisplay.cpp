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

// On-screen START button (READY + FINISHED screens). A touch in this rect sets
// g_touchStart, which display_takeTouchStart() drains into isCycleStartPressed
// (same one-tick-pulse path as the physical button / MQTT start command).
static const int BTN_START_X = 46, BTN_START_Y = 300, BTN_START_W = 180, BTN_START_H = 90;
static volatile bool g_touchStart = false;

// Operating-screen cycle controls.
//  - Running: a single PAUSE button.
//  - Paused : three stacked buttons RESUME / RESTART / STOP-DRAIN.
// Touches arm the matching one-shot flags, drained in KegWasher.ino.
static const int BTN_PAUSE_X = 46, BTN_PAUSE_Y = 330, BTN_PAUSE_W = 180, BTN_PAUSE_H = 55;
static const int BTN_RESUME_X  = 8, BTN_RESUME_Y  = 262, BTN_BAND_W = 256, BTN_BAND_H = 42;
static const int BTN_RESTART_Y = 310;
static const int BTN_STOPDRN_Y = 358;
static const word COL_PAUSE = 0xFD20;  // orange
static volatile bool g_touchPause   = false;
static volatile bool g_touchRestart = false;
static volatile bool g_touchStop    = false;

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

void display_showReadyScreen() {
  KDS::ready(ipStr());
  KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  reapplyMqtt();
}
void display_showFinishedScreen() {
  KDS::finished(ipStr());
  KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  reapplyMqtt();
}
void display_showStopping() { KDS::stopping(ipStr()); reapplyMqtt(); }
void display_showHaltedScreen() {
  KDS::halted(ipStr());
  KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  reapplyMqtt();
}
void display_showError(const char* m)   { KDS::error(m, ipStr());   reapplyMqtt(); }
void display_showMessage(const char* m) { KDS::message(m, ipStr()); reapplyMqtt(); }

void display_showProgress(const char* /*label*/, int percentage,
                          int currentValue, int targetValue, int /*remainingMinutes*/) {
  KDS::heating(currentValue, targetValue, percentage, ipStr());
  reapplyMqtt();
}

void display_updateTimer(unsigned long elapsedMs) {
  unsigned long duration = stageTimerFor(recipePhase);
  if (duration == 0) return;  // not a recipe phase — nothing to count down
  unsigned long remaining = (elapsedMs < duration) ? (duration - elapsedMs) : 0UL;
  // Ceiling, not floor: with a 1 Hz redraw, floor division makes the countdown
  // skip a value (e.g. 5,3,2,1,0). Rounding up makes it tick every second.
  unsigned long remainingSec = (remaining + 999UL) / 1000UL;
  KDS::operatingTimer((int)(remainingSec / 60UL), (int)(remainingSec % 60UL));
}

void display_updateStatus() {
  KDS::operatingStatus((int)hardware_getCausticTemp(),
                       isWaterOk, isAirOk, isCo2Ok, isEstopActive, kwMqttReady);
}

// Draw the cycle-control buttons for the current pause state.
//  - Running: one PAUSE button.
//  - Paused : RESUME / RESTART / STOP-DRAIN, with a PAUSED banner.
static void drawCycleControls() {
  if (machineState == MACH_HELD) {
    display_flashBanner("PAUSED", RED, true);
    KDS::button(BTN_RESUME_X, BTN_RESUME_Y,  BTN_BAND_W, BTN_BAND_H, "RESUME",     GREEN);
    KDS::button(BTN_RESUME_X, BTN_RESTART_Y, BTN_BAND_W, BTN_BAND_H, "RESTART",    COL_PAUSE);
    KDS::button(BTN_RESUME_X, BTN_STOPDRN_Y, BTN_BAND_W, BTN_BAND_H, "STOP/DRAIN", RED);
  } else {
    KDS::button(BTN_PAUSE_X, BTN_PAUSE_Y, BTN_PAUSE_W, BTN_PAUSE_H, "PAUSE", COL_PAUSE);
  }
}

void display_showOperatingScreen() {
  KDS::operatingFrame(phaseNames[recipePhase], ipStr());
  display_updateTimer(timers_getStateElapsed());
  display_updateStatus();
  drawCycleControls();
  reapplyMqtt();
}
void display_update() { display_showOperatingScreen(); }

// Called on a pause-state change: full redraw so the control buttons + banner
// (and, on resume, the status block the paused buttons overwrote) are correct.
void display_setPaused(bool /*paused*/) { display_showOperatingScreen(); }

void display_flashBanner(const char* text, word /*bgColor*/, bool visible) {
  KDS::banner(text, RED, visible);
}

void display_updateFooter() { KDS::footer(ipStr()); reapplyMqtt(); }

void display_setMqttIndicators(bool connected, bool pubActive, bool subActive) {
  g_mqttUp = connected; g_pub = pubActive; g_sub = subActive;
  KDS::mqttIndicators(connected, pubActive, subActive);
}

// Hit-test: is (x,y) inside rect (rx,ry,rw,rh)?
static inline bool inRect(int x, int y, int rx, int ry, int rw, int rh) {
  return x >= rx && x <= rx + rw && y >= ry && y <= ry + rh;
}

void display_doEvents() {
  // Pump touch every loop (replaces genie.DoEvents).
  //  - READY / FINISHED / HALTED: a tap in START arms a cycle start / recover.
  //  - Operating, running: PAUSE toggles pause.
  //  - Operating, paused: RESUME / RESTART / STOP-DRAIN.
  int x, y;
  if (!KDS::touch(&x, &y)) return;

  bool startScreen = (machineState == MACH_COMPLETE) ||
                     (machineState == MACH_STOPPED) ||
                     (machineState == MACH_IDLE && idleSub == IDLE_READY);
  bool operating = (machineState == MACH_EXECUTE || machineState == MACH_HELD);
  bool held      = (machineState == MACH_HELD);

  if (startScreen && inRect(x, y, BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H)) {
    g_touchStart = true;
    KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", WHITE);  // press feedback
  } else if (operating && !held) {
    if (inRect(x, y, BTN_PAUSE_X, BTN_PAUSE_Y, BTN_PAUSE_W, BTN_PAUSE_H)) {
      g_touchPause = true;
      KDS::button(BTN_PAUSE_X, BTN_PAUSE_Y, BTN_PAUSE_W, BTN_PAUSE_H, "...", WHITE);
    }
  } else if (operating && held) {
    if (inRect(x, y, BTN_RESUME_X, BTN_RESUME_Y, BTN_BAND_W, BTN_BAND_H)) {
      g_touchPause = true;  // RESUME = toggle pause off
      KDS::button(BTN_RESUME_X, BTN_RESUME_Y, BTN_BAND_W, BTN_BAND_H, "...", WHITE);
    } else if (inRect(x, y, BTN_RESUME_X, BTN_RESTART_Y, BTN_BAND_W, BTN_BAND_H)) {
      g_touchRestart = true;
      KDS::button(BTN_RESUME_X, BTN_RESTART_Y, BTN_BAND_W, BTN_BAND_H, "...", WHITE);
    } else if (inRect(x, y, BTN_RESUME_X, BTN_STOPDRN_Y, BTN_BAND_W, BTN_BAND_H)) {
      g_touchStop = true;
      KDS::button(BTN_RESUME_X, BTN_STOPDRN_Y, BTN_BAND_W, BTN_BAND_H, "...", WHITE);
    }
  }
}

// Drain a pending START-button touch (one-shot). Called from KegWasher.ino's
// mqtt_applyCmdFlags() and injected into isCycleStartPressed.
bool display_takeTouchStart() {
  if (g_touchStart) { g_touchStart = false; return true; }
  return false;
}

// Drain a pending PAUSE/RESUME-button touch (one-shot).
bool display_takeTouchPause() {
  if (g_touchPause) { g_touchPause = false; return true; }
  return false;
}

// Drain a pending RESTART-button touch (one-shot).
bool display_takeTouchRestart() {
  if (g_touchRestart) { g_touchRestart = false; return true; }
  return false;
}

// Drain a pending STOP/DRAIN-button touch (one-shot).
bool display_takeTouchStop() {
  if (g_touchStop) { g_touchStop = false; return true; }
  return false;
}
