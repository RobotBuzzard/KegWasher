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

// ── Lifecycle ──────────────────────────────────────────────────────────
void display_init();
void display_showStartup();

// ── State screens ──────────────────────────────────────────────────────
// Operating states: call once on state entry, then call the update fns below.
void display_showOperatingScreen();
void display_update();  // alias for display_showOperatingScreen()

void display_showReadyScreen();    // "Check levels / Press START"
void display_showFinishedScreen(); // "Cycle complete / DRAIN+START = new"
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

// ── Network state (defined in KegWasher.ino) ───────────────────────────
extern uint8_t kwLocalIP[4];
extern bool    kwEthernetReady;
extern volatile uint8_t kwHttpClients;
extern bool    kwMqttReady;

#endif // KEG_DISPLAY_H
