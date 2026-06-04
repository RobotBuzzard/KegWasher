// ======================================================================
// KegStateMachine.h - Two-axis state machine API.
// State/phase IDs live in KegConfig.h. The machine runs on two orthogonal
// axes (PackML machine state × ISA-88 recipe phase) — see
// docs/state-taxonomy.md and docs/packml-rearchitecture-plan.md.
// ======================================================================
#ifndef KEG_STATE_MACHINE_H
#define KEG_STATE_MACHINE_H

#include <Arduino.h>
#include "KegConfig.h"

// ── Axis A: PackML machine state (MACH_*). volatile — the ESTOP path reads it.
extern volatile byte machineState;
extern const char* machStateNames[];   // indexed 0..NUM_MACH_STATES-1

// ── Axis B: ISA-88 recipe phase (PHASE_*). Only meaningful while EXECUTE.
extern byte recipePhase;
extern const char* phaseNames[];        // indexed 0..NUM_PHASES-1

// IDLE sub-state (IDLE_*). Only meaningful while machineState == MACH_IDLE.
extern byte idleSub;

// Keg size latched at cycle-start (START press). All timer calculations use
// this so flipping the selector mid-cycle has no effect. isLargeKeg
// (KegHardware.h) still reflects the live pin state for display/MQTT.
extern bool kegSizeLatched;

// ── Lifecycle ──────────────────────────────────────────────────────────
void stateMachine_init();
void stateMachine_process();

// ── Operator control commands (edges) ──────────────────────────────────
// Hold/Unhold an in-progress recipe. Only meaningful in MACH_EXECUTE/MACH_HELD;
// a no-op elsewhere. Holding runs the current phase's safe-shutdown (exitPhase)
// so nothing runs dry / overpressurizes; unholding re-asserts the phase outputs
// (enterPhase) and continues the frozen countdown.
void stateMachine_setPause(bool wantPaused);
void stateMachine_togglePause();

// Operator STOP/DRAIN (graceful — distinct from the ESTOP fault path): evacuate
// the keg's current contents to the correct destination (caustic→caustic
// reservoir, sanitizer→sani reservoir, water→drain) using the normal drain
// timers, then de-energize and halt (MACH_STOPPED). No-op outside EXECUTE/HELD.
void stateMachine_stop();

// RESTART: re-run the CURRENT recipe phase from the start of its timer (same
// phase, same keg contents — no evacuation). Unholds first if held. No-op
// outside EXECUTE/HELD.
void stateMachine_restart();

// Abort to MACH_ABORTED using the current global errorCode (ESTOP / external
// fault / startup systems-not-go). Callable from any machine state.
void stateMachine_abort();

// Recovery (operator Clear/Reset surface): MACH_ABORTED → MACH_CLEARING (begin
// PackML path B), MACH_STOPPED/MACH_COMPLETE → MACH_IDLE. No-op elsewhere.
void stateMachine_reset();

// Adjusted (keg-size-scaled) duration for a recipe phase. Single source of
// truth for "how long is this phase" — used by the phase handlers, the display
// countdown, and the MQTT remaining-time publish. Returns 0 for PHASE_NONE /
// non-recipe phases. Uses the LATCHED keg size (kegSizeLatched).
unsigned long stageTimerFor(byte phase);

#endif // KEG_STATE_MACHINE_H
