// ======================================================================
// KegDisplaySerial.h — draw the KegWasher UI on a gen4-uLCD-43DT (Diablo16)
// using RAW SPE serial commands from the ClearCore. No ViSi-Genie/Workshop4.
//
// Firmware-independent core: all functions take plain params (the firmware's
// display_*() wrappers call these with live globals). Uses only write-only
// (GetAck) draw commands with RX draining → no response-stream desync.
//
// Panel: 480x272 RGB565, resistive touch, SPE @9600 TTL on ClearCore COM1.
// ======================================================================
#pragma once
#include <Arduino.h>

namespace KDS {

void     begin();                 // Serial1 init + RTS reset-release + SPE handshake
uint32_t ackErrors();             // running count of unACKed commands (health metric)

// ---- full-screen renderers (call on state entry) ----
void startup(const char* line1, const char* line2);
void notReady(bool water, bool air, bool co2, bool estop, const char* ip);
void ready(const char* ip);
void heating(int curC, int tgtC, int pct, const char* ip);
void readiness(bool water, bool air, bool co2, bool estop, bool level, bool allOk, const char* ip);
void operatingFrame(const char* phaseName, const char* desc,
                    int phaseIdx, int phaseCount,
                    const char* ip, word stateColour);                        // static chrome
void operatingFrame(const char* stateName, const char* ip, word barColour);   // legacy 3-arg
void finished(const char* ip);
void error(const char* msg, const char* ip);
void message(const char* msg, const char* ip);
void stopping(const char* ip);    // STOP/DRAIN evacuation in progress
void halted(const char* ip);      // de-energized after a STOP/DRAIN; START recovers

// ---- partial updates for the operating screen (no full redraw → no flicker) ----
void operatingTimer(int minutes, int seconds);
void cycleProgress(unsigned long cycleElapsedMs, unsigned long cycleTotalMs);  // edge-strip countdown
void operatingStatus(int tempC10, bool water, bool air, bool co2, bool estop, bool mqtt);  // temp in tenths C (legacy 2x2 grid)
void operatingTemp(int tempC10);               // temp-only partial update (tenths C)
void operatingIO(byte outBits, byte inBits);   // OUT (8 actuators) / IN (5 inputs) dot grid
void operatingLowTemp(bool show);              // steady LOW TEMP banner; safe to call per tick
void readinessTemp(int tempC, bool warn);      // tank temp cell on READY/NOT_READY; per tick OK

// ---- touch ----
void touchEnable();
bool touch(int* x, int* y);       // true once per fresh press; fills *x,*y (CALIBRATED coords)
bool touchRaw(int* x, int* y);    // raw uncalibrated touch_Get sample; true while pressed
void mark(int x, int y);          // draw a small marker at a panel coord (touch feedback)

// ---- touch calibration (host-side affine: screen = [a b c; d e f] * [rawx rawy 1]) ----
void setTouchCalibration(float a, float b, float c, float d, float e, float f);

// ---- footer MQTT pub/sub indicators (partial update; call on MQTT state change) ----
void mqttIndicators(bool connected, bool pubActive, bool subActive,
                    bool webActive = false);   // web = HTTP editor activity dot
void footer(const char* ip);                                // redraw footer bar + IP + P/S labels
// Alert strip. Default y = the reserved zone under the title rule; pass y1/y2
// to place it elsewhere (READY's zone is occupied by the sensor grid).
void banner(const char* text, word colour, bool visible, int y1 = -1, int y2 = -1);
void button(int x, int y, int w, int h, const char* label, word colour);         // secondary (card + coloured label)
void buttonPrimary(int x, int y, int w, int h, const char* label, word colour);  // primary (solid fill, dark label)

// ---- generic primitives (used by the settings editor in KegDisplay) ----
void screen(const char* title, word barColour);              // edge strip + title + rule + footer strip
void text(int x, int y, int sz, word fg, word bg, const char* s);  // legacy: FONT_7 at sz multiplier
void label(int x, int y, bool bold, int sx, int sy, word fg, word bg, const char* s);  // DejaVu, pixel pos
void label4(int x, int y, word fg, word bg, const char* s);   // FONT_4 12x16 subtitle face
void fontMetrics(int* cell, int* capTop, int* capH);          // measured FONT_8 metrics
void labelRight(int x2, int y, bool bold, int sx, int sy, word fg, word bg, const char* s);
void rule(int y);                                            // thin separator line, content width
void fillRect(int x, int y, int x2, int y2, word col);       // filled rect (clear/refresh a field)

} // namespace KDS
