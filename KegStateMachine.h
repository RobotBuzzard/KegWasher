// ======================================================================
// KegStateMachine.h - State IDs already in KegConfig.h; this is the API.
// ======================================================================
#ifndef KEG_STATE_MACHINE_H
#define KEG_STATE_MACHINE_H

#include <Arduino.h>
#include "KegConfig.h"

extern volatile byte currentState;
extern const char* stateNames[];
// True while an operating cycle is paused (outputs safely held per-stage, the
// stage countdown frozen). Read by the display + MQTT status; set via
// stateMachine_setPause(). Always false outside the operating states.
extern bool cyclePaused;
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

// Pause/resume an in-progress cycle. Only meaningful in the operating states;
// a no-op elsewhere. Pausing runs the current stage's safe-shutdown
// (exitState) so nothing runs dry / overpressurizes; resuming re-asserts the
// stage outputs (enterState) and continues the frozen countdown.
void stateMachine_setPause(bool wantPaused);
void stateMachine_togglePause();

// Operator STOP/DRAIN (graceful — distinct from the ESTOP fault path): evacuate
// the keg's current contents to the correct destination (caustic->caustic
// reservoir, sanitizer->sani reservoir, water->drain) using the normal drain
// timers, then de-energize and halt (HALTED). No-op outside operating states.
void stateMachine_stop();

// RESTART: re-run the CURRENT stage from the start of its timer (same stage,
// same keg contents — no evacuation). Resumes first if paused. No-op outside
// operating states.
void stateMachine_restart();

// Adjusted (keg-size-scaled) duration for an operating state. Single source of
// truth for "how long is this stage" — used by the state handlers, the display
// countdown, and the MQTT remaining-time publish. Returns 0 for non-operating
// states. Uses the LATCHED keg size (kegSizeLatched).
unsigned long stageTimerFor(byte state);

void state_startup();
void state_dirtyDrain();
void state_dirtyRinse();
void state_dirtyPurge();
void state_washing();
void state_causticReturn();
void state_rinsing();
void state_rinsePurge();
void state_sanitize();
void state_saniReturn();
void state_pressure();
void state_finished();
void state_error();
void state_stopping();
void state_halted();

#endif // KEG_STATE_MACHINE_H
