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
// Push the state-timer start forward by deltaMs so paused time doesn't count
// toward the stage duration. Called on RESUME with the time spent paused.
void timers_shiftStateStart(unsigned long deltaMs);
unsigned long timers_getStateElapsed();
unsigned long timers_getCycleElapsed();
bool timers_isStateDone(unsigned long duration);
// Pass the LATCHED keg size (kegSizeLatched), not the live pin (isLargeKeg) —
// see the contract note in KegTimers.cpp / KegStateMachine.h.
unsigned long timers_adjustForKegSize(unsigned long baseTime, bool isLarge);

#endif // KEG_TIMERS_H
