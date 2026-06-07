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
extern int  enclosureTempValue;
extern int  causticTempValue;
extern int  causticLevelValue;

// Sensor health flags. True when reading is out of range.
extern bool causticTempSensorError;
extern bool enclosureTempSensorError;

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
void hardware_setCabinFan(int pwmValue);
void hardware_setReadyLamp(bool on);   // GREEN cycle-start indicator on IO5 (dedicated output)

int  hardware_getCausticTemp();
int  hardware_getEnclosureTemp();
int  hardware_getCausticLevel();

void hardware_manageFan();
bool hardware_monitorHeating();
bool hardware_checkHeatingRate();

#endif // KEG_HARDWARE_H
