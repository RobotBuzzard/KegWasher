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
#include "KegDiagnostics.h"

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
static volatile bool g_touchRecover = false;  // RECOVER button on the ABORTED screen

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

// ── Settings editor (Phase 5) ─────────────────────────────────────────
// 11 editable params: the 10 stage timers + largeKegMod, paginated 4/page.
// Working copies are edited live; SAVE commits to the globals + WASHER.CFG.
#define S_BLACK 0x0000u
static const word S_GREY = 0x4208;          // disabled nav button
static const int  S_NUM = 11, S_PER_PAGE = 4, S_PAGES = 3;   // ceil(11/4)=3

static unsigned long sWkTimer[10];
static double        sWkMod;
static int           sPage = 0;

static unsigned long* const sTimerPtr[10] = {
  &dirtyDrainTimer, &dirtyRinseTimer, &dirtyPurgeTimer, &washTimer, &causticRtnTimer,
  &rinseTimer, &rinsePurgeTimer, &saniTimer, &saniRtnTimer, &purgeTimer
};
static const char* const sLabel[S_NUM] = {
  "DIRTY DRN", "DIRTY RNS", "DIRTY PRG", "WASH", "CAUS RTN",
  "RINSE", "RNS PURGE", "SANITIZE", "SANI RTN", "PRESSURE", "LG KEG x"
};
// Validation bounds — MUST match KegConfig.cpp (TIMER_MIN_MS/MAX, kegmod 1.0-3.0).
static const unsigned long S_TMIN = 5000UL, S_TMAX = 1800000UL, S_TSTEP = 5000UL;

// Layout (portrait 272x480).
static const int S_ROW_Y0 = 52, S_ROW_H = 64;
static const int S_MINUS_X = 150, S_PLUS_X = 210, S_BTN_W = 54, S_BTN_H = 50;
static const int S_NAV_Y = 318, S_NAV_W = 76, S_NAV_H = 40, S_NEXT_X = 188;
static const int S_ACT_Y = 404, S_ACT_W = 120, S_ACT_H = 44, S_CANCEL_X = 144;
// SETTINGS button on the READY screen.
static const int S_OPEN_X = 66, S_OPEN_Y = 406, S_OPEN_W = 140, S_OPEN_H = 42;

static void sFmt(int idx, char* buf, size_t n) {
  if (idx < 10) { snprintf(buf, n, "%lus", sWkTimer[idx] / 1000UL); return; }
  // largeKegMod, formatted without %f (newlib-nano printf may omit float support)
  int whole = (int)(sWkMod + 0.0001);
  int frac  = (int)((sWkMod - whole) * 10.0 + 0.5);
  if (frac >= 10) { whole++; frac = 0; }
  snprintf(buf, n, "%d.%dx", whole, frac);
}

static void sDrawValue(int slot, int idx) {
  int rowY = S_ROW_Y0 + slot * S_ROW_H;
  char v[12]; sFmt(idx, v, sizeof(v));
  KDS::fillRect(8, rowY + 32, 144, rowY + 58, S_BLACK);   // clear old value first
  KDS::text(10, rowY + 34, 2, AMBER, S_BLACK, v);
}

static void sDrawRow(int slot, int idx) {
  int rowY = S_ROW_Y0 + slot * S_ROW_H;
  KDS::text(8, rowY + 6, 2, WHITE, S_BLACK, sLabel[idx]);
  sDrawValue(slot, idx);
  KDS::button(S_MINUS_X, rowY + 7, S_BTN_W, S_BTN_H, "-", BLUE);
  KDS::button(S_PLUS_X,  rowY + 7, S_BTN_W, S_BTN_H, "+", BLUE);
}

static void sDraw() {
  KDS::screen("SETTINGS", AMBER);
  int first = sPage * S_PER_PAGE;
  for (int slot = 0; slot < S_PER_PAGE; slot++) {
    int idx = first + slot;
    if (idx >= S_NUM) break;
    sDrawRow(slot, idx);
  }
  KDS::button(8,        S_NAV_Y, S_NAV_W, S_NAV_H, "PREV", sPage > 0 ? BLUE : S_GREY);
  KDS::button(S_NEXT_X, S_NAV_Y, S_NAV_W, S_NAV_H, "NEXT", sPage < S_PAGES - 1 ? BLUE : S_GREY);
  char pg[8]; snprintf(pg, sizeof(pg), "P%d/%d", sPage + 1, S_PAGES);
  KDS::text(112, S_NAV_Y + 12, 2, WHITE, S_BLACK, pg);
  KDS::button(8,          S_ACT_Y, S_ACT_W, S_ACT_H, "SAVE",   GREEN);
  KDS::button(S_CANCEL_X, S_ACT_Y, S_ACT_W, S_ACT_H, "CANCEL", RED);
  KDS::footer(ipStr());
  reapplyMqtt();          // re-show live pub/sub dots (footer reset them to red)
}

static void sAdjust(int idx, int dir) {
  if (idx < 10) {
    long v = (long)sWkTimer[idx] + dir * (long)S_TSTEP;
    if (v < (long)S_TMIN) v = (long)S_TMIN;
    if (v > (long)S_TMAX) v = (long)S_TMAX;
    sWkTimer[idx] = (unsigned long)v;
  } else {
    double m = sWkMod + dir * 0.1;
    if (m < 1.0) m = 1.0;
    if (m > 3.0) m = 3.0;
    sWkMod = m;
  }
}

static void sExitToReady() {
  idleSub = IDLE_READY;          // state machine no-ops in SETTINGS; repaint READY here
  display_showReadyScreen();
}

static void sSave() {
  for (int i = 0; i < 10; i++) *sTimerPtr[i] = sWkTimer[i];
  largeKegMod = sWkMod;
  config_saveToSD();             // persists the full config (incl. these) to WASHER.CFG
  diagnostics_logEvent("Settings saved");
  sExitToReady();
}

static void sCancel() {
  diagnostics_logEvent("Settings cancelled");
  sExitToReady();                // working copies discarded (re-copied on next open)
}

void display_openSettings() {
  for (int i = 0; i < 10; i++) sWkTimer[i] = *sTimerPtr[i];
  sWkMod = largeKegMod;
  sPage  = 0;
  idleSub = IDLE_SETTINGS;       // state_idle no-ops; this screen owns the panel
  diagnostics_logEvent("Settings opened");
  sDraw();
}

void display_init() {
  KDS::begin();
  KDS::touchEnable();
  if (cfgTouchCalValid) {
    KDS::setTouchCalibration(cfgTouchCal[0], cfgTouchCal[1], cfgTouchCal[2],
                             cfgTouchCal[3], cfgTouchCal[4], cfgTouchCal[5]);
  }
  // The ONE boot screen — setup() fills in status rows on this same frame.
  KDS::screen("KEGWASHER", BLUE);
}

void display_showStartup() {
  KDS::startup("KegWasher", "starting up...");
  delay(2000);   // let the operator see the splash
}

// Single progressive boot screen (Linux-boot style). display_init() drew the
// "KEGWASHER" frame; setup() calls display_bootStatus() to fill one status row in
// as each subsystem comes up, then display_bootDone(). No splash chain.
void display_bootStatus(int row, const char* label, const char* value, word color) {
  int y = 74 + row * 34;
  KDS::text(8,   y,     2, WHITE, 0x0000u, label);   // label (size 2)
  KDS::text(158, y + 6, 1, color, 0x0000u, value);   // status (size 1)
}
void display_bootDone(bool ok) {
  KDS::text(16, 300, 3, ok ? GREEN : AMBER, 0x0000u, ok ? "SYSTEMS GO" : "CHECK SYS");
  KDS::footer(ipStr());   // IP shown here, so the NETWORK row can just say OK/OFFLINE
  reapplyMqtt();
}

void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk) {
  KDS::readiness(waterOk, airOk, co2Ok, estopOk, false, ipStr());  // amber, no START
  reapplyMqtt();
}

void display_showReadyScreen() {
  // Same unified screen, all-OK: green title + the live signal rows + START.
  KDS::readiness(isWaterOk, isAirOk, isCo2Ok, !isEstopActive, true, ipStr());
  KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  KDS::button(S_OPEN_X, S_OPEN_Y, S_OPEN_W, S_OPEN_H, "SETTINGS", BLUE);
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
void display_showError(const char* m) {
  KDS::error(m, ipStr());
  // RECOVER button (blue = operator action). Touch is gated to MACH_ABORTED in
  // display_doEvents and drained → stateMachine_reset() (which refuses while the
  // fault condition is still active). The flashing banner shows the hint.
  KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "RECOVER", BLUE);
  reapplyMqtt();
}
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
    display_flashBanner("PAUSED", AMBER, true);   // amber: operator hold, not a fault
    KDS::button(BTN_RESUME_X, BTN_RESUME_Y,  BTN_BAND_W, BTN_BAND_H, "RESUME",     GREEN);
    KDS::button(BTN_RESUME_X, BTN_RESTART_Y, BTN_BAND_W, BTN_BAND_H, "RESTART",    COL_PAUSE);
    KDS::button(BTN_RESUME_X, BTN_STOPDRN_Y, BTN_BAND_W, BTN_BAND_H, "STOP/DRAIN", RED);
  } else {
    KDS::button(BTN_PAUSE_X, BTN_PAUSE_Y, BTN_PAUSE_W, BTN_PAUSE_H, "PAUSE", COL_PAUSE);
  }
}

void display_showOperatingScreen() {
  // Title bar: GREEN while running (IEC normal), AMBER while held (abnormal/hold).
  KDS::operatingFrame(phaseNames[recipePhase], ipStr(),
                      (machineState == MACH_HELD) ? AMBER : GREEN);
  display_updateTimer(timers_getStateElapsed());
  display_updateStatus();
  drawCycleControls();
  reapplyMqtt();
}
void display_update() { display_showOperatingScreen(); }

// Called on a pause-state change: full redraw so the control buttons + banner
// (and, on resume, the status block the paused buttons overwrote) are correct.
void display_setPaused(bool /*paused*/) { display_showOperatingScreen(); }

void display_flashBanner(const char* text, word bgColor, bool visible) {
  // Honour the caller's colour — GREEN for READY/COMPLETE, RED for faults.
  // (Was hardcoded RED, which painted the green "READY" banner red.)
  KDS::banner(text, bgColor, visible);
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

  // The settings editor owns every touch while it's open (idleSub == SETTINGS).
  if (machineState == MACH_IDLE && idleSub == IDLE_SETTINGS) {
    int first = sPage * S_PER_PAGE;
    for (int slot = 0; slot < S_PER_PAGE; slot++) {
      int idx = first + slot;
      if (idx >= S_NUM) break;
      int rowY = S_ROW_Y0 + slot * S_ROW_H;
      if (inRect(x, y, S_MINUS_X, rowY + 7, S_BTN_W, S_BTN_H)) { sAdjust(idx, -1); sDrawValue(slot, idx); return; }
      if (inRect(x, y, S_PLUS_X,  rowY + 7, S_BTN_W, S_BTN_H)) { sAdjust(idx, +1); sDrawValue(slot, idx); return; }
    }
    if (inRect(x, y, 8,         S_NAV_Y, S_NAV_W, S_NAV_H)) { if (sPage > 0)           { sPage--; sDraw(); } return; }
    if (inRect(x, y, S_NEXT_X,  S_NAV_Y, S_NAV_W, S_NAV_H)) { if (sPage < S_PAGES - 1) { sPage++; sDraw(); } return; }
    if (inRect(x, y, 8,         S_ACT_Y, S_ACT_W, S_ACT_H)) { sSave();   return; }
    if (inRect(x, y, S_CANCEL_X, S_ACT_Y, S_ACT_W, S_ACT_H)) { sCancel(); return; }
    return;   // swallow stray touches while editing
  }

  bool startScreen = (machineState == MACH_COMPLETE) ||
                     (machineState == MACH_STOPPED) ||
                     (machineState == MACH_IDLE && idleSub == IDLE_READY);
  bool operating = (machineState == MACH_EXECUTE || machineState == MACH_HELD);
  bool held      = (machineState == MACH_HELD);

  if (startScreen && inRect(x, y, BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H)) {
    g_touchStart = true;
    KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", WHITE);  // press feedback
  } else if (machineState == MACH_IDLE && idleSub == IDLE_READY &&
             inRect(x, y, S_OPEN_X, S_OPEN_Y, S_OPEN_W, S_OPEN_H)) {
    display_openSettings();        // SETTINGS button on the READY screen
  } else if (machineState == MACH_ABORTED &&
             inRect(x, y, BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H)) {
    g_touchRecover = true;   // RECOVER on the fault screen → stateMachine_reset()
    KDS::button(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "...", WHITE);
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

// Drain a pending RECOVER-button touch (one-shot, ABORTED screen).
bool display_takeTouchRecover() {
  if (g_touchRecover) { g_touchRecover = false; return true; }
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
