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

int  enclosureTempValue = 0;
int  causticTempValue = 0;
int  causticLevelValue = 0;

bool causticTempSensorError = false;
bool enclosureTempSensorError = false;

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
  int rawEnc = analogRead(enclosureTemp);
  int rawCau = analogRead(causticTemp);
  int rawLvl = analogRead(causticLevelSensor);

  if (validateAdc(rawEnc)) {
    enclosureTempValue = filterAdc(enclosureTempValue, rawEnc);
    enclosureTempSensorError = false;
  } else {
    enclosureTempSensorError = true;
  }

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
  pinMode(cycleStart, INPUT);
  pinMode(manualDrain, INPUT);

  pinMode(co2Out, OUTPUT);
  pinMode(cabinFanPWM, OUTPUT);
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
  enclosureTempValue = analogRead(enclosureTemp);
  causticTempValue   = analogRead(causticTemp);
  causticLevelValue  = analogRead(causticLevelSensor);
}

// ---------- Per-loop input read ----------
void hardware_readInputs() {
  isAirOk              = debounceRead(DI_AIR_OK);
  isCo2Ok              = debounceRead(DI_CO2_OK);
  isWaterOk            = debounceRead(DI_WATER_OK);
  isLargeKeg           = debounceRead(DI_LARGE_KEG);
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
  hardware_manageFan();
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
  if (causticTempSensorError ||
      enclosureTempSensorError)                   { errorCode = ERR_SENSOR_FAULT;   return false; }
  if (hardware_getCausticLevel() < MIN_CAUSTIC_LEVEL) {
                                                    errorCode = ERR_CAUSTIC_LEVEL;  return false; }
  return true;
#endif
}

// ---------- All-stop (main-loop only) ----------
void hardware_allStop() {
  digitalWrite(co2Out, LOW);
  digitalWrite(alarmOut, HIGH);
  analogWrite(cabinFanPWM, 0);
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
void hardware_setCabinFan(int pwm)     { analogWrite(cabinFanPWM, pwm); }

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

// ---------- Fan (with hysteresis + safety override) ----------
void hardware_manageFan() {
  static bool fanOn = false;
  static unsigned long lastChangeMs = 0;
  // Hysteresis transitions are rate-limited to avoid PWM thrashing when
  // the analog input is noisy or floating (e.g., bench testing without
  // the enclosure-temp sensor wired). With a real sensor, enclosure
  // temperature changes on the order of seconds, not milliseconds, so
  // this rate limit is invisible during normal operation.
  const unsigned long FAN_MIN_CHANGE_MS = 5000;

  // If the sensor is faulted, hold the fan in its last known state
  // rather than reacting to bogus readings. The accessor returns a
  // safety-tripping value on fault, but holding here is more honest:
  // we don't actually know what the temperature is.
  if (enclosureTempSensorError) return;

  int t = hardware_getEnclosureTemp();

  // Force fan on near the enclosure cutoff regardless of hysteresis or
  // rate limit — this is the safety override.
  if (t >= maxEnclosureTemp - 5) {
    if (!fanOn) {
      hardware_setCabinFan(255);
      fanOn = true;
      lastChangeMs = millis();
    }
    return;
  }

  // Normal hysteresis, rate-limited.
  if (millis() - lastChangeMs < FAN_MIN_CHANGE_MS) return;

  if (t > fanOnTemp && !fanOn) {
    hardware_setCabinFan(255);
    fanOn = true;
    lastChangeMs = millis();
  } else if (t < fanOffTemp && fanOn) {
    hardware_setCabinFan(0);
    fanOn = false;
    lastChangeMs = millis();
  }
}

// ---------- Sensor accessors ----------
// On sensor fault, return a value that triggers the relevant safety branch
// in callers (caustic too cold → won't pass WASHING; enclosure too hot →
// fan forced on).
// Ender 3 Pro nozzle thermistor: 100k NTC, beta 3950, R25 = 100k @ 25C.
// Wiring (bench bringup): thermistor from the +24V rail straight into the analog
// pin — NO external divider; the ClearCore analog input's own load is the bottom
// leg. The effective load was solved from one point (raw ADC 2225 @ 22.2C/72F,
// supply 24.56V, 0-10V/12-bit input) → ~32.2k:
//   V = adc/4095 * 10V ;  R_t = R_LOAD * (VPLUS - V) / V
//   1/T = 1/T25 + ln(R_t/R25)/BETA
// ⚠ With these values V hits the 10V input ceiling near ~43C, so this front-end
// CANNOT sense the caustic range (50-70C). The caustic input (A10) needs a real
// divider sized for that range before non-bench washing. See docs/io-table.md.
static const float THERM_R25   = 100000.0f;
static const float THERM_BETA  = 3950.0f;
static const float THERM_T25   = 298.15f;
static const float THERM_VPLUS = 24.56f;
static const float THERM_VFS   = 10.0f;     // ClearCore analog full-scale (0-10V)
static const float THERM_RLOAD = 32200.0f;  // effective analog-input load to GND

static int thermistorTempC(int adcVal) {
  if (adcVal < 30) return -99;              // open / no current → invalid (fails safe cold)
  float v = (float)adcVal * (THERM_VFS / (float)ADC_MAX);
  if (v < 0.05f || v >= THERM_VPLUS) return -99;
  float rt = THERM_RLOAD * (THERM_VPLUS - v) / v;
  if (rt < 1.0f) return 150;                // near short → very hot
  float tK = 1.0f / (1.0f / THERM_T25 + logf(rt / THERM_R25) / THERM_BETA);
  int tC = (int)(tK - 273.15f + 0.5f);
  if (tC < -40) tC = -40;
  if (tC > 150) tC = 150;
  return tC;
}

int hardware_getCausticTemp() {
  if (causticTempSensorError) return minCausticTemp - 10;
  return thermistorTempC(causticTempValue);
}

int hardware_getEnclosureTemp() {
  if (enclosureTempSensorError) return maxEnclosureTemp + 10;
  return thermistorTempC(enclosureTempValue);
}

int hardware_getCausticLevel() {
  return map(causticLevelValue, 0, ADC_MAX, 0, 100);
}
