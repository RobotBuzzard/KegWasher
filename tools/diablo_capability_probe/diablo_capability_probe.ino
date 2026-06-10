// ======================================================================
// diablo_capability_probe.ino — verify which "modern UI" SPE commands the
// gen4-uLCD-43DT's factory SPE2 runtime actually supports, before the screen
// redesign builds on them. Candidates (all wrapped by Diablo_Serial_4DLib,
// all listed in the DIABLO16 internal-functions manual, but SPE exposes a
// subset — so PROVE each one):
//
//   txt_Set(FONT_ID, 1..11)  — esp. FONT_6/7/8 (DejaVu proportional)
//   gfx_MoveTo + putCH       — pixel-exact text placement (vs cell grid)
//   gfx_Button               — native raised/sunken button w/ centred label
//   gfx_Panel                — raised/recessed card
//   TEXT_BOLD / TEXT_XGAP    — weight + letter-spacing
//   gfx_Line / TriangleFilled— separators + icon primitives
//
// Each call is wrapped in PROBE(): pre-drain, call, report ACK over USB.
// Visual result is verified via the bench camera (exposure pinned 2026-06-10).
//
// BRICK NOTE: Display object built in setup(), never file-scope (ctor
// flush() faults pre-USB; recover via double-tap RESET).
// ======================================================================
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

#define DW 272   // portrait
#define DH 480

Diablo_Serial_4DLib *D = nullptr;
volatile uint32_t timeouts = 0;
void onTimeout(int, unsigned char) { timeouts++; }

// Results are buffered and re-printed every 5 s from loop() so the report
// survives the USB port being re-opened after the one-shot setup() run.
char report[1200];
size_t rlen = 0;
void rep(const char* line, bool ok) {
  rlen += snprintf(report + rlen, sizeof(report) - rlen, "%s %s\r\n",
                   line, ok ? "ACK=YES" : "ACK=no");
}

#define PROBE(label, call) do {                       \
    while (Serial1.available()) Serial1.read();       \
    uint32_t _t = timeouts;                           \
    call;                                             \
    rep(label, timeouts == _t);                       \
  } while (0)

static void put(const char* s) {
  while (*s) {
    while (Serial1.available()) Serial1.read();
    D->putCH((word)(uint8_t)*s++);
  }
}

// pixel-positioned text via gfx_MoveTo (the thing we're proving)
static void textAt(int x, int y, const char* s) {
  while (Serial1.available()) Serial1.read();
  D->gfx_MoveTo(x, y);
  put(s);
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }
  Serial.println(F("== diablo_capability_probe =="));

  Serial1.begin(9600);
  Serial1.ttl(true);
  D = new Diablo_Serial_4DLib(&Serial1);
  D->Callback4D = onTimeout;
  D->TimeLimit4D = 1000;

  // RTS reset -> SPE@9600 -> handshake -> portrait (same dance as KDS::begin)
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);  delay(200);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF); delay(4500);
  for (int i = 0; i < 12; i++) {
    while (Serial1.available()) Serial1.read();
    uint32_t e = timeouts; D->gfx_Cls();
    if (timeouts == e) break;
    delay(200);
  }
  while (Serial1.available()) Serial1.read();
  D->gfx_Set(SCREEN_MODE, PORTRAIT);
  while (Serial1.available()) Serial1.read();
  D->gfx_Set(BACKGROUND_COLOUR, 0x0000);
  while (Serial1.available()) Serial1.read();
  D->gfx_Cls();

  // defaults: white on black, opaque
  D->txt_Set(TEXT_COLOUR, 0xFFFF);
  while (Serial1.available()) Serial1.read();
  D->txt_Set(TEXT_OPACITY, 1);
  while (Serial1.available()) Serial1.read();
  D->txt_Set(TEXT_BACKGROUND, 0x0000);

  // ---- 1. font sampler: ID + "Ag 09:42" per row ----
  const word fonts[] = { FONT_1, FONT_2, FONT_3, FONT_4, FONT_5,
                         FONT_6, FONT_7, FONT_8, FONT_11 };
  const char* names[] = { "F1 5x7", "F2 8x8", "F3 8x12", "F4 12x16",
                          "F5 MSSans", "F6 DVCond", "F7 DejaVu",
                          "F8 DVBold", "F11 EGA" };
  int y = 4;
  for (unsigned i = 0; i < sizeof(fonts)/sizeof(fonts[0]); i++) {
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "txt_FontID(%d)", (int)fonts[i]);
    while (Serial1.available()) Serial1.read();
    uint32_t e = timeouts;
    D->txt_FontID(fonts[i]);
    rep(lbl, timeouts == e);
    char row[32];
    snprintf(row, sizeof(row), "%s Ag 09:42", names[i]);
    textAt(8, y, row);
    y += 26;
  }

  // ---- 2. size multiplier on a proportional font (big countdown look) ----
  PROBE("FONT_7 W2/H3", { D->txt_FontID(FONT_7);
                          D->txt_Set(TEXT_WIDTH, 2); D->txt_Set(TEXT_HEIGHT, 3); });
  textAt(8, y, "09:42");
  y += 52;

  // ---- 3. bold + letter spacing ----
  PROBE("TEXT_BOLD",  { D->txt_Set(TEXT_WIDTH, 1); D->txt_Set(TEXT_HEIGHT, 1);
                        D->txt_FontID(FONT_7); D->txt_Set(TEXT_BOLD, 1); });
  textAt(8, y, "BOLD attr");
  PROBE("TEXT_XGAP",  { D->txt_Set(TEXT_XGAP, 3); });
  textAt(120, y, "S P A C E D");
  while (Serial1.available()) Serial1.read();
  D->txt_Set(TEXT_XGAP, 0);
  y += 26;

  // ---- 4. pixel-exact placement proof: odd offsets no cell grid hits ----
  PROBE("gfx_MoveTo(137,y)", { D->gfx_MoveTo(137, y); });
  put("@137px");
  textAt(8, y, "@8px");
  y += 28;

  // ---- 5. native button, raised + sunken, two fonts ----
  PROBE("gfx_Button raised F7",
        D->gfx_Button(1, 8,   y, 0x0455, 0xFFFF, FONT_7, 1, 2, (char*)"START"));
  PROBE("gfx_Button sunken F3",
        D->gfx_Button(0, 140, y, 0x8800, 0xFFFF, FONT_3, 1, 2, (char*)"STOP"));
  y += 48;

  // ---- 6. panels: raised + recessed cards ----
  PROBE("gfx_Panel raised",   D->gfx_Panel(1, 8,   y, 120, 56, 0x2104));
  PROBE("gfx_Panel recessed", D->gfx_Panel(0, 140, y, 120, 56, 0x2104));
  while (Serial1.available()) Serial1.read();
  D->txt_FontID(FONT_7);
  textAt(20, y + 18, "card");
  y += 64;

  // ---- 7. lines + triangle (separators, icons) ----
  PROBE("gfx_Line", D->gfx_Line(8, y, DW - 8, y, 0x4208));
  PROBE("gfx_TriangleFilled",
        D->gfx_TriangleFilled(20, y + 30, 44, y + 6, 68, y + 30, 0x07E0));
  PROBE("gfx_CircleFilled", D->gfx_CircleFilled(100, y + 18, 12, 0xFD20));

  rlen += snprintf(report + rlen, sizeof(report) - rlen,
                   "total timeouts: %lu\r\n== end ==\r\n", (unsigned long)timeouts);
}

void loop() {
  Serial.println(F("== diablo_capability_probe report =="));
  Serial.print(report);
  delay(5000);
}
