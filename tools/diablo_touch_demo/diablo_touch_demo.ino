// ======================================================================
// diablo_touch_demo.ino — verify resistive touch on the gen4-uLCD-43DT via
// the KegDisplaySerial raw-framed touch reader. Draws crosshair targets at
// known panel coords; each tap marks the point and logs "TOUCH x,y" over USB.
// Tap the targets and confirm the logged coords match. NO ViSi-Genie.
// ======================================================================
#include <ClearCore.h>
#include "KegDisplaySerial.h"

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("== diablo_touch_demo =="));
  KDS::begin();
  KDS::touchEnable();
  // reference targets the operator can aim at (panel is 480x272)
  KDS::error("TOUCH THE DOTS", "tap targets; coords log to USB");
  KDS::mark(40, 60); KDS::mark(232, 60);          // portrait 272x480 targets
  KDS::mark(136, 240);
  KDS::mark(40, 420); KDS::mark(232, 420);
  Serial.print(F("ready, ackErrors=")); Serial.println(KDS::ackErrors());
  Serial.println(F("TAP the dots: expect ~ (40,60)(232,60)(136,240)(40,420)(232,420)"));
}

void loop() {
  int x, y;
  if (KDS::touch(&x, &y)) {
    KDS::mark(x, y);
    Serial.print(F("TOUCH ")); Serial.print(x); Serial.print(','); Serial.println(y);
  }
  delay(40);
}
