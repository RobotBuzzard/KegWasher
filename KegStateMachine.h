// ======================================================================
// KegStateMachine.h - State IDs already in KegConfig.h; this is the API.
// ======================================================================
#ifndef KEG_STATE_MACHINE_H
#define KEG_STATE_MACHINE_H

#include <Arduino.h>
#include "KegConfig.h"

extern volatile byte currentState;
extern const char* stateNames[];
// Startup sub-state — promoted from file-static so MQTT status publish
// (and any other observer) can surface it. Only meaningful when
// currentState == STATE_STARTUP.
extern byte startupSubState;
// Keg size latched at cycle-start (START button press). All timer
// calculations use this so flipping the selector mid-cycle has no effect.
// isLargeKeg (KegHardware.h) still reflects the live pin state and is
// used for display and MQTT so the operator can see the current switch position.
extern bool kegSizeLatched;

void stateMachine_init();
void stateMachine_process();
void stateMachine_changeState(byte newState);
bool canTransitionTo(byte targetState);

void state_startup();
void state_draining();
void state_rinsing();
void state_washing();
void state_sanitize();
void state_pressure();
void state_finished();
void state_error();

#endif // KEG_STATE_MACHINE_H
