// ======================================================================
// diablo_touch_cal.ino — 3-point touchscreen calibration for the gen4-uLCD-43DT
// over raw serial (KegDisplaySerial). Tap the 3 crosshairs; the sketch averages
// each touch, solves the affine map  screen = [A B C; D E F] . [rawx rawy 1],
// prints the 6 coefficients over USB @115200, applies them, then enters a VERIFY
// mode (tap anywhere -> a dot should land under your finger).
//
// Copy the printed coefficients into the firmware (KDS::setTouchCalibration(...)
// on boot) to persist the calibration. NO ViSi-Genie.
// ======================================================================
#include <ClearCore.h>
#include "KegDisplaySerial.h"

// calibration targets (portrait 272x480), well spread + non-collinear
const int NPTS = 3;
const int SX[NPTS] = { 40, 232, 136 };
const int SY[NPTS] = { 70,  70, 410 };

// --- 3x3 linear solve (Cramer) for the affine coefficients ---
float det3(float m[3][3]) {
  return m[0][0]*(m[1][1]*m[2][2]-m[1][2]*m[2][1])
       - m[0][1]*(m[1][0]*m[2][2]-m[1][2]*m[2][0])
       + m[0][2]*(m[1][0]*m[2][1]-m[1][1]*m[2][0]);
}
void solve3(float M[3][3], float r[3], float out[3]) {
  float det = det3(M);
  for (int c = 0; c < 3; c++) {
    float Mc[3][3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++) Mc[i][j] = (j == c) ? r[i] : M[i][j];
    out[c] = det3(Mc) / det;
  }
}

// block until a stable touch, return its averaged RAW coords
void captureTouch(int* rx, int* ry) {
  int x, y;
  while (!KDS::touchRaw(&x, &y)) { delay(20); }   // wait for press
  long sx = 0, sy = 0; int n = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 300) {                   // average ~300ms of samples
    if (KDS::touchRaw(&x, &y)) { sx += x; sy += y; n++; }
    delay(15);
  }
  *rx = n ? (int)(sx / n) : x;
  *ry = n ? (int)(sy / n) : y;
  while (KDS::touchRaw(&x, &y)) { delay(20); }     // wait for release
  delay(250);                                      // debounce
}

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) {}
  Serial.println(F("== diablo_touch_cal =="));
  KDS::begin();
  KDS::touchEnable();

  int rawX[NPTS], rawY[NPTS];
  for (int i = 0; i < NPTS; i++) {
    char msg[20]; snprintf(msg, sizeof(msg), "TAP TARGET %d/%d", i + 1, NPTS);
    KDS::message(msg, "");
    KDS::mark(SX[i], SY[i]);
    captureTouch(&rawX[i], &rawY[i]);
    KDS::mark(SX[i], SY[i]);                        // re-mark (confirm)
    Serial.print(F("pt ")); Serial.print(i);
    Serial.print(F(": screen(")); Serial.print(SX[i]); Serial.print(','); Serial.print(SY[i]);
    Serial.print(F(") raw(")); Serial.print(rawX[i]); Serial.print(','); Serial.print(rawY[i]); Serial.println(')');
  }

  // solve affine: M=[rawx rawy 1], rhs = screen X then screen Y
  float M[3][3] = {
    { (float)rawX[0], (float)rawY[0], 1 },
    { (float)rawX[1], (float)rawY[1], 1 },
    { (float)rawX[2], (float)rawY[2], 1 },
  };
  float rX[3] = { (float)SX[0], (float)SX[1], (float)SX[2] };
  float rY[3] = { (float)SY[0], (float)SY[1], (float)SY[2] };
  float abc[3], def[3];
  solve3(M, rX, abc);
  solve3(M, rY, def);

  Serial.println(F("=== CALIBRATION COEFFICIENTS ==="));
  Serial.print(F("KDS::setTouchCalibration("));
  Serial.print(abc[0], 6); Serial.print(F(", ")); Serial.print(abc[1], 6); Serial.print(F(", ")); Serial.print(abc[2], 4); Serial.print(F(", "));
  Serial.print(def[0], 6); Serial.print(F(", ")); Serial.print(def[1], 6); Serial.print(F(", ")); Serial.print(def[2], 4);
  Serial.println(F(");"));

  KDS::setTouchCalibration(abc[0], abc[1], abc[2], def[0], def[1], def[2]);
  KDS::message("CAL DONE - tap to test", "");
}

void loop() {
  int x, y;
  if (KDS::touch(&x, &y)) {                          // calibrated coords now
    KDS::mark(x, y);
    Serial.print(F("VERIFY touch -> (")); Serial.print(x); Serial.print(','); Serial.print(y); Serial.println(')');
  }
  delay(30);
}
