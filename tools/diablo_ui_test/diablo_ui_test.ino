// ======================================================================
// diablo_ui_test.ino — exercise the KegDisplaySerial module on real hardware.
// Cycles every state screen, runs a LIVE countdown on the operating screen
// (proves flicker-free partial updates), and polls touch. Logs ACK-error
// count + touch coords over USB @115200. NO ViSi-Genie.
// ======================================================================
#include <ClearCore.h>
#include "KegDisplaySerial.h"

const char* IP = "192.168.1.111";

void showState(int s) {
  switch (s) {
    case 0: KDS::startup("KegWasher", "initializing..."); break;
    case 1: KDS::notReady(true, false, true, true, IP);   break;
    case 2: KDS::ready(IP);                                break;
    case 3: KDS::heating(45, 71, 63, IP);                 break;
    case 4: { // OPERATING: static frame, then live countdown via partial updates
      KDS::operatingFrame("WASHING", IP);
      KDS::operatingStatus(71, true, true, false, true, true);
      for (int t = 165; t >= 150; t--) {            // 02:45 -> 02:30, 1 Hz
        KDS::operatingTimer(t / 60, t % 60);
        // animate the footer MQTT indicators: connected, pub blinks each tick,
        // sub blinks every 3rd tick (simulates real pub/sub traffic)
        KDS::mqttIndicators(true, (t % 2) == 0, (t % 3) == 0);
        int tx, ty;
        if (KDS::touch(&tx, &ty)) { Serial.print(F("TOUCH ")); Serial.print(tx); Serial.print(','); Serial.println(ty); }
        delay(1000);
      }
      break;
    }
    case 5: KDS::finished(IP);                  break;
    case 6: KDS::error("E07: OVERTEMP", IP);    break;
  }
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("== diablo_ui_test (KegDisplaySerial) =="));
  KDS::begin();
  KDS::touchEnable();
  Serial.print(F("synced, ackErrors=")); Serial.println(KDS::ackErrors());
}

void loop() {
  static int s = 0;
  uint32_t before = KDS::ackErrors();
  uint32_t t0 = millis();
  showState(s);
  uint32_t dt = millis() - t0;       // full redraw time (state screens; not operating's countdown)
  uint32_t afterDraw = KDS::ackErrors();
  Serial.print(F("screen ")); Serial.print(s);
  Serial.print(F("  ms=")); Serial.print(dt);
  Serial.print(F("  draw=")); Serial.print(afterDraw - before);
  s = (s + 1) % 7;
  // poll touch during the dwell
  uint32_t pollBefore = KDS::ackErrors();
  for (int i = 0; i < 30; i++) {
    int tx, ty;
    if (KDS::touch(&tx, &ty)) { Serial.print(F(" [TOUCH ")); Serial.print(tx); Serial.print(','); Serial.print(ty); Serial.print(F("]")); }
    delay(100);
  }
  Serial.print(F("  poll=")); Serial.println(KDS::ackErrors() - pollBefore);
}
