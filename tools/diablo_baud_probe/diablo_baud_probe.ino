// ======================================================================
// diablo_baud_probe.ino — find the gen4 display's SPE comms baud.
//
// The display answers Diablo-serial commands with "bad gfx cmd" at 9600 ->
// bytes arrive but decode wrong == baud mismatch (Diablo16 SPE often defaults
// to 115200). This sketch sweeps candidate bauds; at each it fills the whole
// screen a DISTINCT colour. Only the CORRECT baud will actually fill the
// screen (wrong bauds keep showing "bad gfx cmd"). Watch the panel and note
// the colour -> that maps to the baud. USB log @115200 also reports whether
// the display ACKed (so we learn if the display->CC RX/ACK wire works too).
//
//   9600 -> RED | 19200 -> YELLOW | 38400 -> GREEN | 57600 -> CYAN | 115200 -> BLUE
//
// Baud is changed with ConnectorCOM1.Speed() (NOT Serial1.begin(), which
// floats RTS — RTS is wired to the display RESET line). See BRICK NOTE in
// diablo_touch_test.ino for why Display is built in setup().
// ======================================================================
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

Diablo_Serial_4DLib *Display = nullptr;
volatile uint32_t timeouts = 0;
void onTimeout(int e, unsigned char b) { timeouts++; }

struct Cand { uint32_t baud; word colour; const char *name; };
Cand cands[] = {
  {  9600,    RED,    "RED"    },
  { 19200,    YELLOW, "YELLOW" },
  { 38400,    GREEN,  "GREEN"  },
  { 57600,    CYAN,   "CYAN"   },
  { 115200,   BLUE,   "BLUE"   },
};
const int NCAND = sizeof(cands) / sizeof(cands[0]);

void setup() {
  Serial.begin(115200);                 // USB up first -> always recoverable
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("== diablo_baud_probe =="));

  Serial1.begin(9600);                  // initial open; Speed() retunes per-candidate
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);   // release display from reset
  delay(300);

  Display = new Diablo_Serial_4DLib(&Serial1);
  Display->Callback4D = onTimeout;      // REQUIRED for heap obj (not zero-init)
  Display->TimeLimit4D = 1500;
}

void loop() {
  for (int i = 0; i < NCAND; i++) {
    ConnectorCOM1.Speed(cands[i].baud); // RTS-safe baud change
    while (Serial1.available()) Serial1.read();   // drain stale RX
    uint32_t before = timeouts;
    Display->gfx_BGcolour(cands[i].colour);
    Display->gfx_Cls();                 // fills screen with bg colour at correct baud
    bool acked = (timeouts == before);  // no new timeout => display ACKed
    Serial.print(F("baud ")); Serial.print(cands[i].baud);
    Serial.print(F(" -> ")); Serial.print(cands[i].name);
    Serial.print(F("  ACK=")); Serial.print(acked ? F("YES") : F("no"));
    Serial.print(F("  (total timeouts ")); Serial.print(timeouts); Serial.println(')');
    delay(3000);                        // dwell so the panel can be observed
  }
  Serial.println(F("--- sweep done, repeating ---"));
}
