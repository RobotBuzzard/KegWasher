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

// Current screen id — mirrored to the retained MQTT topic kegwasher/screen
// (change-detected in mqtt_publishStatus) so the dashboard's panel replica
// follows what the panel actually shows instead of re-deriving it from state.
static const char* g_screen = "BOOT";
const char* display_currentScreen() { return g_screen; }

// On-screen START button (READY + FINISHED screens). A touch in this rect sets
// g_touchStart, which display_takeTouchStart() drains into isCycleStartPressed
// (same one-tick-pulse path as the physical button / MQTT start command).
static const int BTN_START_X = 18, BTN_START_Y = 180, BTN_START_W = 236, BTN_START_H = 150;
static volatile bool g_touchStart = false;
static volatile bool g_touchRecover = false;  // RECOVER button on the ABORTED screen

// Operating-screen cycle controls.
//  - Running: a single PAUSE button.
//  - Paused : three stacked buttons RESUME / RESTART / STOP-DRAIN.
// Touches arm the matching one-shot flags, drained in KegWasher.ino.
static const int BTN_PAUSE_X = 18, BTN_PAUSE_Y = 348, BTN_PAUSE_W = 236, BTN_PAUSE_H = 50;
static const int BTN_RESUME_X  = 18, BTN_RESUME_Y  = 288, BTN_BAND_W = 236, BTN_BAND_H = 44;
static const int BTN_RESTART_Y = 340;
static const int BTN_STOPDRN_Y = 392;
static const word COL_PAUSE = 0xFD20;  // orange
static volatile bool g_touchPause   = false;
static volatile bool g_touchRestart = false;
static volatile bool g_touchStop    = false;

// ── Settings editor (Phase 5) ─────────────────────────────────────────
// 14 editable params over 4 pages, plus a read-only INFO page: the 10 stage
// timers, largeKegMod, pauseMaxMs, and the two process temps (wash floor +
// heater target). maxCausticTemp is a safety limit — SD-only, deliberately
// NOT panel-editable. Working copies are edited live; SAVE commits to the
// globals + WASHER.CFG.
#define S_BLACK 0x0000u
static const word S_GREY = 0x4208;          // disabled nav button
static const int  S_NUM = 14, S_PER_PAGE = 4;
static const int  S_ITEM_PAGES = 4;             // ceil(14/4)
static const int  S_PAGES = S_ITEM_PAGES + 1;   // + trailing INFO page

static unsigned long sWkTimer[10];
static double        sWkMod;
static unsigned long sWkPauseMs;
static int           sWkMinT, sWkOptT;
static int           sPage = 0;

static unsigned long* const sTimerPtr[10] = {
  &dirtyDrainTimer, &dirtyRinseTimer, &dirtyPurgeTimer, &washTimer, &causticRtnTimer,
  &rinseTimer, &rinsePurgeTimer, &saniTimer, &saniRtnTimer, &purgeTimer
};
static const char* const sLabel[S_NUM] = {
  "DIRTY DRN", "DIRTY RNS", "DIRTY PRG", "WASH", "CAUS RTN",
  "RINSE", "RNS PURGE", "SANITIZE", "SANI RTN", "PRESSURE", "LG KEG x",
  "PAUSE MAX", "MIN TEMP", "HEAT TGT"
};
// Validation bounds — MUST match KegConfig.cpp (TIMER_MIN_MS/MAX, kegmod
// 1.0-3.0, PAUSE_MIN_MS/PAUSE_LIMIT_MS, TEMP_MIN_C). Temps additionally
// cross-clamp: MIN TEMP <= HEAT TGT <= maxCausticTemp (the SD-only cutoff).
static const unsigned long S_TMIN = 5000UL, S_TMAX = 1800000UL, S_TSTEP = 5000UL;
static const unsigned long S_PMIN = 60000UL, S_PMAX = 3600000UL, S_PSTEP = 60000UL;
static const int S_TEMP_MIN = 0;

// Layout (portrait 272x480, direction-C grid: content x=18..256).
static const int S_ROW_Y0 = 76, S_ROW_H = 58;
static const int S_MINUS_X = 146, S_PLUS_X = 204, S_BTN_W = 52, S_BTN_H = 44;
static const int S_NAV_Y = 330, S_NAV_W = 70, S_NAV_H = 40, S_NEXT_X = 186;
static const int S_ACT_Y = 400, S_ACT_W = 110, S_ACT_H = 44, S_CANCEL_X = 146;
// SETTINGS button on the READY screen.
static const int S_OPEN_X = 18, S_OPEN_Y = 392, S_OPEN_W = 236, S_OPEN_H = 44;
static const word S_SUBTX = 0x84B4;   // secondary text (palette SUB)

static void sFmt(int idx, char* buf, size_t n) {
  if (idx < 10)  { snprintf(buf, n, "%lus", sWkTimer[idx] / 1000UL); return; }
  if (idx == 11) { snprintf(buf, n, "%lum", sWkPauseMs / 60000UL);   return; }
  if (idx == 12) { snprintf(buf, n, "%dC",  sWkMinT);                return; }
  if (idx == 13) { snprintf(buf, n, "%dC",  sWkOptT);                return; }
  // largeKegMod, formatted without %f (newlib-nano printf may omit float support)
  int whole = (int)(sWkMod + 0.0001);
  int frac  = (int)((sWkMod - whole) * 10.0 + 0.5);
  if (frac >= 10) { whole++; frac = 0; }
  snprintf(buf, n, "%d.%dx", whole, frac);
}

static void sDrawValue(int slot, int idx) {
  int rowY = S_ROW_Y0 + slot * S_ROW_H;
  char v[12]; sFmt(idx, v, sizeof(v));
  KDS::fillRect(16, rowY + 18, 140, rowY + 50, S_BLACK);   // clear old value first
  KDS::label(18, rowY + 20, true, 1, 2, WHITE, S_BLACK, v);
}

static void sDrawRow(int slot, int idx) {
  int rowY = S_ROW_Y0 + slot * S_ROW_H;
  KDS::label(18, rowY + 2, false, 1, 1, S_SUBTX, S_BLACK, sLabel[idx]);
  sDrawValue(slot, idx);
  KDS::button(S_MINUS_X, rowY + 4, S_BTN_W, S_BTN_H, "-", BLUE);
  KDS::button(S_PLUS_X,  rowY + 4, S_BTN_W, S_BTN_H, "+", BLUE);
}

// Read-only INFO page (last settings page): build id, network, broker, config
// source. Pure text — no +/- rows, so the touch handler's row loop naturally
// skips it (first item index is past S_NUM).
static void sDrawInfo() {
  char b[40];
  int y = S_ROW_Y0;
  const char* lab[4] = { "FIRMWARE", "NETWORK", "BROKER", "CONFIG" };
  char v0[40], v2[40];
  snprintf(v0, sizeof(v0), "%s %s", __DATE__, __TIME__);
  snprintf(v2, sizeof(v2), "%s:%d %s", mqttBrokerIp, mqttBrokerPort,
           kwMqttReady ? "OK" : "OFFLINE");
  const char* val[4] = { v0, ipStr(), v2,
                         cfgLoadedFromSD ? "SD WASHER.CFG" : "COMPILED DEFAULTS" };
  (void)b;
  for (int i = 0; i < 4; i++) {
    KDS::label(18, y, false, 1, 1, S_SUBTX, S_BLACK, lab[i]);
    KDS::label(18, y + 20, true, 1, 1, WHITE, S_BLACK, val[i]);
    y += S_ROW_H;
  }
}

static void sDraw() {
  bool info = (sPage == S_ITEM_PAGES);
  g_screen = info ? "INFO" : "SETTINGS";
  KDS::screen(info ? "INFO" : "SETTINGS", AMBER);
  if (info) {
    sDrawInfo();
  } else {
    int first = sPage * S_PER_PAGE;
    for (int slot = 0; slot < S_PER_PAGE; slot++) {
      int idx = first + slot;
      if (idx >= S_NUM) break;
      sDrawRow(slot, idx);
    }
  }
  KDS::button(18,       S_NAV_Y, S_NAV_W, S_NAV_H, "PREV", sPage > 0 ? BLUE : S_GREY);
  KDS::button(S_NEXT_X, S_NAV_Y, S_NAV_W, S_NAV_H, "NEXT", sPage < S_PAGES - 1 ? BLUE : S_GREY);
  char pg[8]; snprintf(pg, sizeof(pg), "%d/%d", sPage + 1, S_PAGES);
  KDS::label(120, S_NAV_Y + 10, true, 1, 2, WHITE, S_BLACK, pg);
  KDS::buttonPrimary(18,   S_ACT_Y, S_ACT_W, S_ACT_H, "SAVE",   GREEN);
  KDS::button(S_CANCEL_X,  S_ACT_Y, S_ACT_W, S_ACT_H, "CANCEL", RED);
  KDS::footer(ipStr());
  reapplyMqtt();          // re-show live pub/sub dots (footer reset them to red)
}

static void sAdjust(int idx, int dir) {
  if (idx < 10) {
    long v = (long)sWkTimer[idx] + dir * (long)S_TSTEP;
    if (v < (long)S_TMIN) v = (long)S_TMIN;
    if (v > (long)S_TMAX) v = (long)S_TMAX;
    sWkTimer[idx] = (unsigned long)v;
  } else if (idx == 10) {
    double m = sWkMod + dir * 0.1;
    if (m < 1.0) m = 1.0;
    if (m > 3.0) m = 3.0;
    sWkMod = m;
  } else if (idx == 11) {
    long v = (long)sWkPauseMs + dir * (long)S_PSTEP;
    if (v < (long)S_PMIN) v = (long)S_PMIN;
    if (v > (long)S_PMAX) v = (long)S_PMAX;
    sWkPauseMs = (unsigned long)v;
  } else if (idx == 12) {
    int v = sWkMinT + dir;                 // wash floor: never above the target
    if (v < S_TEMP_MIN) v = S_TEMP_MIN;
    if (v > sWkOptT)    v = sWkOptT;
    sWkMinT = v;
  } else {                                 // idx == 13
    int v = sWkOptT + dir;                 // heater target: floor..safety cutoff
    if (v < sWkMinT)         v = sWkMinT;
    if (v > maxCausticTemp)  v = maxCausticTemp;
    sWkOptT = v;
  }
}

static void sExitToReady() {
  idleSub = IDLE_READY;          // state machine no-ops in SETTINGS; repaint READY here
  display_showReadyScreen();
}

static void sSave() {
  for (int i = 0; i < 10; i++) *sTimerPtr[i] = sWkTimer[i];
  largeKegMod = sWkMod;
  pauseMaxMs         = sWkPauseMs;
  minCausticTemp     = sWkMinT;
  optimalCausticTemp = sWkOptT;
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
  sWkMod     = largeKegMod;
  sWkPauseMs = pauseMaxMs;
  sWkMinT    = minCausticTemp;
  sWkOptT    = optimalCausticTemp;
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
  g_screen = "BOOT";
  KDS::startup("KegWasher", "starting up...");
  delay(2000);   // let the operator see the splash
}

// Single progressive boot screen (Linux-boot style). display_init() drew the
// "KEGWASHER" frame; setup() calls display_bootStatus() to fill one status row in
// as each subsystem comes up, then display_bootDone(). No splash chain.
void display_bootStatus(int row, const char* label, const char* value, word color) {
  int y = 84 + row * 30;
  KDS::label(18,  y, false, 1, 1, WHITE, 0x0000u, label);
  KDS::labelRight(256, y, true, 1, 1, color, 0x0000u, value);
}
void display_bootDone(bool ok) {
  KDS::label(18, 310, true, 2, 2, ok ? GREEN : AMBER, 0x0000u, ok ? "SYSTEMS GO" : "CHECK SYS");
  KDS::footer(ipStr());   // IP shown here, so the NETWORK row can just say OK/OFFLINE
  reapplyMqtt();
}

void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk) {
  g_screen = "NOT_READY";
  KDS::readiness(waterOk, airOk, co2Ok, estopOk, false, ipStr());  // amber, no START
  reapplyMqtt();
}

void display_showReadyScreen() {
  g_screen = "READY";
  // Same unified screen, all-OK: green title + the live signal rows + START.
  KDS::readiness(isWaterOk, isAirOk, isCo2Ok, !isEstopActive, true, ipStr());
  KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  KDS::button(S_OPEN_X, S_OPEN_Y, S_OPEN_W, S_OPEN_H, "SETTINGS", BLUE);
  reapplyMqtt();
}
void display_showFinishedScreen() {
  g_screen = "COMPLETE";
  KDS::finished(ipStr());
  KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  reapplyMqtt();
}
void display_showStopping() {
  g_screen = "STOPPING"; KDS::stopping(ipStr()); reapplyMqtt(); }
void display_showHaltedScreen() {
  g_screen = "STOPPED";
  KDS::halted(ipStr());
  KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", GREEN);
  reapplyMqtt();
}
void display_showError(const char* m) {
  g_screen = "ABORTED";
  KDS::error(m, ipStr());
  // RECOVER button (blue = operator action). Touch is gated to MACH_ABORTED in
  // display_doEvents and drained → stateMachine_reset() (which refuses while the
  // fault condition is still active). The flashing banner shows the hint.
  KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "RECOVER", BLUE);
  reapplyMqtt();
}
void display_showMessage(const char* m) {
  g_screen = "MESSAGE"; KDS::message(m, ipStr()); reapplyMqtt(); }

void display_showProgress(const char* /*label*/, int percentage,
                          int currentValue, int targetValue, int /*remainingMinutes*/) {
  g_screen = "HEATING";
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

// One-line operator description per recipe phase (indexed like phaseNames).
static const char* const phaseDescs[NUM_PHASES] = {
  "",
  "draining old product",  "pre-rinse to drain",     "air-purging rinse water",
  "caustic recirculating", "caustic back to tank",
  "rinsing to drain",      "air-purging rinse water",
  "sanitizer recirculating", "sanitizer back to tank",
  "CO2 charge - sealing keg"
};

void display_showOperatingScreen() {
  g_screen = (machineState == MACH_HELD) ? "PAUSED" : "OPERATING";
  // Edge strip: GREEN while running (IEC normal), AMBER while held.
  KDS::operatingFrame(phaseNames[recipePhase], phaseDescs[recipePhase],
                      (int)recipePhase, NUM_PHASES - 1, ipStr(),
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
      if (inRect(x, y, S_MINUS_X, rowY + 4, S_BTN_W, S_BTN_H)) { sAdjust(idx, -1); sDrawValue(slot, idx); return; }
      if (inRect(x, y, S_PLUS_X,  rowY + 4, S_BTN_W, S_BTN_H)) { sAdjust(idx, +1); sDrawValue(slot, idx); return; }
    }
    if (inRect(x, y, 18,        S_NAV_Y, S_NAV_W, S_NAV_H)) { if (sPage > 0)           { sPage--; sDraw(); } return; }
    if (inRect(x, y, S_NEXT_X,  S_NAV_Y, S_NAV_W, S_NAV_H)) { if (sPage < S_PAGES - 1) { sPage++; sDraw(); } return; }
    if (inRect(x, y, 18,        S_ACT_Y, S_ACT_W, S_ACT_H)) { sSave();   return; }
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
    KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "START", WHITE);  // press feedback
  } else if (machineState == MACH_IDLE && idleSub == IDLE_READY &&
             inRect(x, y, S_OPEN_X, S_OPEN_Y, S_OPEN_W, S_OPEN_H)) {
    display_openSettings();        // SETTINGS button on the READY screen
  } else if (machineState == MACH_ABORTED &&
             inRect(x, y, BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H)) {
    g_touchRecover = true;   // RECOVER on the fault screen → stateMachine_reset()
    KDS::buttonPrimary(BTN_START_X, BTN_START_Y, BTN_START_W, BTN_START_H, "...", WHITE);
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
