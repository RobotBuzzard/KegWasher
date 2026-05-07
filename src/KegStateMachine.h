// ======================================================================
// KegStateMachine.h - State IDs already in KegConfig.h; this is the API.
// ======================================================================
#ifndef KEG_STATE_MACHINE_H
#define KEG_STATE_MACHINE_H

#include <Arduino.h>
#include "KegConfig.h"

extern volatile byte currentState;
extern const char* stateNames[];

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
