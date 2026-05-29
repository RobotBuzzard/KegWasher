// ======================================================================
// KegDisplay.h - 4D Systems gen4-uLCD-43DT (Diablo16) wrapper, ViSi-Genie over COM1
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
// ViSi-Genie indexes objects GLOBALLY per object-type across the whole
// project, NOT restarting at 0 per form. (This is the opposite of the
// Goldelox mental model and was the source of a whole class of "writes to
// the wrong widget" bugs.) The values below were derived 2026-05-29 by
// enumerating KegWasher.4DGenie in declaration order — see docs comment at
// the end of this block for the full map. If you add/remove/reorder widgets
// in Workshop4, RE-ENUMERATE — every index downstream of the change shifts.
//
// Verified-by-alias entries are solid. Entries marked (UNVERIFIED-POS) point
// at the last unnamed STRINGS object on a form — almost certainly the footer
// slot, but its on-screen POSITION has not been confirmed in Workshop4. See
// the OBJ_*_IP_STR note below.

// FORM_NOT_READY (Form1) — USER_LED 0..4
#define OBJ_NR_WATER       0  // water OK indicator
#define OBJ_NR_AIR         1  // air OK indicator
#define OBJ_NR_CO2         2  // CO2 OK indicator
#define OBJ_NR_ESTOP       3  // estop OK indicator (lit = closed/OK)
#define OBJ_NR_BANNER      4  // "NOT READY" banner (flash by toggling)
// FORM_NOT_READY has no STRINGS object → no footer (see OBJ_NO_FOOTER)

// FORM_READY (Form2) — USER_LED 5, STRINGS 1
#define OBJ_RD_BANNER      5  // "READY" banner (flash by toggling)
#define OBJ_RD_IP_STR      1  // IP footer (alias "IP-footer" — CONFIRMED)

// FORM_HEATING (Form3) — USER_LED 6, LED_DIGITS 0..4, STRINGS 2..3
#define OBJ_HT_BANNER      6  // "HEATING" banner (flash by toggling)
#define OBJ_HT_CURR        0  // current temp °C
#define OBJ_HT_TARGET      1  // target temp °C
#define OBJ_HT_PCT         2  // percent complete 0-100
#define OBJ_HT_EST_MIN     3  // est. minutes remaining
#define OBJ_HT_LEVEL       4  // caustic level %
#define OBJ_HT_IP_STR      3  // (UNVERIFIED-POS) spare Strings3; Strings2 = HT-status

// FORM_OPERATING (Form4) — LED_DIGITS 5..7, USER_LED 7..12, STRINGS 4..5
#define OBJ_OP_TIMER_MIN   5  // minutes remaining
#define OBJ_OP_TIMER_SEC   6  // seconds remaining
#define OBJ_OP_TEMP        7  // caustic temp °C
#define OBJ_OP_WATER       7  // water pressure OK (Userled7, unnamed — by position)
#define OBJ_OP_AIR         8  // air pressure OK  (Userled8, unnamed — by position)
#define OBJ_OP_CO2         9  // CO2 pressure OK  (Userled9, unnamed — by position)
#define OBJ_OP_ESTOP      10  // estop active     (Userled10, unnamed — by position)
#define OBJ_OP_KEG        11  // large keg selected (alias "Keg" — CONFIRMED)
#define OBJ_OP_MQTT       12  // MQTT connected     (alias "MQTT" — CONFIRMED)
#define OBJ_OP_STATE_STR   4  // state name (alias "State-name" — CONFIRMED)
#define OBJ_OP_IP_STR      5  // (UNVERIFIED-POS) spare Strings5

// FORM_FINISHED (Form5) — USER_LED 13..14, STRINGS 6..8
#define OBJ_FN_COMPLETE   14  // "COMPLETE" banner (alias "COMPLETE-banner" — CONFIRMED)
#define OBJ_FN_READY      13  // "READY" banner (Userled13, unnamed — by elimination)
#define OBJ_FN_IP_STR      8  // (UNVERIFIED-POS) spare Strings8; 6/7 = FN-msg-a/b

// FORM_ERROR (Form6) — USER_LED 15, STRINGS 9..10
#define OBJ_ER_BANNER     15  // banner (alias "ERROR-banner" — CONFIRMED)
#define OBJ_ER_MSG         9  // error message text (alias "Error-msg" — CONFIRMED)
#define OBJ_ER_IP_STR     10  // (UNVERIFIED-POS) spare Strings10

// FORM_MESSAGE (Form7) — STRINGS 11..12
#define OBJ_MSG_TEXT      11  // message text (alias "Message-text" — CONFIRMED)
#define OBJ_MSG_IP_STR    12  // (UNVERIFIED-POS) spare Strings12

// Sentinel: a form with no IP-footer STRINGS object. display_updateFooter
// skips the WriteStr when the per-form index resolves to this.
#define OBJ_NO_FOOTER    0xFF

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
