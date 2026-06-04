// ======================================================================
// KegDisplaySerial.cpp — raw SPE serial UI for the gen4-uLCD-43DT (Diablo16).
// See KegDisplaySerial.h. Write-only commands + RX drain → desync-free.
// ======================================================================
#include "KegDisplaySerial.h"
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

namespace {
  const int DW = 272, DH = 480;   // PORTRAIT — panel mounted 272W x 480H

  // palette (RGB565 literals — certain)
  const word C_BG    = 0x0000;  // black
  const word C_BAR   = 0x001F;  // blue
  const word C_FOOT  = 0x4208;  // dark grey
  const word C_TXT   = 0xFFFF;  // white
  const word C_OK    = 0x07E0;  // green
  const word C_BAD   = 0xF800;  // red
  const word C_WARN  = 0xFD20;  // orange
  const word C_TIMER = 0xFFE0;  // yellow
  const word C_CYAN  = 0x07FF;
  const word C_NAVY  = 0x0010;
  const word C_DIM   = 0x0320;  // dim green (connected, idle)

  // operating-screen field rects (portrait 272x480; flicker-free partial updates)
  const int TMR_X1 = 8,  TMR_Y1 = 110, TMR_X2 = 263, TMR_Y2 = 190;   // timer field
  const int TMP_X1 = 36, TMP_Y1 = 250, TMP_X2 = 235, TMP_Y2 = 322;   // temp field
  const int DOT_Y  = 400;                       // status indicator row
  const int dotX[5] = { 26, 86, 146, 206, 250 };

  // footer banner: IP (left) + MQTT pub/sub indicator dots (right)
  const int FOOT_TY = DH - 18, FOOT_DY = DH - 11;
  const int PUB_LX = 150, PUB_DX = 172, SUB_LX = 196, SUB_DX = 218;

  Diablo_Serial_4DLib* D = nullptr;
  volatile uint32_t g_err = 0;
  void onTO(int, unsigned char) { g_err++; }

  // 4D system font cell is ~8x8 px at size 1; txt_MoveCursor units scale with
  // the current text size, so pixel->cell = px / (8 * size).
  inline void drain() { while (Serial1.available()) Serial1.read(); }
  void bg(word c)                              { drain(); D->gfx_Set(BACKGROUND_COLOUR, c); }
  void cls()                                   { drain(); D->gfx_Cls(); }
  void rect(int a,int b,int c,int d,word col)  { drain(); D->gfx_RectangleFilled(a,b,c,d,col); }
  void disc(int x,int y,int r,word col)        { drain(); D->gfx_CircleFilled(x,y,r,col); }
  void put(const char* s)                      { while (*s) { drain(); D->putCH((word)(uint8_t)*s++); } }

  // place text near pixel (x,y) at the given size via the char grid. The SPE
  // system font cell is ~12px tall x ~8px wide; txt_MoveCursor takes char cells
  // (scaled by size). (TEXT_XPOS/YPOS are NOT valid txt_Set funcs — they halt
  // the runtime with "Bad txt_Set command number".)
  const int CELL_H = 12, CELL_W = 8;
  void textPx(int x,int y,int sz,word fg,word bgc,const char* s) {
    drain(); D->txt_Set(TEXT_COLOUR, fg);
    drain(); D->txt_Set(TEXT_BACKGROUND, bgc);
    drain(); D->txt_Set(TEXT_OPACITY, 1);          // OPAQUE so bg shows
    drain(); D->txt_Set(TEXT_WIDTH, sz);
    drain(); D->txt_Set(TEXT_HEIGHT, sz);
    drain(); D->txt_MoveCursor(y / (CELL_H * sz), x / (CELL_W * sz));
    put(s);
  }

  // ---- raw, fully-framed reads (for touch) ----
  // The lib's touch_Get is GetAckResp; its response can desync and leave a
  // straggler that NAKs later draws. Read raw instead: send opcode, read exactly
  // ACK(0x06)+BE16 word, then drain until the line is quiet.
  int readByteTO(int ms) {
    uint32_t t = millis();
    while (millis() - t < (uint32_t)ms) { if (Serial1.available()) return Serial1.read(); }
    return -1;
  }
  void drainQuiet(int quietMs) {
    uint32_t last = millis();
    while (millis() - last < (uint32_t)quietMs) { if (Serial1.available()) { Serial1.read(); last = millis(); } }
  }
  word rawCmdResp(word opcode, word arg) {
    drainQuiet(2);
    uint16_t op = (uint16_t)opcode;
    Serial1.write((uint8_t)(op >> 8)); Serial1.write((uint8_t)(op & 0xFF));
    Serial1.write((uint8_t)(arg >> 8)); Serial1.write((uint8_t)(arg & 0xFF));
    Serial1.flush();
    int ack = readByteTO(300);
    int hi  = readByteTO(300);
    int lo  = readByteTO(300);
    drainQuiet(3);
    if (ack != 0x06 || hi < 0 || lo < 0) { g_err++; return 0xFFFF; }
    return (word)((hi << 8) | lo);
  }

  // host-side touch calibration (affine): screen = [a b c; d e f] . [rawx, rawy, 1]
  // Defaults from diablo_touch_cal (2026-06-03); override at runtime via
  // setTouchCalibration() (e.g. firmware loads per-unit values from SD config).
  float g_a = 0.972363f, g_b = 0.031048f, g_c = -7.2746f,
        g_d = 0.084970f, g_e = 0.989651f, g_f = -15.0600f;
  int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
  void applyCal(int rx, int ry, int* sx, int* sy) {
    *sx = clampi((int)(g_a * rx + g_b * ry + g_c + 0.5f), 0, DW - 1);
    *sy = clampi((int)(g_d * rx + g_e * ry + g_f + 0.5f), 0, DH - 1);
  }

  void chrome(const char* title, word barColour) {
    bg(C_BG); cls();
    rect(0, 0, DW - 1, 46, barColour);
    textPx(8, 8, 3, C_TXT, barColour, title);
    rect(0, DH - 22, DW - 1, DH - 1, C_FOOT);
  }
  void footerIP(const char* ip) {
    textPx(6, FOOT_TY, 1, C_CYAN, C_FOOT, ip);          // IP / "offline" on the left
    textPx(PUB_LX, FOOT_TY, 1, C_TXT, C_FOOT, "P");     // pub label
    textPx(SUB_LX, FOOT_TY, 1, C_TXT, C_FOOT, "S");     // sub label
    disc(PUB_DX, FOOT_DY, 5, C_BAD);                    // dots start "disconnected"
    disc(SUB_DX, FOOT_DY, 5, C_BAD);                    // until mqttIndicators() runs
  }

  void statusDots(bool w,bool a,bool c,bool e,bool m) {
    bool v[5] = { w, a, c, e, m };
    for (int i = 0; i < 5; i++) disc(dotX[i], DOT_Y, 8, v[i] ? C_OK : C_BAD);
  }
}

namespace KDS {

void begin() {
  Serial1.begin(9600);
  Serial1.ttl(true);
  // hardware-reset the display via COM1 RTS (adapter RESET-EN must be ON):
  // assert reset, release, wait for the Diablo16 to boot. Clears any runtime
  // "Touch to continue" error halt and starts fresh.
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);  delay(200);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF); delay(4500);
  D = new Diablo_Serial_4DLib(&Serial1);
  D->Callback4D = onTO;
  D->TimeLimit4D = 1000;
  for (int i = 0; i < 12; i++) {                 // first cmds after boot drop; sync first
    drain(); uint32_t t = g_err; D->gfx_Cls();
    if (g_err == t) break;
    delay(200);
  }
  drain(); D->gfx_Set(SCREEN_MODE, PORTRAIT);    // 272W x 480H
}

uint32_t ackErrors() { return g_err; }

void startup(const char* l1, const char* l2) {
  chrome("STARTUP", C_NAVY);
  textPx(16, 92, 2, C_TXT, C_BG, l1);
  textPx(16, 140, 1, C_CYAN, C_BG, l2);
}

void notReady(bool water, bool air, bool co2, bool estop, const char* ip) {
  chrome("NOT READY", C_BAD);
  const char* labels[4] = { "WATER", "AIR", "CO2", "ESTOP" };
  bool ok[4] = { water, air, co2, estop };
  for (int i = 0; i < 4; i++) {
    int y = 100 + i * 80;
    disc(30, y + 6, 12, ok[i] ? C_OK : C_BAD);
    textPx(56, y, 2, C_TXT, C_BG, labels[i]);
    textPx(180, y, 2, ok[i] ? C_OK : C_BAD, C_BG, ok[i] ? "OK" : "--");
  }
  footerIP(ip);
}

void ready(const char* ip) {
  chrome("READY", C_OK);
  textPx(24, 150, 2, C_TXT, C_BG, "Check levels");
  textPx(24, 210, 2, C_TIMER, C_BG, "Press START");
  footerIP(ip);
}

void heating(int curC, int tgtC, int pct, const char* ip) {
  chrome("HEATING", C_WARN);
  char b[24];
  snprintf(b, sizeof(b), "CUR %dC", curC);  textPx(24, 90, 2, C_TXT, C_BG, b);
  snprintf(b, sizeof(b), "TGT %dC", tgtC);  textPx(24, 140, 2, C_TXT, C_BG, b);
  snprintf(b, sizeof(b), "%d%%", pct);      textPx(70, 220, 5, C_TIMER, C_BG, b);
  footerIP(ip);
}

void operatingFrame(const char* stateName, const char* ip) {
  chrome(stateName, C_BAR);
  textPx(8, 54, 1, C_CYAN, C_BG, "TIME LEFT");
  textPx(8, TMP_Y1 - 16, 1, C_CYAN, C_BG, "TEMP");
  const char* dl[5] = { "W", "A", "C", "E", "M" };       // static dot labels
  for (int i = 0; i < 5; i++) textPx(dotX[i] - 4, DOT_Y + 16, 1, C_TXT, C_BG, dl[i]);
  footerIP(ip);
}

void operatingTimer(int minutes, int seconds) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", minutes, seconds);
  rect(TMR_X1, TMR_Y1, TMR_X2, TMR_Y2, C_BG);            // clear field
  textPx(TMR_X1 + 10, TMR_Y1 + 8, 6, C_TIMER, C_BG, b);
}

void operatingStatus(int tempC, bool water, bool air, bool co2, bool estop, bool mqtt) {
  char b[8];
  snprintf(b, sizeof(b), "%dC", tempC);
  rect(TMP_X1, TMP_Y1, TMP_X2, TMP_Y2, C_BG);            // clear field
  textPx(TMP_X1 + 6, TMP_Y1 + 6, 4, C_TXT, C_BG, b);
  statusDots(water, air, co2, estop, mqtt);
}

void finished(const char* ip) {
  chrome("COMPLETE", C_OK);
  textPx(40, 130, 3, C_TXT, C_BG, "DONE!");
  textPx(16, 210, 2, C_CYAN, C_BG, "Cycle complete");
  textPx(16, 250, 2, C_CYAN, C_BG, "DRAIN+START=new");
  footerIP(ip);
}

void error(const char* msg, const char* ip) {
  chrome("ERROR", C_BAD);
  textPx(16, 100, 2, C_BAD, C_BG, msg);
  footerIP(ip);
}

void message(const char* msg, const char* ip) {
  chrome("MESSAGE", C_NAVY);
  textPx(16, 100, 2, C_TXT, C_BG, msg);
  footerIP(ip);
}

void touchEnable() { drain(); D->touch_Set(TOUCH_ENABLE); }      // touch_Set is GetAck (clean)

void mark(int x, int y) { disc(x, y, 7, 0xFFFF); disc(x, y, 3, 0xF800); }   // touch feedback dot

// MQTT pub/sub footer indicators (partial update — call when MQTT state changes).
//   connected=false -> both RED;  connected idle -> dim green;
//   pubActive -> bright yellow pub dot;  subActive -> bright cyan sub dot.
void mqttIndicators(bool connected, bool pubActive, bool subActive) {
  word pc = !connected ? C_BAD : (pubActive ? C_TIMER : C_DIM);
  word sc = !connected ? C_BAD : (subActive ? C_CYAN : C_DIM);
  disc(PUB_DX, FOOT_DY, 5, pc);
  disc(SUB_DX, FOOT_DY, 5, sc);
}

void footer(const char* ip) { footerIP(ip); }   // redraw the footer (IP + P/S labels + reset dots)

// flashing alert strip just under the title bar (visible toggled by the caller)
void banner(const char* text, word colour, bool visible) {
  if (visible) { rect(0, 48, DW - 1, 84, colour); textPx(8, 54, 2, C_TXT, colour, text); }
  else         { rect(0, 48, DW - 1, 84, C_BG); }
}

bool touch(int* x, int* y) {
  static bool down = false;
  word st = rawCmdResp(F_touch_Get, TOUCH_STATUS);               // raw framed read
  if (st == TOUCH_PRESSED) {
    int tx = rawCmdResp(F_touch_Get, TOUCH_GETX);
    int ty = rawCmdResp(F_touch_Get, TOUCH_GETY);
    if (!down) { down = true; applyCal(tx, ty, x, y); return true; }  // calibrated, fresh-press edge
  } else if (st == TOUCH_RELEASED || st == NOTOUCH) {
    down = false;
  }
  return false;
}

bool touchRaw(int* x, int* y) {                                  // uncalibrated sample (for cal)
  word st = rawCmdResp(F_touch_Get, TOUCH_STATUS);
  if (st == TOUCH_PRESSED || st == TOUCH_MOVING) {
    *x = rawCmdResp(F_touch_Get, TOUCH_GETX);
    *y = rawCmdResp(F_touch_Get, TOUCH_GETY);
    return true;
  }
  return false;
}

void setTouchCalibration(float a, float b, float c, float d, float e, float f) {
  g_a = a; g_b = b; g_c = c; g_d = d; g_e = e; g_f = f;
}

} // namespace KDS
