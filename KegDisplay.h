// ======================================================================
// KegDisplay.h - 4D Systems Goldelox 128x128 wrapper (ViSi-Genie over COM1)
// ======================================================================
#ifndef KEG_DISPLAY_H
#define KEG_DISPLAY_H

#include <Arduino.h>
#include "KegConfig.h"
#include <genieArduinoDEV.h>

extern Genie genie;

// Color constants — previously from Goldelox_Const4D.h. Kept so call sites
// in KegStateMachine.cpp that pass colors to display_flashBanner() still
// compile. The color argument is ignored at runtime (appearance is set in
// Workshop4); these values are never sent over the wire.
#ifndef RED
#define RED   0xF800u
#endif
#ifndef GREEN
#define GREEN 0x07E0u
#endif

// ── Form numbers ───────────────────────────────────────────────────────
// Workshop4 numbers forms zero-indexed (Form0 is the first form,
// confirmed against ViSi-Genie Reference Manual R 3.0).
// These map 1:1 to Workshop4 form numbers in the .4DGenie project.
#define FORM_STARTUP    0  // "Reading settings / Checking..." splash
#define FORM_NOT_READY  1  // Pre-flight failing
#define FORM_READY      2  // "Press START" — all systems go
#define FORM_HEATING    3  // Caustic heating progress
#define FORM_OPERATING  4  // Main cycle (draining → pressure)
#define FORM_FINISHED   5  // Cycle complete
#define FORM_ERROR      6  // Fault condition
#define FORM_MESSAGE    7  // Generic one-liner

// ── Object indices ─────────────────────────────────────────────────────
// Indices auto-increment per object type per form in Workshop4.
// All values are placeholders — update after form design is complete.

// FORM_OPERATING — LED_DIGITS
#define OBJ_OP_TIMER_MIN   0  // minutes remaining
#define OBJ_OP_TIMER_SEC   1  // seconds remaining
#define OBJ_OP_TEMP        2  // caustic temp °C
// FORM_OPERATING — USER_LED
#define OBJ_OP_WATER       0  // water pressure OK (lit = OK)
#define OBJ_OP_AIR         1  // air pressure OK
#define OBJ_OP_CO2         2  // CO2 pressure OK
#define OBJ_OP_ESTOP       3  // estop active (lit = tripped)
#define OBJ_OP_KEG         4  // large keg selected (lit = large)
#define OBJ_OP_MQTT        5  // MQTT connected
// FORM_OPERATING — STRINGS
#define OBJ_OP_STATE_STR   0  // state name
#define OBJ_OP_IP_STR      1  // IP address / "offline"

// FORM_HEATING — LED_DIGITS
#define OBJ_HT_CURR        0  // current temp °C
#define OBJ_HT_TARGET      1  // target temp °C
#define OBJ_HT_PCT         2  // percent complete 0-100
#define OBJ_HT_EST_MIN     3  // est. minutes remaining
#define OBJ_HT_LEVEL       4  // caustic level %
// FORM_HEATING — USER_LED
#define OBJ_HT_BANNER      0  // "HEATING" banner (flash by toggling)
// FORM_HEATING — STRINGS
#define OBJ_HT_IP_STR      1  // IP address footer

// FORM_NOT_READY — USER_LED
#define OBJ_NR_WATER       0  // water OK indicator
#define OBJ_NR_AIR         1  // air OK indicator
#define OBJ_NR_CO2         2  // CO2 OK indicator
#define OBJ_NR_ESTOP       3  // estop OK indicator (lit = closed/OK)
#define OBJ_NR_BANNER      4  // "NOT READY" banner (flash by toggling)

// FORM_READY — USER_LED
#define OBJ_RD_BANNER      0  // "READY" banner (flash by toggling)
// FORM_READY — STRINGS
#define OBJ_RD_IP_STR      1  // IP address footer

// FORM_FINISHED — USER_LED
#define OBJ_FN_COMPLETE    0  // "COMPLETE" banner (shown before alarm ack)
#define OBJ_FN_READY       1  // "READY" banner (shown after alarm ack)
// FORM_FINISHED — STRINGS
#define OBJ_FN_IP_STR      2  // IP address footer

// FORM_ERROR — STRINGS
#define OBJ_ER_MSG         0  // error message text
#define OBJ_ER_IP_STR      1  // IP address footer
// FORM_ERROR — USER_LED
#define OBJ_ER_BANNER      0  // banner (flash by toggling)

// FORM_MESSAGE — STRINGS
#define OBJ_MSG_TEXT       0  // message text
#define OBJ_MSG_IP_STR     1  // IP address footer

// ── Lifecycle ──────────────────────────────────────────────────────────
void display_init();
void display_showStartup();

// ── State screens ──────────────────────────────────────────────────────
// Operating states: call once on state entry, then call update fns below.
void display_showOperatingScreen();
void display_update();  // alias for display_showOperatingScreen()

// STARTUP sub-state screens
void display_showReadyScreen();    // STARTUP_READY "Check levels / Press START"
void display_showFinishedScreen(); // FINISHED "Cycle complete! / DRAIN / START"
void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk);

// Progress / error / info
void display_showProgress(const char* label, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes = -1);
void display_showError(const char* errorMsg);
void display_showMessage(const char* message);

// ── Partial updates (call from KegWasher.ino loop) ─────────────────────
void display_updateTimer(unsigned long elapsedMs);  // every 1 s during operating states
void display_updateStatus();                        // every 5 s during operating states

// ── Banner flash (500 ms interval, called from state machine) ──────────
// text + bgColor are ignored at runtime — appearance is set in Workshop4.
// Kept in signature so call sites in KegStateMachine.cpp are unchanged.
void display_flashBanner(const char* text, word bgColor, bool visible);

// ── Footer (IP + MQTT indicator) ───────────────────────────────────────
void display_updateFooter();

// ── Network state (defined in KegWasher.ino) ───────────────────────────
extern uint8_t kwLocalIP[4];
extern bool    kwEthernetReady;
extern volatile uint8_t kwHttpClients;
extern bool    kwMqttReady;

#endif // KEG_DISPLAY_H
