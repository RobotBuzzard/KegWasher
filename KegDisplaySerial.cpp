// ======================================================================
// KegDisplaySerial.cpp — raw SPE serial UI for the gen4-uLCD-43DT (Diablo16).
// See KegDisplaySerial.h. Write-only commands + RX drain → desync-free.
//
// Visual language ("direction C", 2026-06-10): minimal typographic on black.
// State is a 6 px edge strip + semantic accents only; DejaVu proportional
// fonts (FONT_7/FONT_8 bold) positioned pixel-exact via gfx_MoveTo (all
// verified on hardware by tools/diablo_capability_probe). Layout grid:
// content x=18..258, thin TRACK rules between sections, footer bar at 462.
// ======================================================================
#include "KegDisplaySerial.h"
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

namespace {
  const int DW = 272, DH = 480;   // PORTRAIT — panel mounted 272W x 480H

  // ---- palette (RGB565, quantized from the approved mockups) ----
  const word C_BG      = 0x0000;  // black
  const word C_TXT     = 0xEF7E;  // #ECEFF1 near-white
  const word C_SUB     = 0x84B4;  // #8494A0 secondary text
  const word C_TRACK   = 0x29A8;  // #2D3740 rules / inactive
  const word C_CARD    = 0x1904;  // #1C2226 footer bar / fields
  const word C_CARDHI  = 0x2166;  // #262E36 secondary button fill
  const word C_GRN     = 0x064A;  // #00C853 running / ok
  const word C_GRN_TXT = 0x0102;  // dark green text on green fill
  const word C_AMB     = 0xFD80;  // #FFB300 held / warning
  const word C_AMB_TXT = 0x18A0;  // dark amber text on amber fill
  const word C_RED     = 0xE1C6;  // #E53935 fault
  const word C_CYAN    = 0x05FA;  // #00BCD4 info accent / links
  const word C_BLUE    = 0x1C5C;  // #1E88E5 operator action / transition
  const word C_DIMGRN  = 0x0346;  // #006934 idle-connected dot

  // legacy callers pass saturated GREEN/AMBER/RED/BLUE literals — remap to
  // the design palette so KegDisplay didn't have to change its constants.
  word remap(word c) {
    switch (c) {
      case 0x07E0: return C_GRN;   case 0xFD20: return C_AMB;
      case 0xF800: return C_RED;   case 0x001F: return C_BLUE;
      case 0x0010: return C_BLUE;  case 0xFFE0: return C_AMB;
      case 0x07FF: return C_CYAN;  default:     return c;
    }
  }
  // label ink per fill — explicit, not luma-guessed (green/amber/cyan/white
  // fills read better with dark ink; red/blue/dark fills with near-white)
  word inkOn(word fill) {
    if (fill == C_AMB)  return C_AMB_TXT;
    if (fill == C_GRN)  return C_GRN_TXT;
    if (fill == C_CYAN) return C_GRN_TXT;
    if (fill == 0xFFFF) return 0x0000;
    return C_TXT;
  }

  // ---- layout grid ----
  const int LX = 18, RX = DW - 14;          // content left / right edge
  const int STRIP_W   = 6;
  const int TITLE_Y   = 10;
  const int META_Y    = 46;
  const int RULE1_Y   = 66;
  const int BANNER_Y1 = 72, BANNER_Y2 = 104;
  const int FOOT_Y    = DH - 18;

  // operating screen fields
  const int TMR_Y = 112, TMR_CLR_Y2 = 182;
  const int REM_Y = 188;
  const int RULE2_Y = 210;
  const int TMP_Y = 222, TMP_CLR_Y2 = 252;
  const int TMPSUB_Y = 258;
  const int RULE3_Y = 280;
  const int SENS_Y = 292;                   // 2x2 grid, rows +24
  const int FOOT_DOT_PUB_X = 238, FOOT_DOT_SUB_X = 256, FOOT_DOT_Y = DH - 9;

  Diablo_Serial_4DLib* D = nullptr;
  volatile uint32_t g_err = 0;
  void onTO(int, unsigned char) { g_err++; }

  inline void drain() { while (Serial1.available()) Serial1.read(); }
  void bg(word c)                              { drain(); D->gfx_Set(BACKGROUND_COLOUR, c); }
  void cls()                                   { drain(); D->gfx_Cls(); }
  void rect(int a,int b,int c,int d,word col)  { drain(); D->gfx_RectangleFilled(a,b,c,d,col); }
  void hline(int x1,int x2,int y,word col)     { drain(); D->gfx_Line(x1,y,x2,y,col); }
  void disc(int x,int y,int r,word col)        { drain(); D->gfx_CircleFilled(x,y,r,col); }

  // rounded rect = 2 rects + 4 discs (no gfx_RoundPanel over SPE)
  void rrect(int x1,int y1,int x2,int y2,int r,word col) {
    rect(x1 + r, y1, x2 - r, y2, col);
    rect(x1, y1 + r, x2, y2 - r, col);
    disc(x1 + r, y1 + r, r, col); disc(x2 - r, y1 + r, r, col);
    disc(x1 + r, y2 - r, r, col); disc(x2 - r, y2 - r, r, col);
  }

  // ---- text engine: DejaVu fonts, pixel-positioned, width-measured ----
  // FONT_7 = DejaVu Sans 9pt, FONT_8 = DejaVu Sans Bold 9pt (probe-verified).
  // Glyph widths are queried once at begin() (charwidth is per current font)
  // so labels can be centred / right-aligned without GetAckResp at draw time.
  byte g_w7[95], g_w8[95];                  // widths for ASCII 32..126
  int  g_h7 = 13, g_h8 = 13;                // line heights (measured at begin)

  // current text state — cached to skip redundant txt_Set spam
  word g_font = 0xFFFF, g_fg = 0xFFFF, g_bgc = 0xFFFF;
  int  g_sx = -1, g_sy = -1;

  void setFont(bool bold, int sx, int sy, word fg, word bgc) {
    word f = bold ? FONT_8 : FONT_7;
    if (f != g_font)  { drain(); D->txt_FontID(f);            g_font = f; }
    if (sx != g_sx)   { drain(); D->txt_Set(TEXT_WIDTH,  sx); g_sx = sx; }
    if (sy != g_sy)   { drain(); D->txt_Set(TEXT_HEIGHT, sy); g_sy = sy; }
    if (fg != g_fg)   { drain(); D->txt_Set(TEXT_COLOUR, fg); g_fg = fg; }
    if (bgc != g_bgc) { drain(); D->txt_Set(TEXT_BACKGROUND, bgc); g_bgc = bgc; }
  }

  int strW(const char* s, bool bold, int sx) {
    const byte* t = bold ? g_w8 : g_w7;
    long w = 0;
    for (; *s; s++) {
      unsigned ch = (unsigned char)*s;
      if (ch >= 32 && ch <= 126) w += t[ch - 32];
      else w += t['o' - 32];                // non-ASCII (°): assume ~'o' wide
    }
    return (int)(w * sx);
  }

  void put(const char* s) { while (*s) { drain(); D->putCH((word)(uint8_t)*s++); } }

  void textAt(int x, int y, const char* s) {
    drain(); D->gfx_MoveTo(x, y);
    put(s);
  }
  void lbl(int x, int y, bool bold, int sx, int sy, word fg, word bgc, const char* s) {
    setFont(bold, sx, sy, fg, bgc);
    textAt(x, y, s);
  }
  // centred within [x1,x2]
  void lblC(int x1, int x2, int y, bool bold, int sx, int sy, word fg, word bgc, const char* s) {
    int w = strW(s, bold, sx);
    int x = x1 + (x2 - x1 - w) / 2; if (x < x1) x = x1;
    lbl(x, y, bold, sx, sy, fg, bgc, s);
  }
  // right-aligned to x2
  void lblR(int x2, int y, bool bold, int sx, int sy, word fg, word bgc, const char* s) {
    lbl(x2 - strW(s, bold, sx), y, bold, sx, sy, fg, bgc, s);
  }

  // ---- raw, fully-framed reads (touch + font metrics) ----
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
  // charwidth/charheight take a single raw byte argument (lib source-verified)
  word rawCmdResp8(word opcode, byte arg) {
    drainQuiet(2);
    uint16_t op = (uint16_t)opcode;
    Serial1.write((uint8_t)(op >> 8)); Serial1.write((uint8_t)(op & 0xFF));
    Serial1.write(arg);
    Serial1.flush();
    int ack = readByteTO(300);
    int hi  = readByteTO(300);
    int lo  = readByteTO(300);
    drainQuiet(3);
    if (ack != 0x06 || hi < 0 || lo < 0) { g_err++; return 0xFFFF; }
    return (word)((hi << 8) | lo);
  }

  void buildWidthCache() {
    const word fonts[2] = { FONT_7, FONT_8 };
    byte* tabs[2] = { g_w7, g_w8 };
    int*  hs[2]   = { &g_h7, &g_h8 };
    for (int f = 0; f < 2; f++) {
      drain(); D->txt_FontID(fonts[f]);
      g_font = fonts[f];
      for (int ch = 32; ch <= 126; ch++) {
        word w = rawCmdResp8(F_charwidth, (byte)ch);
        tabs[f][ch - 32] = (w == 0xFFFF) ? 8 : (byte)w;
      }
      word h = rawCmdResp8(F_charheight, (byte)'A');
      if (h != 0xFFFF) *hs[f] = (int)h;
    }
  }

  // host-side touch calibration (affine) — unchanged from the previous build
  float g_a = 0.972363f, g_b = 0.031048f, g_c = -7.2746f,
        g_d = 0.084970f, g_e = 0.989651f, g_f = -15.0600f;
  int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
  void applyCal(int rx, int ry, int* sx, int* sy) {
    *sx = clampi((int)(g_a * rx + g_b * ry + g_c + 0.5f), 0, DW - 1);
    *sy = clampi((int)(g_d * rx + g_e * ry + g_f + 0.5f), 0, DH - 1);
  }

  // ---- shared chrome: edge strip + title (+ optional meta line) ----
  void chrome(const char* title, word stripColour, const char* meta) {
    word sc = remap(stripColour);
    bg(C_BG); cls();
    g_font = 0xFFFF; g_fg = g_bgc = 0xFFFF; g_sx = g_sy = -1;   // panel state reset
    rect(0, 0, STRIP_W, DH - 1, sc);
    lbl(LX, TITLE_Y, true, 2, 2, C_TXT, C_BG, title);
    if (meta && *meta) lbl(LX, META_Y, false, 1, 1, C_SUB, C_BG, meta);
    hline(LX, RX, RULE1_Y, C_TRACK);
    rect(0, FOOT_Y, DW - 1, DH - 1, C_CARD);
  }
  void footerIP(const char* ip) {
    lbl(8, FOOT_Y + 3, false, 1, 1, C_SUB, C_CARD, ip);
    disc(FOOT_DOT_PUB_X, FOOT_DOT_Y, 4, C_RED);   // dots start "disconnected"
    disc(FOOT_DOT_SUB_X, FOOT_DOT_Y, 4, C_RED);   // until mqttIndicators() runs
  }

  // sensor grid (2 cols x 2 rows): dot + label. Used by readiness + operating.
  const char* SENS_LBL[4] = { "WATER", "AIR", "CO2", "E-STOP" };
  void sensorDot(int i, int y0, bool ok) {
    int x = LX + (i % 2) * 124, y = y0 + (i / 2) * 24;
    disc(x + 4, y + 7, 4, ok ? C_GRN : C_RED);
  }
  void sensorGrid(int y0, bool w, bool a, bool c, bool e, bool emphasizeBad) {
    bool ok[4] = { w, a, c, e };
    for (int i = 0; i < 4; i++) {
      int x = LX + (i % 2) * 124, y = y0 + (i / 2) * 24;
      sensorDot(i, y0, ok[i]);
      word fg = (!ok[i] && emphasizeBad) ? C_TXT : C_SUB;
      lbl(x + 15, y, false, 1, 1, fg, C_BG, SENS_LBL[i]);
    }
  }

  // partial-update caches for the operating screen
  int  g_lastTemp = -999;
  int  g_lastSens = -1;     // packed w|a|c|e bits

  // RTS-reset / baud-bump (unchanged, hardware-proven)
  void resetSync() {
    ConnectorCOM1.RtsMode(SerialBase::LINE_ON);  delay(200);
    ConnectorCOM1.RtsMode(SerialBase::LINE_OFF); delay(4500);   // Diablo16 boot
    ConnectorCOM1.Speed(9600);
    for (int i = 0; i < 12; i++) {
      drain(); uint32_t e = g_err; D->gfx_Cls();
      if (g_err == e) break;
      delay(200);
    }
    drain(); D->gfx_Set(SCREEN_MODE, PORTRAIT);
  }
  bool changeBaud(word rateIdx, uint32_t rawBaud) {
    drainQuiet(3);
    uint16_t op = (uint16_t)F_setbaudWait;                 // 38 = 0x0026
    Serial1.write((uint8_t)(op >> 8)); Serial1.write((uint8_t)op);
    Serial1.write((uint8_t)(rateIdx >> 8)); Serial1.write((uint8_t)rateIdx);
    Serial1.flush();                                       // fully transmit at the OLD baud first
    ConnectorCOM1.Speed(rawBaud);                          // RTS-safe host switch
    delay(130);                                            // display sleeps ~100ms then switches+ACKs
    drainQuiet(5);                                         // absorb the setbaud ACK / transition noise
    for (int i = 0; i < 4; i++) {                          // verify the link at the new baud
      drain(); uint32_t e = g_err; D->gfx_Cls();
      if (g_err == e) return true;
      delay(40);
    }
    resetSync();                                           // verify failed -> force known 9600 state
    return false;
  }
}

namespace KDS {

void begin() {
  Serial1.begin(9600);             // SPE boots at 9600; the lib object + handshake below
  Serial1.ttl(true);               // need it open before resetSync()'s handshake
  D = new Diablo_Serial_4DLib(&Serial1);
  D->Callback4D = onTO;
  D->TimeLimit4D = 1000;
  resetSync();                     // RTS-reset display -> SPE@9600, handshake, portrait
  changeBaud(BAUD_115200, 115200); // bump link to 115200 (~12x faster redraws; 9600 fallback)
  buildWidthCache();               // DejaVu glyph widths for centring (one-time, ~0.5 s)
  drain(); D->txt_Set(TEXT_OPACITY, 1);   // opaque text everywhere (bg colour managed)
}

uint32_t ackErrors() { return g_err; }

void startup(const char* l1, const char* l2) {
  chrome(l1, C_BLUE, l2);
  footerIP("booting");
}

// Unified readiness screen. Title + strip reflect overall readiness; failing
// inputs get a red dot + bright label. The caller adds START when allOk.
void readiness(bool water, bool air, bool co2, bool estop, bool allOk, const char* ip) {
  chrome(allOk ? "READY" : "NOT READY",
         allOk ? C_GRN : C_AMB,
         allOk ? "insert kegs - press start" : "waiting on systems below");
  sensorGrid(84, water, air, co2, estop, true);
  hline(LX, RX, 142, C_TRACK);
  footerIP(ip);
}
void notReady(bool water, bool air, bool co2, bool estop, const char* ip) {
  readiness(water, air, co2, estop, false, ip);
}
void ready(const char* ip) { readiness(true, true, true, true, true, ip); }

void heating(int curC, int tgtC, int pct, const char* ip) {
  chrome("HEATING", C_BLUE, "caustic warming to target");
  char b[16];
  snprintf(b, sizeof(b), "%dC", curC);
  rect(LX - 2, TMR_Y, RX, TMR_CLR_Y2, C_BG);
  lbl(LX, TMR_Y, true, 3, 4, C_TXT, C_BG, b);
  snprintf(b, sizeof(b), "target %dC", tgtC);
  lbl(LX, REM_Y, false, 1, 1, C_SUB, C_BG, b);
  hline(LX, RX, RULE2_Y, C_TRACK);
  snprintf(b, sizeof(b), "%d%%", pct);
  lbl(LX, TMP_Y, true, 2, 2, C_CYAN, C_BG, b);
  lbl(LX, TMPSUB_Y, false, 1, 1, C_SUB, C_BG, "of heat-up window used");
  footerIP(ip);
}

// Operating frame: phase name + position + description, countdown + temp
// fields, sensor grid. Partial updates fill the fields afterwards.
void operatingFrame(const char* phaseName, const char* desc,
                    int phaseIdx, int phaseCount,
                    const char* ip, word stateColour) {
  char meta[40];
  snprintf(meta, sizeof(meta), "PHASE %d/%d - %s", phaseIdx, phaseCount, desc);
  chrome(phaseName, stateColour, meta);
  lbl(LX, REM_Y, false, 1, 1, C_SUB, C_BG, "remaining");
  hline(LX, RX, RULE2_Y, C_TRACK);
  lbl(LX, TMPSUB_Y, false, 1, 1, C_SUB, C_BG, "caustic");
  hline(LX, RX, RULE3_Y, C_TRACK);
  sensorGrid(SENS_Y, true, true, true, true, false);
  g_lastTemp = -999; g_lastSens = -1;       // force first partial update
  footerIP(ip);
}
// legacy 3-arg form (KegDisplay migrates to the rich one; keep compiling)
void operatingFrame(const char* stateName, const char* ip, word barColour) {
  operatingFrame(stateName, "", 0, 0, ip, barColour);
}

void operatingTimer(int minutes, int seconds) {
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", minutes, seconds);
  rect(LX - 2, TMR_Y, RX, TMR_CLR_Y2, C_BG);
  lbl(LX, TMR_Y, true, 4, 5, C_TXT, C_BG, b);
}

void operatingStatus(int tempC, bool water, bool air, bool co2, bool estop, bool mqtt) {
  (void)mqtt;   // footer dots are driven by mqttIndicators()
  if (tempC != g_lastTemp) {
    g_lastTemp = tempC;
    char b[8];
    snprintf(b, sizeof(b), "%dC", tempC);
    rect(LX - 2, TMP_Y, 150, TMP_CLR_Y2, C_BG);
    lbl(LX, TMP_Y, true, 2, 2, C_GRN, C_BG, b);
  }
  int packed = (water << 3) | (air << 2) | (co2 << 1) | (int)estop;
  if (packed != g_lastSens) {
    g_lastSens = packed;
    sensorDot(0, SENS_Y, water); sensorDot(1, SENS_Y, air);
    sensorDot(2, SENS_Y, co2);   sensorDot(3, SENS_Y, !estop ? true : false);
  }
}

void finished(const char* ip) {
  chrome("COMPLETE", C_BLUE, "kegs done - remove and reload");
  lbl(LX, TMR_Y, true, 3, 4, C_GRN, C_BG, "DONE");
  footerIP(ip);
}

void error(const char* msg, const char* ip) {
  chrome("FAULT", C_RED, "machine made safe");
  // wrap the message manually at ~28 chars (F7 is proportional but this is
  // a safe budget for 240 px)
  char line[32];
  int y = 84;
  const char* p = msg;
  while (*p && y < 180) {
    int n = 0;
    int lastSp = -1;
    while (p[n] && n < 28) { if (p[n] == ' ') lastSp = n; n++; }
    if (p[n] && lastSp > 0) n = lastSp;
    memcpy(line, p, n); line[n] = 0;
    lbl(LX, y, false, 1, 1, C_TXT, C_BG, line);
    y += 22;
    p += n; while (*p == ' ') p++;
  }
  footerIP(ip);
}

void stopping(const char* ip) {
  chrome("STOPPING", C_AMB, "clearing keg - please wait");
  lbl(LX, TMR_Y, true, 2, 2, C_AMB, C_BG, "DRAINING");
  footerIP(ip);
}

void halted(const char* ip) {
  chrome("STOPPED", C_AMB, "machine halted by operator");
  lbl(LX, 150, false, 1, 1, C_SUB, C_BG, "START = return to ready");
  footerIP(ip);
}

void message(const char* msg, const char* ip) {
  chrome("MESSAGE", C_BLUE, "");
  lbl(LX, 84, false, 1, 1, C_TXT, C_BG, msg);
  footerIP(ip);
}

void touchEnable() { drain(); D->touch_Set(TOUCH_ENABLE); }

void mark(int x, int y) { disc(x, y, 7, C_TXT); disc(x, y, 3, C_RED); }

void mqttIndicators(bool connected, bool pubActive, bool subActive) {
  word pc = !connected ? C_RED : (pubActive ? C_AMB : C_DIMGRN);
  word sc = !connected ? C_RED : (subActive ? C_CYAN : C_DIMGRN);
  disc(FOOT_DOT_PUB_X, FOOT_DOT_Y, 4, pc);
  disc(FOOT_DOT_SUB_X, FOOT_DOT_Y, 4, sc);
}

void footer(const char* ip) { rect(0, FOOT_Y, DW - 1, DH - 1, C_CARD); footerIP(ip); }

// alert banner in the reserved zone under the rule (flash-toggled by caller)
void banner(const char* text, word colour, bool visible) {
  word c = remap(colour);
  if (visible) {
    rrect(16, BANNER_Y1, 256, BANNER_Y2, 8, c);
    lblC(16, 256, BANNER_Y1 + 6, true, 1, 2, inkOn(c), c, text);
  } else {
    rect(10, BANNER_Y1 - 2, 260, BANNER_Y2 + 2, C_BG);
  }
}

// Secondary button: dark card fill, coloured bold label (direction-C default).
void button(int x, int y, int w, int h, const char* label, word colour) {
  word accent = remap(colour);
  if (accent == C_TRACK || accent == 0x4208) accent = C_SUB;   // disabled nav
  rrect(x, y, x + w, y + h, 8, C_CARDHI);
  int sy = (h >= 40) ? 2 : 1;
  int sx = (h >= 40 && strW(label, true, 2) <= w - 12) ? 2 : 1;
  int ty = y + (h - g_h8 * sy) / 2; if (ty < y + 2) ty = y + 2;
  lblC(x, x + w, ty, true, sx, sy, accent, C_CARDHI, label);
}

// Primary button: solid semantic fill, dark label. For THE action on a screen.
void buttonPrimary(int x, int y, int w, int h, const char* label, word colour) {
  word fill = remap(colour);
  rrect(x, y, x + w, y + h, 10, fill);
  int sy = (h >= 90) ? 3 : 2;
  int sx = 2;
  if (strW(label, true, sx) > w - 16) sx = 1;
  int ty = y + (h - g_h8 * sy) / 2; if (ty < y + 2) ty = y + 2;
  lblC(x, x + w, ty, true, sx, sy, inkOn(fill), fill, label);
}

// ---- generic primitives (settings editor / boot screen in KegDisplay) ----
void screen(const char* title, word barColour) { chrome(title, barColour, ""); }
void text(int x, int y, int sz, word fg, word bg, const char* s) {
  lbl(x, y, false, sz, sz, remap(fg), bg == 0x4208 ? C_CARD : bg, s);
}
void label(int x, int y, bool bold, int sx, int sy, word fg, word bg, const char* s) {
  lbl(x, y, bold, sx, sy, remap(fg), bg, s);
}
void labelRight(int x2, int y, bool bold, int sx, int sy, word fg, word bg, const char* s) {
  lblR(x2, y, bold, sx, sy, remap(fg), bg, s);
}
void rule(int y) { hline(LX, RX, y, C_TRACK); }
void fillRect(int x, int y, int x2, int y2, word col) { rect(x, y, x2, y2, col); }

bool touch(int* x, int* y) {
  static bool down = false;
  word st = rawCmdResp(F_touch_Get, TOUCH_STATUS);
  if (st == TOUCH_PRESSED) {
    int tx = rawCmdResp(F_touch_Get, TOUCH_GETX);
    int ty = rawCmdResp(F_touch_Get, TOUCH_GETY);
    if (!down) { down = true; applyCal(tx, ty, x, y); return true; }
  } else if (st == TOUCH_RELEASED || st == NOTOUCH) {
    down = false;
  }
  return false;
}

bool touchRaw(int* x, int* y) {
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
