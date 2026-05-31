// ======================================================================
// cc_reset_knock.ino — Track B / B1 decisive test
// ======================================================================
// Reset the gen4-uLCD-43DT (Diablo16) into its bootloader using the
// ClearCore's COM1 RTS line:
//     ClearCore COM1 RTS -> RJ45 -> 4D adapter SW1(RESET-EN) -> display RESET
// then send the verified bootloader knock "4DGL" @ 115200 and watch for the
// reply "db16". Result is printed over the ClearCore USB serial (USB-C).
//
// Physical setup (Track B / B0):
//   - CAT5: ClearCore COM1 <-> adapter RJ45  CONNECTED
//   - adapter micro-USB to PC                DISCONNECTED (no CP2104 contention)
//   - SW1 = RESET-EN (ON)                    so RTS reaches display RESET
//   - AUX 24 V powering the display
//   - ClearCore USB-C -> PC                  (flash this sketch + read result)
//
// Mirrors the verified COM1/RTS pattern from KegDisplay.cpp (which omits an
// explicit Mode() call — COM1 is TTL by default). Knock timing from the
// successful-flash strace: bootloader window >=50 ms, 4DGL -> db16 ~11 ms.
// ======================================================================
#include "ClearCore.h"

const uint32_t USB_BAUD  = 115200;
const uint32_t DISP_BAUD = 115200;   // Diablo16 bootloader baud (verified)
const char*    WANT      = "db16";

// One reset attempt + knock burst. assertFirst picks the RTS edge polarity
// (we don't know which way SW1 wires it, so the caller tries both).
bool tryReset(bool assertFirst) {
  if (assertFirst) {
    ConnectorCOM1.RtsMode(SerialBase::LINE_ON);   // assert (hold in reset)
    delay(50);
    ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);  // release -> chip boots
  } else {
    ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
    delay(50);
    ConnectorCOM1.RtsMode(SerialBase::LINE_ON);
  }

  // drain stale rx, then knock immediately within the bootloader window
  while (Serial1.available()) { Serial1.read(); }

  uint8_t match = 0;                 // rolling match against "db16"
  uint32_t t0 = millis();
  uint32_t lastKnock = 0;
  while (millis() - t0 < 150) {
    if (millis() - lastKnock >= 5) {
      Serial1.print("4DGL");
      lastKnock = millis();
    }
    while (Serial1.available()) {
      char c = (char)Serial1.read();
      if (c == WANT[match]) {
        if (++match == 4) { return true; }
      } else {
        match = (c == WANT[0]) ? 1 : 0;
      }
    }
  }
  return false;
}

void setup() {
  Serial.begin(USB_BAUD);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0 < 3000)) { }

  Serial1.begin(DISP_BAUD);          // COM1 TTL by default; matches KegDisplay
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);  // idle deasserted

  Serial.println();
  Serial.println(F("== cc_reset_knock: ClearCore RTS -> Diablo16 bootloader =="));
  Serial.println(F("(CAT5 connected, adapter micro-USB unplugged, SW1=RESET-EN)"));
}

void loop() {
  bool ok = tryReset(true);
  if (ok) {
    Serial.println(F("*** db16 - BOOTLOADER REACHED via RTS assert->release ***"));
  } else {
    bool ok2 = tryReset(false);
    if (ok2) {
      Serial.println(F("*** db16 - BOOTLOADER REACHED via RTS release->assert ***"));
    } else {
      Serial.println(F("no db16 (tried both RTS edge polarities)"));
    }
  }
  delay(2000);
}
