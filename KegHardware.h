// ======================================================================
// KegHardware.h - I/O, debounced inputs, filtered analogs, heater control
// ======================================================================
#ifndef KEG_HARDWARE_H
#define KEG_HARDWARE_H

#include <Arduino.h>
#include "KegConfig.h"
#include "ClearCore.h"

// Debounced digital input states (active per io-table.md).
extern bool isAirOk;
extern bool isCo2Ok;
extern volatile bool isEstopActive;       // ISR + polled
extern bool isWaterOk;
extern bool isLargeKeg;
extern bool isCycleStartPressed;
extern bool isManualDrainPressed;

// Filtered analog readings (raw ADC counts; convert via getters).
extern int  causticTempValue;
extern bool isCausticLevelOk;   // NC float switch, debounced (true = level OK)

// Sensor health flags. True when reading is out of range.
extern bool causticTempSensorError;

// Heater state tracking.
extern bool          isHeaterActive;
extern unsigned long heatingStartTime;
extern int           heatingStartTemp;
extern int           heatingLastTemp;
extern unsigned long heatingLastCheckTime;

// Latched in the ESTOP ISR; main loop consumes via hardware_consumeEstopFlag().
extern volatile bool estopFlag;

void hardware_init();
void hardware_readInputs();
bool hardware_allSystemsGo();
void hardware_allStop();              // main-loop only; performs full shutdown
bool hardware_consumeEstopFlag();     // returns true once after each ESTOP trigger

void hardware_setCo2(bool state);
void hardware_setAlarm(bool state);
void hardware_setDrain(bool state);
void hardware_setWater(bool state);
void hardware_setAir(bool state);
void hardware_setCaustic(bool state);
void hardware_setPump(bool state);
void hardware_setSanitizer(bool state);
void hardware_setCausticHeater(bool state);
void hardware_setReadyLamp(bool on);   // GREEN cycle-start indicator on IO5 (dedicated output)
void hardware_setKegsDone(bool on);    // COMPLETE / swap-kegs signal on CCIO-A7

// Shadow of the driven actuator outputs, for the operating display's OUT grid.
// Bit order: 0=drain 1=water 2=air 3=caustic 4=pump 5=sanitizer 6=co2 7=heater.
byte hardware_getOutputBits();
void hardware_pulseLamps();            // HELD: breathe IO4+IO5 via PWM (call per loop tick)
void hardware_pulseReadyLamp();        // COMPLETE: breathe IO5 green alone (call per loop tick)

int  hardware_getCausticTemp();      // whole degrees C (rounded, calibrated)
int  hardware_getCausticTempC10();   // tenths of a degree C (calibrated) — display precision

bool hardware_monitorHeating();
bool hardware_checkHeatingRate();

#endif // KEG_HARDWARE_H
