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
void operatingFrame(const char* stateName, const char* ip);   // static chrome
void finished(const char* ip);
void error(const char* msg, const char* ip);
void message(const char* msg, const char* ip);

// ---- partial updates for the operating screen (no full redraw → no flicker) ----
void operatingTimer(int minutes, int seconds);
void operatingStatus(int tempC, bool water, bool air, bool co2, bool estop, bool mqtt);

// ---- touch ----
void touchEnable();
bool touch(int* x, int* y);       // true once per fresh press; fills *x,*y (CALIBRATED coords)
bool touchRaw(int* x, int* y);    // raw uncalibrated touch_Get sample; true while pressed
void mark(int x, int y);          // draw a small marker at a panel coord (touch feedback)

// ---- touch calibration (host-side affine: screen = [a b c; d e f] * [rawx rawy 1]) ----
void setTouchCalibration(float a, float b, float c, float d, float e, float f);

// ---- footer MQTT pub/sub indicators (partial update; call on MQTT state change) ----
void mqttIndicators(bool connected, bool pubActive, bool subActive);

} // namespace KDS
