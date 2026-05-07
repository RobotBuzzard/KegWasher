// ======================================================================
// DisplayTest.ino - Smoke test: print "TEST" on the Goldelox 128x128
// ======================================================================
// Minimal sketch to verify the toolchain + ClearCore + Goldelox path.
// No CCIO, no SD, no inputs — just init the display on COM1 and write
// one big word.
//
// Compile:  arduino-cli compile --fqbn ClearCore:sam:clearcore .
// Upload:   arduino-cli upload  --fqbn ClearCore:sam:clearcore -p /dev/ttyACM0 .
// ======================================================================
#include <Arduino.h>
#include <ClearCore.h>
#include <Goldelox_Serial_4DLib.h>
#include <Goldelox_Const4D.h>

Goldelox_Serial_4DLib Display(&Serial1);

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600);

  // Goldelox hardware reset via COM1 RTS line
  ConnectorCOM1.RtsMode(SerialBase::LINE_ON);
  delay(1000);
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
  delay(3000);

  Display.gfx_ScreenMode(2);       // portrait
  Display.gfx_BGcolour(BLACK);
  Display.gfx_Cls();
  Display.txt_FGcolour(WHITE);
  Display.txt_Width(3);
  Display.txt_Height(3);
  Display.gfx_MoveTo(25, 50);
  Display.putstr("TEST");

  Serial.println("DisplayTest: TEST written to Goldelox");
}

void loop() {
}
