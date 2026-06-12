// ======================================================================
// KegDisplay.h - gen4-uLCD-43DT (Diablo16) wrapper.
// Backend: raw SPE serial via KegDisplaySerial (KDS::*) over COM1 — NO
// ViSi-Genie. The display_*() API below is unchanged so existing callers
// (KegWasher.ino, KegStateMachine.cpp, KegDiagnostics.cpp) need no edits.
// ======================================================================
#ifndef KEG_DISPLAY_H
#define KEG_DISPLAY_H

#include <Arduino.h>
#include "KegConfig.h"

// Color constants kept so KegStateMachine.cpp's display_flashBanner() calls
// compile. With the serial backend the banner colour is honoured.
#ifndef RED
#define RED   0xF800u
#endif
#ifndef GREEN
#define GREEN 0x07E0u
#endif
#ifndef WHITE
#define WHITE 0xFFFFu
#endif
// IEC 60204-1 status palette: GREEN=normal/running, AMBER=abnormal/held,
// RED=fault/emergency, BLUE=mandatory operator action.
#ifndef AMBER
#define AMBER 0xFD20u
#endif
#ifndef BLUE
#define BLUE  0x001Fu
#endif

// ── Lifecycle ──────────────────────────────────────────────────────────
void display_init();              // brings up the panel + draws the single boot frame
void display_showStartup();       // (legacy splash; no longer called from setup)
// Single progressive boot screen: fill rows during setup(), then call _bootDone().
void display_bootStatus(int row, const char* label, const char* value, word color);
void display_bootDone(bool ok);

// ── State screens ──────────────────────────────────────────────────────
// Operating states: call once on state entry, then call the update fns below.
void display_showOperatingScreen();
void display_update();  // alias for display_showOperatingScreen()

void display_showReadyScreen();    // "Check levels / Press START"
void display_showFinishedScreen(); // "Cycle complete / DRAIN+START = new"
void display_showStopping();       // "DRAINING" — STOP/DRAIN evacuation in progress
void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk);

void display_showProgress(const char* label, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes = -1);
void display_showError(const char* errorMsg);
void display_showMessage(const char* message);

// ── Partial updates (call from KegWasher.ino loop) ─────────────────────
void display_updateTimer(unsigned long elapsedMs);  // every 1 s during operating states
void display_updateStatus();                        // every 5 s during operating states

// ── Banner flash (500 ms interval, called from the state machine) ──────
void display_flashBanner(const char* text, word bgColor, bool visible);

// ── Footer (IP) + MQTT pub/sub indicators ──────────────────────────────
void display_updateFooter();
void display_setMqttIndicators(bool connected, bool pubActive, bool subActive);

// ── Touch event pump — call every loop (replaces genie.DoEvents) ───────
void display_doEvents();
bool display_takeTouchStart();   // one-shot: a START button touch is pending
bool display_takeTouchSilence(); // one-shot: SILENCE (COMPLETE screen / MQTT cmd/silence)
void display_requestSilence();   // MQTT cmd/silence sets the same flag
bool display_takeTouchRecover(); // one-shot: a RECOVER button touch is pending (ABORTED)
bool display_takeTouchPause();   // one-shot: a PAUSE/RESUME button touch is pending
bool display_takeTouchRestart(); // one-shot: a RESTART button touch is pending (paused)
bool display_takeTouchStop();    // one-shot: a STOP/DRAIN button touch is pending (paused)

// Update the operating screen's paused overlay: a "PAUSED" banner + the
// PAUSE/RESUME button label. Call when the pause state changes.
void display_setPaused(bool paused);

// ── On-screen settings editor (Phase 5) ───────────────────────────────
// Enter from the READY screen's SETTINGS button. Edits working copies of the
// stage timers + largeKegMod; SAVE applies them + writes WASHER.CFG, CANCEL
// discards. Lives entirely in the display layer (idleSub = IDLE_SETTINGS); the
// state machine no-ops while it's open and READY repaints on exit.
void display_openSettings();
void display_updateStoppingTimer(unsigned long remainingMs);

// Id of the screen the panel is currently showing (BOOT/NOT_READY/READY/
// SETTINGS/INFO/HEATING/OPERATING/PAUSED/STOPPING/STOPPED/COMPLETE/ABORTED/
// MESSAGE) — mirrored to the retained kegwasher/screen topic.
const char* display_currentScreen();

// ── Network state (defined in KegWasher.ino) ───────────────────────────
extern uint8_t kwLocalIP[4];
extern bool    kwEthernetReady;
extern volatile uint8_t kwHttpClients;
extern bool    kwMqttReady;

#endif // KEG_DISPLAY_H
