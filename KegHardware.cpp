// ======================================================================
// KegHardware.cpp - I/O, debounced inputs, filtered analogs, heater control
// ======================================================================
#include "KegHardware.h"
#include "KegDiagnostics.h"
#include <math.h>

// ---------- Public state ----------
bool isAirOk = false;
bool isCo2Ok = false;
volatile bool isEstopActive = false;
bool isWaterOk = false;
bool isLargeKeg = false;
bool isCycleStartPressed = false;
bool isManualDrainPressed = false;

int  causticTempValue = 0;
int  causticLevelValue = 0;

bool causticTempSensorError = false;

bool          isHeaterActive = false;
unsigned long heatingStartTime = 0;
int           heatingStartTemp = 0;
int           heatingLastTemp = 0;
unsigned long heatingLastCheckTime = 0;

volatile bool estopFlag = false;

// ---------- Debounce ----------
#define DEBOUNCE_MS 50

enum DigitalIdx {
  DI_AIR_OK = 0,
  DI_CO2_OK,
  DI_WATER_OK,
  DI_LARGE_KEG,
  DI_CYCLE_START,
  DI_MANUAL_DRAIN,
  DI_COUNT
};

static const uint8_t debouncedPins[DI_COUNT] = {
  airOk, co2Ok, waterOk, largeKeg, cycleStart, manualDrain
};

static bool          db_lastReading[DI_COUNT] = {false};
static bool          db_stable[DI_COUNT] = {false};
static unsigned long db_changeMs[DI_COUNT] = {0};

static bool debounceRead(DigitalIdx idx) {
  bool reading = digitalRead(debouncedPins[idx]);
  if (reading != db_lastReading[idx]) {
    db_changeMs[idx] = millis();
    db_lastReading[idx] = reading;
  }
  if ((millis() - db_changeMs[idx]) > DEBOUNCE_MS) {
    db_stable[idx] = reading;
  }
  return db_stable[idx];
}

// NOTE: the IO1 START-button "ready lamp" (time-multiplexed LED on the same pin)
// was reverted — the active-low read auto-asserted START because IO1 reads LOW
// when *not* pressed (the indicator LED does NOT pull the node high as the wiring
// suggested). Needs bench measurement of IO1-S (pressed vs released, as a plain
// input, with/without a pull-up) before re-attempting. cycleStart is back to the
// original active-high debounced read.

// ---------- Analog filtering ----------
// Reject the extreme ends of the ADC range — those almost always mean
// open- or short-circuit on the sensor wiring rather than a real reading.
static const int ADC_REJECT_LOW  = 5;
static const int ADC_REJECT_HIGH = ADC_MAX - 5;

static bool validateAdc(int raw) {
  return raw > ADC_REJECT_LOW && raw < ADC_REJECT_HIGH;
}

// IIR low-pass: new = (3*prev + raw) / 4. Smooths sensor jitter without
// adding meaningful lag at the read cadence used here.
static int filterAdc(int prev, int raw) {
  return (prev * 3 + raw) / 4;
}

static void readTemperatureSensors() {
  int rawCau = analogRead(causticTemp);
  int rawLvl = analogRead(causticLevelSensor);

  if (validateAdc(rawCau)) {
    causticTempValue = filterAdc(causticTempValue, rawCau);
    causticTempSensorError = false;
  } else {
    causticTempSensorError = true;
  }

  // Level sensor is non-safety-critical: hold last good value silently.
  if (validateAdc(rawLvl)) {
    causticLevelValue = filterAdc(causticLevelValue, rawLvl);
  }
}

// ---------- ESTOP ISR ----------
// The hardware ESTOP loop physically de-powers the heater contactor and
// solenoid valves, so this ISR's job is just to flip the software state
// fast: light the alarm (ClearCore-native pin), kill the ClearCore-native
// outputs that aren't on the hardware loop, and flag the main loop.
//
// We deliberately do NOT call diagnostics_logEvent or write CCIO outputs
// from this ISR. Logging uses Serial which is interrupt-driven; CCIO
// writes go over a COM0 serial transaction. Either could deadlock from
// an ISR.
static void hardware_estopIsr() {
  digitalWrite(alarmOut, HIGH);
  digitalWrite(co2Out, LOW);
  isEstopActive = true;
  estopFlag = true;
}

bool hardware_consumeEstopFlag() {
  if (!estopFlag) return false;
  estopFlag = false;
  return true;
}

// ---------- Init ----------
void hardware_init() {
  CcioPort.Mode(Connector::CCIO);
  CcioPort.PortOpen();

  pinMode(airOk, INPUT);
  pinMode(co2Ok, INPUT);
  pinMode(ESTOP, INPUT);
  pinMode(waterOk, INPUT);
  pinMode(largeKeg, INPUT);
  pinMode(cycleStart, INPUT);   // NO momentary switch S<->G; IO0-5 inputs are negative-true
  pinMode(manualDrain, INPUT);

  pinMode(co2Out, OUTPUT);
  pinMode(readyLedOut, OUTPUT);   // GREEN cycle-start indicator (was the fan on IO5)
  pinMode(alarmOut, OUTPUT);
  pinMode(drainOut, OUTPUT);
  pinMode(waterOut, OUTPUT);
  pinMode(airOut, OUTPUT);
  pinMode(causticOut, OUTPUT);
  pinMode(pumpOut, OUTPUT);
  pinMode(sanitizerOut, OUTPUT);
  pinMode(causticHeaterOut, OUTPUT);

  hardware_allStop();
  hardware_setAlarm(false);   // boot is not a fault: allStop lights IO4, clear it

  // Fail-safe E-stop. A normally-CLOSED safety chain runs from GND through the
  // drive-OK contacts (pump, etc.) and the E-stop switch into DI8. DI6-DI8 are
  // negative-true SINKING inputs with an internal 3.3V/10k pull-up, so:
  //   chain healthy (closed) → DI8 sunk to GND → digitalRead HIGH → OK
  //   E-stop pressed / drive fault / broken wire (open) → pull-up → LOW → TRIP
  // The trip edge is therefore HIGH→LOW = FALLING. (INPUT_PULLUP is a no-op on
  // ClearCore — the pull-up is fixed in hardware regardless of pinMode.)
  attachInterrupt(digitalPinToInterrupt(ESTOP), hardware_estopIsr, FALLING);

  // Seed analog filters with one valid sample so the first cycle isn't
  // smeared by the zero-initialised previous value.
  causticTempValue   = analogRead(causticTemp);
  causticLevelValue  = analogRead(causticLevelSensor);
}

// ---------- Per-loop input read ----------
void hardware_readInputs() {
  isAirOk              = debounceRead(DI_AIR_OK);
  isCo2Ok              = debounceRead(DI_CO2_OK);
  isWaterOk            = debounceRead(DI_WATER_OK);
  isLargeKeg           = debounceRead(DI_LARGE_KEG);
  // IO1 is a plain NO switch S<->G. ClearCore digital inputs are negative-true:
  // digitalRead() returns HIGH when the pin is at GND (pressed), LOW when open
  // (released). So the press reads HIGH -> ACTIVE-HIGH. (Verified vs the manual +
  // libClearCore DigitalIn::UpdateFilterState; see clearcore_io_input_behavior memo.)
  isCycleStartPressed  = debounceRead(DI_CYCLE_START);
  isManualDrainPressed = debounceRead(DI_MANUAL_DRAIN);

  // ESTOP is intentionally not debounced — fastest possible response.
  // Fail-safe NC safety chain to GND on a negative-true sinking input:
  //   chain healthy (closed) → DI8 at GND → digitalRead HIGH → not active
  //   E-stop / drive fault / wire break (open) → 3.3V pull-up → LOW → ACTIVE
  // The ISR catches the FALLING edge (HIGH→LOW = trip); this poll tracks the
  // held/released state so the loop reacts even if the edge is missed, and so
  // the system recovers once the chain is restored.
  isEstopActive = (digitalRead(ESTOP) == LOW);

  readTemperatureSensors();

  if (isHeaterActive) hardware_monitorHeating();
}

// ---------- System-go check ----------
bool hardware_allSystemsGo() {
#ifdef BENCH_MODE
  // Bench-mode: bypass operating-policy gates so the state machine can
  // progress without real plumbing. Hardware-safety paths (heater
  // interlocks, ESTOP ISR, watchdog) remain untouched.
  return true;
#else
  if (isEstopActive)                              { errorCode = ERR_ESTOP;          return false; }
  if (!isAirOk)                                   { errorCode = ERR_AIR_PRESSURE;   return false; }
  if (!isCo2Ok)                                   { errorCode = ERR_CO2_PRESSURE;   return false; }
  if (!isWaterOk)                                 { errorCode = ERR_WATER_PRESSURE; return false; }
  if (causticTempSensorError)                     { errorCode = ERR_SENSOR_FAULT;   return false; }
  if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
                                                    errorCode = ERR_CAUSTIC_LEVEL;  return false; }
  return true;
#endif
}

// ---------- All-stop (main-loop only) ----------
void hardware_allStop() {
  digitalWrite(co2Out, LOW);
  digitalWrite(alarmOut, HIGH);
  digitalWrite(readyLedOut, LOW);     // green off on all-stop (fault/estop)
  digitalWrite(drainOut, LOW);
  digitalWrite(waterOut, LOW);
  digitalWrite(airOut, LOW);
  digitalWrite(causticOut, LOW);
  digitalWrite(pumpOut, LOW);
  digitalWrite(sanitizerOut, LOW);
  digitalWrite(causticHeaterOut, LOW);
  isHeaterActive = false;
}

// ---------- Output setters ----------
void hardware_setCo2(bool state)       { digitalWrite(co2Out, state ? HIGH : LOW); }
void hardware_setAlarm(bool state)     { digitalWrite(alarmOut, state ? HIGH : LOW); }
void hardware_setDrain(bool state)     { digitalWrite(drainOut, state ? HIGH : LOW); }
void hardware_setWater(bool state)     { digitalWrite(waterOut, state ? HIGH : LOW); }
void hardware_setAir(bool state)       { digitalWrite(airOut, state ? HIGH : LOW); }
void hardware_setCaustic(bool state)   { digitalWrite(causticOut, state ? HIGH : LOW); }
void hardware_setPump(bool state)      { digitalWrite(pumpOut, state ? HIGH : LOW); }
void hardware_setSanitizer(bool state) { digitalWrite(sanitizerOut, state ? HIGH : LOW); }
// GREEN cycle-start / ready indicator on IO5 — a dedicated output (no time-mux).
void hardware_setReadyLamp(bool on)    { digitalWrite(readyLedOut, on ? HIGH : LOW); }

// PAUSED indicator: both stacklight lamps (IO4 red + IO5 green, PWM-capable)
// breathe on a ~1.6 s triangle so a held machine is visible across the room.
// digitalWrite from the normal paths overrides PWM when the hold ends.
void hardware_pulseLamps() {
  unsigned long t = millis() % 1066UL;     // ~1.07 s (was 1.6 s; -33%)
  int duty = (t < 533UL) ? (int)(t * 255UL / 533UL)
                         : (int)((1066UL - t) * 255UL / 533UL);
  analogWrite(alarmOut, duty);
  analogWrite(readyLedOut, 255 - duty);    // alternate: green peaks as red dips
}

// ---------- Heater (interlocked) ----------
void hardware_setCausticHeater(bool state) {
  if (state) {
    if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
      digitalWrite(causticHeaterOut, LOW);
      isHeaterActive = false;
      errorCode = ERR_CAUSTIC_LEVEL;
      diagnostics_logEvent("Heater blocked: low caustic level");
      return;
    }
    if (hardware_getCausticTemp() >= maxCausticTemp) {
      digitalWrite(causticHeaterOut, LOW);
      isHeaterActive = false;
      errorCode = ERR_HEATER_OVERTEMP;
      diagnostics_logEvent("Heater blocked: overtemp");
      return;
    }
    if (!isHeaterActive) {
      heatingStartTime     = millis();
      heatingStartTemp     = hardware_getCausticTemp();
      heatingLastTemp      = heatingStartTemp;
      heatingLastCheckTime = heatingStartTime;
      char buf[48];
      snprintf(buf, sizeof(buf), "Heater ON, start %dC", heatingStartTemp);
      diagnostics_logEvent(buf);
    }
  } else if (isHeaterActive) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Heater OFF, final %dC, %lus",
             hardware_getCausticTemp(),
             (millis() - heatingStartTime) / 1000UL);
    diagnostics_logEvent(buf);
  }
  digitalWrite(causticHeaterOut, state ? HIGH : LOW);
  isHeaterActive = state;
}

bool hardware_monitorHeating() {
  if (!isHeaterActive) return true;

  int currentTemp = hardware_getCausticTemp();
  unsigned long now = millis();

  if (currentTemp >= maxCausticTemp) {
    hardware_setCausticHeater(false);
    errorCode = ERR_HEATER_OVERTEMP;
    diagnostics_logEvent("Heater overtemp shutdown");
    return false;
  }

  if (now - heatingStartTime > MAX_HEATING_TIME) {
    hardware_setCausticHeater(false);
    errorCode = ERR_HEATING_TIMEOUT;
    diagnostics_logEvent("Heater timeout");
    return false;
  }

  if (now - heatingLastCheckTime > 60000UL) {
    int delta = currentTemp - heatingLastTemp;
    if (delta < MIN_HEATING_RATE && currentTemp < (optimalCausticTemp - 5)) {
      char buf[48];
      snprintf(buf, sizeof(buf), "Slow heating: %dC/min", delta);
      diagnostics_logEvent(buf);
    }
    heatingLastTemp = currentTemp;
    heatingLastCheckTime = now;
  }

  if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
    hardware_setCausticHeater(false);
    errorCode = ERR_CAUSTIC_LEVEL;
    diagnostics_logEvent("Heater shutdown: low level");
    return false;
  }

  return true;
}

bool hardware_checkHeatingRate() {
  if (!isHeaterActive || (millis() - heatingStartTime < 120000UL)) return true;
  int totalDelta = hardware_getCausticTemp() - heatingStartTemp;
  float minutes = (millis() - heatingStartTime) / 60000.0f;
  if (minutes <= 0.0f) return true;
  return (totalDelta / minutes) >= MIN_HEATING_RATE;
}

// ---------- Sensor accessors ----------
// On sensor fault, return a value that triggers the relevant safety branch in
// callers (caustic too cold → won't pass WASHING).
//
// (The enclosure NTC + fan-management code was removed: the enclosure fan is now a
//  standalone self-regulating unit with its own thermometer, off the controller.)

// ---- Caustic temp: ProSense ETS50N-150-1001, 4-20mA transmitter on A10 ----
// Read across a shunt (A10 -> GND) on the ClearCore 0-10V / 12-bit input (range +
// depth confirmed against the ClearCore manual + AdcManager: 0-10V, 0-4095).
// Using a 470 ohm shunt: 4mA=1.88V .. 20mA=9.40V. The transmitter's 4-20mA output
// is scaled to its native measuring range -50..150C (device default); if you
// rescale the span in the ETS menu, change ETS_T_AT_4MA / ETS_T_AT_20MA to match.
// <~3.6mA (~1.7V) = open / under-range loop -> fail cold.
// ETS_SHUNT_OHMS MUST equal the installed resistor — set it to the MEASURED value
// (470 ohm ±5% = 446..494) for best accuracy; nominal 470 is the default.
static const float ETS_SHUNT_OHMS = 470.0f;
static const float ETS_T_AT_4MA   = -50.0f;
static const float ETS_T_AT_20MA  = 150.0f;
static const float ETS_MA_FAULT   = 3.6f;

int hardware_getCausticTempC10() {
  // A fully open/short loop (0V or >10V) is already flagged by the ADC rail-reject.
  if (causticTempSensorError) return (int)minCausticTemp - 10;
  float v  = (float)causticTempValue / (float)ADC_MAX * 10.0f;   // ClearCore 0-10V input
  float mA = v / ETS_SHUNT_OHMS * 1000.0f;                       // current through the shunt
  if (mA < ETS_MA_FAULT) return (int)minCausticTemp - 10;        // loop fault -> fail cold
  float t = ETS_T_AT_4MA + (mA - 4.0f) * (ETS_T_AT_20MA - ETS_T_AT_4MA) / 16.0f;
  return (int)lroundf(t * 10.0f) + tempCalOffsetC10;   // tenths of a degree C
}

int hardware_getCausticTemp() {
  int c10 = hardware_getCausticTempC10();
  return (c10 >= 0) ? (c10 + 5) / 10 : (c10 - 5) / 10;   // round-to-nearest whole C
}

int hardware_getCausticLevel() {
  return map(causticLevelValue, 0, ADC_MAX, 0, 100);
}
