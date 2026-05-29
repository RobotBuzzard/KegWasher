// ======================================================================
// KegTimers.cpp
// ======================================================================
// All elapsed-time math here uses (now - then) on unsigned longs, which
// is naturally safe across the ~49-day millis() rollover as long as the
// interval being measured is less than 2^31 ms (~24 days). Every state
// duration here is measured in minutes, so this is comfortable.
// ======================================================================
#include "KegTimers.h"  // largeKegMod comes from KegConfig.h via this header

unsigned long currentMillis = 0;
unsigned long previousStateMillis = 0;
unsigned long stateElapsedTime = 0;
unsigned long cycleStartTime = 0;

void timers_init() {
  currentMillis = millis();
  previousStateMillis = currentMillis;
  stateElapsedTime = 0;
  cycleStartTime = currentMillis;
}

void timers_update() {
  currentMillis = millis();
  stateElapsedTime = currentMillis - previousStateMillis;
}

void timers_resetStateTimer() {
  previousStateMillis = currentMillis;
  stateElapsedTime = 0;
}

unsigned long timers_getStateElapsed() {
  return stateElapsedTime;
}

unsigned long timers_getCycleElapsed() {
  return currentMillis - cycleStartTime;
}

bool timers_isStateDone(unsigned long duration) {
  return stateElapsedTime >= duration;
}

// isLarge MUST be the keg-size value latched at cycle start (kegSizeLatched),
// NOT the live pin (isLargeKeg). Per the latching contract (KegStateMachine.h),
// flipping the selector mid-cycle must not change any stage duration. Taking
// the value as an explicit argument keeps that decision at the call site
// instead of hiding a live-global read in here.
unsigned long timers_adjustForKegSize(unsigned long baseTime, bool isLarge) {
  return isLarge ? (unsigned long)(baseTime * largeKegMod) : baseTime;
}
