// diablo_cal.ino — measure the SPE font cell height. Draw green reference lines
// at known pixel Y (130/260/390) and text markers at txt_MoveCursor rows
// 10/20/30 (size 1). If the cell is 13px, "R10>" aligns with y=130, etc.
// Whatever it aligns with gives cellH = refY / row. Portrait 272x480.
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

Diablo_Serial_4DLib* D;
volatile uint32_t e = 0;
void cb(int, unsigned char) { e++; }
void drain() { while (Serial1.available()) Serial1.read(); }
void put(const char* s){ while(*s){ drain(); D->putCH((word)(uint8_t)*s++); } }

void setup() {
  Serial.begin(115200); uint32_t t = millis(); while (!Serial && millis() - t < 3000) {}
  Serial1.begin(9600); Serial1.ttl(true);
  // hardware-reset the display (RTS->RESET via adapter RESET-EN) to clear any
  // runtime-error halt and boot fresh.
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);  delay(200);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF); delay(4500);   // Diablo16 boot
  D = new Diablo_Serial_4DLib(&Serial1); D->Callback4D = cb; D->TimeLimit4D = 1000;
  for (int i = 0; i < 12; i++) { drain(); uint32_t x = e; D->gfx_Cls(); if (e == x) break; delay(200); }
  drain(); D->gfx_Set(SCREEN_MODE, PORTRAIT);
  drain(); D->gfx_Set(BACKGROUND_COLOUR, 0); drain(); D->gfx_Cls();
  // green reference bars at y=130,260,390
  drain(); D->gfx_RectangleFilled(0, 130, 271, 133, 0x07E0);
  drain(); D->gfx_RectangleFilled(0, 260, 271, 263, 0x07E0);
  drain(); D->gfx_RectangleFilled(0, 390, 271, 393, 0x07E0);
  // white text markers at rows 10,20,30 (size 1)
  drain(); D->txt_Set(TEXT_COLOUR, 0xFFFF); drain(); D->txt_Set(TEXT_WIDTH, 1); drain(); D->txt_Set(TEXT_HEIGHT, 1);
  drain(); D->txt_MoveCursor(10, 0); put("R10>line?");
  drain(); D->txt_MoveCursor(20, 0); put("R20>line?");
  drain(); D->txt_MoveCursor(30, 0); put("R30>line?");
  Serial.println(F("cal drawn: rows 10/20/30 vs green lines 130/260/390"));
}
void loop() {}
