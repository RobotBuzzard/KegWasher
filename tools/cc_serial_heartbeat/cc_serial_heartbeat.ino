// ======================================================================
// cc_serial_heartbeat.ino — minimal, guaranteed USB-safe recovery sketch.
// NO display, NO Serial1, NO blocking calls. Just proves the ClearCore USB
// CDC stays up and enumerable (/dev/ttyACM0) so we can flash freely again.
// ======================================================================
#include <ClearCore.h>

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
}

uint32_t n = 0;
void loop() {
  digitalWrite(LED_BUILTIN, (n & 1) ? HIGH : LOW);
  Serial.print(F("heartbeat ")); Serial.print(n++);
  Serial.print(F("  t=")); Serial.println(millis());
  delay(500);
}
