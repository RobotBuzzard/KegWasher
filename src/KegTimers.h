// ======================================================================
// KegTimers.h - State elapsed time + keg-size-modifier helper
// ======================================================================
#ifndef KEG_TIMERS_H
#define KEG_TIMERS_H

#include <Arduino.h>
#include "KegConfig.h"

extern unsigned long currentMillis;
extern unsigned long previousStateMillis;
extern unsigned long stateElapsedTime;
extern unsigned long cycleStartTime;

void timers_init();
void timers_update();
void timers_resetStateTimer();
unsigned long timers_getStateElapsed();
unsigned long timers_getCycleElapsed();
bool timers_isStateDone(unsigned long duration);
unsigned long timers_adjustForKegSize(unsigned long baseTime);

#endif // KEG_TIMERS_H
