// ======================================================================
// diablo_touch_test.ino — prove direct Diablo16 SERIAL control from the
// ClearCore. No Workshop4, no ViSi-Genie, no flashing.
//
// The gen4-uLCD-43DT is already running SPE2 (Serial Platform Environment),
// listening for serial graphics commands. This sketch:
//   1. clears the screen
//   2. draws a large RED filled circle in the centre
//   3. enables the resistive touchscreen
//   4. on each touch, toggles the circle RED <-> BLUE and logs the coords
//
// WIRING:
//   - CAT5: ClearCore COM1  <->  4D adapter RJ45              CONNECTED
//   - 4D adapter micro-USB (CP2104)                           DISCONNECTED
//        (both share the one display UART; let the ClearCore own it)
//   - AUX 24V  -> display (already running SPE2, showing splash)
//   - ClearCore USB-C -> PC  (flash this sketch + read the log @115200)
//
// Flash:  ~/dev/teknic-clearcore-cli/scripts/flash.sh \
//             ~/dev/KegWasher/tools/diablo_touch_test /dev/ttyACM0
//
// ----------------------------------------------------------------------
// BRICK NOTE (why Display is a pointer built in setup(), not a global):
//   Diablo_Serial_4DLib's constructor calls _virtualPort->flush(). As a
//   file-scope global that flush runs during C++ static init — BEFORE
//   SysManager/clocks/USB are up — and on the ClearCore it dereferences an
//   uninitialised COM1 SERCOM and HardFaults before setup() runs. The USB
//   CDC then never enumerates and the board drops off the bus; only a
//   double-tap of the on-board RESET (forces the bootloader) recovers it.
//   Building Display in setup() AFTER Serial.begin() keeps USB up first and
//   makes the board always recoverable via flash.sh's 1200-baud touch.
// ======================================================================
#include <ClearCore.h>
#include "Diablo_Serial_4DLib.h"
#include "Diablo_Const4D.h"

Diablo_Serial_4DLib *Display = nullptr;   // built in setup(), see BRICK NOTE

int cx, cy, r;
bool isBlue = false;
volatile uint32_t displayTimeouts = 0;

// 4D library timeout handler. Runs from inside the lib's read loop when the
// display doesn't answer within TimeLimit4D (2 s). Heap-allocated objects are
// NOT zero-initialised, so Callback4D MUST be set explicitly — otherwise the
// first timeout calls through an uninitialised pointer. Keep it minimal.
void onDisplayTimeout(int errCode, unsigned char errByte) {
  displayTimeouts++;
}

void setup() {
  Serial.begin(115200);                 // bring USB CDC up FIRST -> always recoverable
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) { }

  Serial.println(F("== diablo_touch_test =="));

  Serial1.begin(9600);                  // SPE comms baud (splash showed "Comms 9600"), TTL default
  // COM1 RTS is wired to the display RESET line on the 4D adapter; LINE_OFF
  // = reset released = display running (matches KegDisplay's end state).
  ConnectorCOM1.RtsMode(SerialBase::LINE_OFF);
  delay(800);                           // let the link settle

  Display = new Diablo_Serial_4DLib(&Serial1);   // ctor flush() now safe (Serial1 open)
  Display->Callback4D = onDisplayTimeout;        // REQUIRED: heap obj isn't zero-init
  Display->TimeLimit4D = 2000;                    // 2 s per-command timeout (explicit)

  Display->gfx_Cls();

  // Query the live (orientation-aware) screen size so we centre correctly
  // regardless of portrait/landscape.
  int w = (int)Display->gfx_Get(GFX_XMAX) + 1;   // current width
  int h = (int)Display->gfx_Get(GFX_YMAX) + 1;   // current height
  if (w < 32 || h < 32) {               // query failed -> fall back to stated panel
    w = 272; h = 480;
    Serial.println(F("gfx_Get failed; assuming 272x480"));
  }
  cx = w / 2;
  cy = h / 2;
  r  = (min(w, h) / 2) - 20;
  if (r < 20) r = 60;

  Serial.print(F("screen ")); Serial.print(w); Serial.print('x'); Serial.println(h);
  Serial.print(F("circle @ ")); Serial.print(cx); Serial.print(',');
  Serial.print(cy); Serial.print(F("  r=")); Serial.println(r);
  Serial.print(F("display timeouts so far: ")); Serial.println(displayTimeouts);

  Display->gfx_CircleFilled(cx, cy, r, RED);
  Display->touch_Set(TOUCH_ENABLE);
  Serial.println(F("RED circle drawn, touch enabled. Touch screen -> toggles BLUE/RED."));
}

void loop() {
  word st = Display->touch_Get(TOUCH_STATUS);
  if (st == TOUCH_PRESSED) {
    word tx = Display->touch_Get(TOUCH_GETX);
    word ty = Display->touch_Get(TOUCH_GETY);
    isBlue = !isBlue;
    Display->gfx_CircleFilled(cx, cy, r, isBlue ? BLUE : RED);
    Serial.print(F("TOUCH @ ")); Serial.print(tx); Serial.print(',');
    Serial.print(ty); Serial.print(F("  -> ")); Serial.println(isBlue ? F("BLUE") : F("RED"));
  }
  delay(30);
}
