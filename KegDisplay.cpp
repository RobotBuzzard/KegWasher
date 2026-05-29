// ======================================================================
// KegDisplay.cpp - Goldelox driver wrapper (ViSi-Genie protocol)
// ======================================================================
// Init dependency: KegWasher.ino calls display_init() before config_init()
// and stateMachine_init() so error messages from those have a screen to
// land on. The Goldelox boot sequence after RTS reset takes ~4 s before
// the Genie handshake can succeed.
// ======================================================================
#include "KegDisplay.h"
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegTimers.h"

Genie genie;

void display_init() {
  Serial1.begin(9600);  // display is in SPE2 mode at 9600 — match it

  // Hardware reset via COM1 RTS (wired to display RESET pin on the adapter).
  // LINE_ON asserts reset; LINE_OFF releases. Goldelox Genie app needs ~4 s
  // to boot after release before it begins the handshake.
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);
  delay(1000);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
  delay(3000);

  genie.Begin((Stream&)Serial1);  // 2-second handshake; returns false if display absent
  genie.WriteContrast(15);
  genie.SetForm(FORM_STARTUP);
}

void display_showStartup() {
  // FORM_STARTUP has static labels in Workshop4 (no dynamic content).
  // Leave a 2 s window so the operator can see the splash.
  genie.SetForm(FORM_STARTUP);
  delay(2000);
}

// ── footer helper ──────────────────────────────────────────────────────

void display_updateFooter() {
  char buf[20];
  if (kwEthernetReady) {
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
  } else {
    snprintf(buf, sizeof(buf), "offline");
  }

  // Each form uses a STRINGS object at OBJ_*_IP_STR for the address line.
  // Index 1 is the planned layout for all forms; Workshop4 must match.
  genie.WriteStr(1, buf);
}

// ── banner flash ──────────────────────────────────────────────────────

void display_flashBanner(const char* text, word /*bgColor*/, bool visible) {
  uint16_t val = visible ? 1 : 0;
  switch (genie.GetForm()) {
    case FORM_NOT_READY:
      genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_NR_BANNER, val);
      break;
    case FORM_READY:
      genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_RD_BANNER, val);
      break;
    case FORM_HEATING:
      genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_HT_BANNER, val);
      break;
    case FORM_FINISHED:
      if (strcmp(text, "READY") == 0) {
        // Alarm acknowledged — hide COMPLETE, show READY
        genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_FN_COMPLETE, 0);
        genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_FN_READY, val);
      } else {
        // "COMPLETE" pre-ack flash
        genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_FN_COMPLETE, val);
      }
      break;
    case FORM_ERROR:
      genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_ER_BANNER, val);
      break;
    default:
      break;
  }
}

// ── state screens ─────────────────────────────────────────────────────

void display_showNotReady(bool waterOk, bool airOk, bool co2Ok, bool estopOk) {
  if (genie.GetForm() != FORM_NOT_READY) {
    genie.SetForm(FORM_NOT_READY);
    display_updateFooter();
  }
  genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_NR_WATER, waterOk  ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_NR_AIR,   airOk    ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_NR_CO2,   co2Ok    ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_NR_ESTOP, estopOk  ? 1 : 0);
}

void display_showReadyScreen() {
  genie.SetForm(FORM_READY);
  display_updateFooter();
}

void display_showFinishedScreen() {
  genie.SetForm(FORM_FINISHED);
  display_updateFooter();
}

void display_showProgress(const char* /*label*/, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes) {
  if (genie.GetForm() != FORM_HEATING) {
    genie.SetForm(FORM_HEATING);
    display_updateFooter();
  }
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_HT_CURR,    (uint16_t)currentValue);
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_HT_TARGET,  (uint16_t)targetValue);
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_HT_PCT,     (uint16_t)percentage);
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_HT_LEVEL,   (uint16_t)hardware_getCausticLevel());
  if (remainingMinutes >= 0) {
    genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_HT_EST_MIN, (uint16_t)remainingMinutes);
  }
}

void display_showError(const char* errorMsg) {
  genie.SetForm(FORM_ERROR);
  genie.WriteStr(OBJ_ER_MSG, errorMsg);
  display_updateFooter();
}

void display_showMessage(const char* message) {
  genie.SetForm(FORM_MESSAGE);
  genie.WriteStr(OBJ_MSG_TEXT, message);
  display_updateFooter();
}

// ── operating state screen ────────────────────────────────────────────

void display_updateTimer(unsigned long elapsedMs) {
  unsigned long duration = 0;
  switch (currentState) {
    case STATE_DRAINING: duration = kegSizeLatched ? timers_adjustForKegSize(dirtyDrainTimer) : dirtyDrainTimer; break;
    case STATE_RINSING:  duration = kegSizeLatched ? timers_adjustForKegSize(rinseTimer)      : rinseTimer;      break;
    case STATE_WASHING:  duration = kegSizeLatched ? timers_adjustForKegSize(washTimer)       : washTimer;       break;
    case STATE_SANITIZE: duration = kegSizeLatched ? timers_adjustForKegSize(saniTimer)       : saniTimer;       break;
    case STATE_PRESSURE: duration = kegSizeLatched ? timers_adjustForKegSize(purgeTimer)      : purgeTimer;      break;
    default: return;
  }
  unsigned long remaining = (elapsedMs < duration) ? (duration - elapsedMs) : 0UL;
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_OP_TIMER_MIN, (uint16_t)(remaining / 60000UL));
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_OP_TIMER_SEC, (uint16_t)((remaining / 1000UL) % 60UL));
}

void display_updateStatus() {
  genie.WriteObject(GENIE_OBJ_LED_DIGITS, OBJ_OP_TEMP,  (uint16_t)hardware_getCausticTemp());
  genie.WriteObject(GENIE_OBJ_USER_LED,   OBJ_OP_WATER, isWaterOk     ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED,   OBJ_OP_AIR,   isAirOk       ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED,   OBJ_OP_CO2,   isCo2Ok       ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED,   OBJ_OP_ESTOP, isEstopActive ? 1 : 0);
  genie.WriteObject(GENIE_OBJ_USER_LED,   OBJ_OP_MQTT,  kwMqttReady   ? 1 : 0);
}

void display_showOperatingScreen() {
  genie.SetForm(FORM_OPERATING);
  genie.WriteStr(OBJ_OP_STATE_STR, stateNames[currentState]);
  genie.WriteObject(GENIE_OBJ_USER_LED, OBJ_OP_KEG, isLargeKeg ? 1 : 0);
  display_updateTimer(timers_getStateElapsed());
  display_updateStatus();
  display_updateFooter();
}

void display_update() { display_showOperatingScreen(); }
