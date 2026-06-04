// ======================================================================
// diablo_draw_test.ino — prove raw-serial drawing on the gen4-uLCD-43DT
// (DIABLO16) from the ClearCore. NO ViSi-Genie, NO Workshop4.
//
// CONFIRMED 2026-06-03: the panel is in SPE and ACKs (0x06) raw serial gfx
// commands cleanly once it has settled after boot. The display->CC RX path
// works. The old "bad gfx cmd" was a sketch bug: gfx_Get(GFX_XMAX) — GFX_XMAX
// (46)/GFX_YMAX(47) are peekW/pokeW system vars, NOT gfx_Get() modes (0-7), so
// gfx_Get(46) raises DIABLO16 runtime error #20. We never query size; the
// gen4-uLCD-43DT is 480x272 (datasheet R2.2).
//
// Display: gen4-uLCD-43DT = 480x272, RGB565, 4-wire resistive, SPE @ 9600, TTL.
// Wiring : CAT5 ClearCore COM1 <-> 4D ClearCore Adaptor RJ45; 4D adapter USB OFF.
//
// All draw calls use the OFFICIAL Diablo_Serial_4DLib (correct opcodes).
//
// BRICK NOTE: Display is built in setup() (after Serial1.begin), never as a
// file-scope global (its ctor's Serial1.flush() faults pre-USB during static
// init and drops the CC off the bus). Recover a bricked CC by double-tapping
// the on-board RESET (forces+holds the bootloader), then reflash.
// ======================================================================
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

#define DW 480          // gen4-uLCD-43DT width  (datasheet R2.2)
#define DH 272          // gen4-uLCD-43DT height (datasheet R2.2)

Diablo_Serial_4DLib *Display = nullptr;
volatile uint32_t timeouts = 0;
void onTimeout(int e, unsigned char b) { timeouts++; }

// Send a command, report whether it ACKed (no new timeout) over USB.
// Pre-drain RX: GetAckResp (query) commands can leave a stray response byte
// that desyncs the next command's ACK read — clearing it first keeps us framed.
#define DRAW(label, call) do {            \
    while (Serial1.available()) Serial1.read(); \
    uint32_t _t = timeouts;               \
    call;                                 \
    Serial.print(F("  " label "  ACK=")); \
    Serial.println(timeouts == _t ? F("YES") : F("no")); \
  } while (0)

// Read back a pixel we drew and compare to the expected RGB565 colour. This is
// the autonomous proof the draw actually landed (no eyes / camera needed).
void verifyPixel(const char *what, int x, int y, word expect) {
  while (Serial1.available()) Serial1.read();   // drain before a query read
  word got = Display->gfx_GetPixel(x, y);
  Serial.print(F("  verify ")); Serial.print(what);
  Serial.print(F(" @(")); Serial.print(x); Serial.print(','); Serial.print(y);
  Serial.print(F(") got=0x")); Serial.print(got, HEX);
  Serial.print(F(" expect=0x")); Serial.print(expect, HEX);
  Serial.println(got == expect ? F("  MATCH") : F("  MISMATCH"));
}

void setup() {
  Serial.begin(115200);                 // USB up FIRST -> always recoverable
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("== diablo_draw_test =="));

  Serial1.begin(9600);                  // SPE comms baud, 8-N-1
  Serial1.ttl(true);                    // 4D panel is TTL level (datasheet 4.1)
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);  // release display RESET (RTS->RESET)
  delay(800);                           // link settle

  Display = new Diablo_Serial_4DLib(&Serial1);
  Display->Callback4D = onTimeout;      // REQUIRED for heap obj (not zero-init)
  Display->TimeLimit4D = 2000;          // full 2 s window so ACKs aren't clipped

  // --- handshake: send gfx_Cls until the panel ACKs, so we don't start drawing
  //     before SPE has finished settling after boot (first commands get dropped) ---
  bool synced = false;
  for (int i = 0; i < 12 && !synced; i++) {
    while (Serial1.available()) Serial1.read();   // drain stale
    uint32_t t = timeouts;
    Display->gfx_Cls();
    if (timeouts == t) { synced = true; }
    else { delay(200); }
    Serial.print(F("handshake try ")); Serial.print(i + 1);
    Serial.println(synced ? F(": SYNCED (ACK)") : F(": no ack yet"));
  }
  if (!synced) Serial.println(F("WARNING: never synced — drawing anyway (TX-only)"));

  Serial.println(F("drawing template (480x272)..."));

  // --- clear ---
  DRAW("gfx_BGcolour(BLACK)", Display->gfx_BGcolour(BLACK));
  DRAW("gfx_Cls",             Display->gfx_Cls());

  // --- title bar ---
  DRAW("title bar rect",      Display->gfx_RectangleFilled(0, 0, DW - 1, 44, BLUE));
  DRAW("txt_FGcolour(WHITE)", Display->txt_FGcolour(WHITE));
  DRAW("txt_BGcolour(BLUE)",  Display->txt_BGcolour(BLUE));
  DRAW("txt_Width(2)",        Display->txt_Width(2));
  DRAW("txt_Height(2)",       Display->txt_Height(2));
  DRAW("txt_MoveCursor(0,0)", Display->txt_MoveCursor(0, 0));
  DRAW("putstr KEGWASHER",    Display->putstr((char *)" KEGWASHER"));

  // --- status line ---
  DRAW("txt_Width(1)",        Display->txt_Width(1));
  DRAW("txt_Height(1)",       Display->txt_Height(1));
  DRAW("txt_FGcolour(YELLOW)",Display->txt_FGcolour(YELLOW));
  DRAW("txt_BGcolour(BLACK)", Display->txt_BGcolour(BLACK));
  DRAW("txt_MoveCursor(7,0)", Display->txt_MoveCursor(7, 0));
  DRAW("putstr STATE",        Display->putstr((char *)" STATE: DRAW TEST OK"));

  // --- big state indicator + footer ---
  DRAW("circle GREEN",        Display->gfx_CircleFilled(DW / 2, DH / 2 + 30, 50, GREEN));
  DRAW("footer rect",         Display->gfx_RectangleFilled(0, DH - 24, DW - 1, DH - 1, RED));

  Serial.print(F("done. ACK timeouts during draw = ")); Serial.println(timeouts);

  // --- autonomous verification: read back pixels we drew ---
  Serial.println(F("verifying draw via gfx_GetPixel (RGB565):"));
  verifyPixel("titlebar BLUE", 240, 20, BLUE);
  verifyPixel("bg BLACK",       50, 80, BLACK);
  verifyPixel("circle GREEN",  DW / 2, DH / 2 + 30, GREEN);
  verifyPixel("footer RED",    240, DH - 12, RED);
}

void loop() {
  static uint32_t n = 0;
  Serial.print(F("alive ")); Serial.print(n++);
  Serial.print(F("  timeouts=")); Serial.println(timeouts);
  delay(3000);
}
