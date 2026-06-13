// ======================================================================
// KegStateMachine.cpp - Two-axis state machine.
//   Axis A: PackML machine state (machineState / MACH_*)
//   Axis B: ISA-88 recipe phase  (recipePhase  / PHASE_*), runs inside EXECUTE
// See docs/state-taxonomy.md (model) + docs/packml-rearchitecture-plan.md.
// ======================================================================
#include "KegStateMachine.h"
#include "KegHardware.h"
#include "KegDisplay.h"
#include "KegTimers.h"
#include "KegDiagnostics.h"

// ── State (Axis A) ─────────────────────────────────────────────────────
volatile byte machineState = MACH_IDLE;
byte          recipePhase  = PHASE_NONE;   // valid only while MACH_EXECUTE
byte          idleSub      = IDLE_INIT;    // valid only while MACH_IDLE

// PackML machine-state names — MQTT `state` topic + event log.
const char* machStateNames[NUM_MACH_STATES] = {
  "IDLE", "STARTING", "EXECUTE", "HELD",
  "COMPLETE", "STOPPING", "STOPPED", "CLEARING", "ABORTED"
};

// ISA-88 recipe-phase names — operating-screen title + MQTT `phase` topic.
// Kept short for the title bar at size-3 font.
const char* phaseNames[NUM_PHASES] = {
  "NONE",
  "DIRTY DRAIN", "DIRTY RINSE", "DIRTY PURGE",
  "WASHING", "CAUSTIC RTN",
  "RINSING", "RINSE PURGE",
  "SANITIZE", "SANI RTN",
  "PRESSURE"
};

bool kegSizeLatched = false;  // set at START press; used by all timer calcs

// Full-drain mode (the latching DRAIN switch, IO2), latched at START like keg
// size so a mid-cycle flip can't change a running phase's duration. When set,
// DIRTY_DRAIN runs fullDrainTimer (minutes — empty a spoiled/full keg) instead
// of dirtyDrainTimer (seconds — clear dregs).
bool drainModeLatched = false;

// One-time pre-check latch. The systems-go pre-check (NOT_READY screen) + caustic
// heat-up are an init ceremony, not per-cycle: once the washer reaches IDLE_READY
// we don't make the operator repeat them between kegs. Cleared on any abort so
// systems get re-verified on recovery. The between-kegs path
// (COMPLETE→START→EXECUTE) bypasses IDLE entirely, so it never sees the pre-check.
static bool washerInitialized = false;

static unsigned long pauseStartMs = 0;  // when MACH_HELD was entered

// Operator STOP/DRAIN evacuation: what's in the keg (captured when STOP is
// pressed) — decides evacuation routing. Read by machineEnter(MACH_STOPPING).
enum { EVAC_NONE = 0, EVAC_CAUSTIC, EVAC_SANI, EVAC_WATER };
static byte evacKind = EVAC_NONE;

// Recipe phase captured at ANY abort (E-stop or other fault) that interrupts a
// running/paused cycle, so recovery returns to PAUSE (HELD) at that phase rather
// than discarding the cycle. PHASE_NONE = no cycle was running → recover to READY.
static byte abortResumePhase = PHASE_NONE;

static const unsigned long AIR_BURST_DURATION = 5000UL;

// PRESSURE phase: minimum time after the return path closes before the keg
// pressure switch is trusted — lets residual SANI_RETURN backpressure bleed
// so a still-settling line can't end the charge instantly.
static const unsigned long PRESSURE_SETTLE_MS = 2000UL;

// ── Forward declarations (statics) ─────────────────────────────────────
static bool canMachTransition(byte from, byte to);
static void changeMachineState(byte newMach);
static void exitPhase(byte phase);
static bool enterPhase(byte phase);
static void advancePhase(byte nextPhase);
static void startRecipe();
static void dispatchPhase();
static byte evacKindForPhase(byte phase);
static unsigned long evacDuration();
static void state_idle();
static void state_starting();
static void state_complete();
static void state_aborted();
static bool faultConditionActive(byte code);
static void abortRecover();
static void state_clearing();
static void state_stopping();
static void state_stopped();
static void showHeatingProgress();
static const char* errorBannerText(byte code);

// Hard upper bounds on time-in-state — last-resort safety nets if a transition
// condition never triggers. 0 = no timeout (operator-paced / self-bounded).
static const unsigned long machStateMaxDuration[NUM_MACH_STATES] = {
  0,                 // IDLE      — operator-paced
  0,                 // STARTING  — heater bounded by hardware_monitorHeating
  0,                 // EXECUTE   — bounded per-phase (phaseMaxDuration)
  0,                 // HELD      — bounded by pauseStartMs / PAUSE_MAX_MS
  0,                 // COMPLETE  — operator acknowledge
  2UL * 60 * 1000,   // STOPPING  — bounded evacuation
  0,                 // STOPPED   — operator acknowledge
  0,                 // CLEARING  — instant pass-through
  0                  // ABORTED   — operator acknowledge
};

static const unsigned long phaseMaxDuration[NUM_PHASES] = {
  0,                  // NONE
  10UL * 60 * 1000,   // DIRTY_DRAIN
  10UL * 60 * 1000,   // DIRTY_RINSE
  10UL * 60 * 1000,   // DIRTY_PURGE
  20UL * 60 * 1000,   // WASHING — longest phase
  10UL * 60 * 1000,   // CAUSTIC_RETURN
  10UL * 60 * 1000,   // RINSING
  10UL * 60 * 1000,   // RINSE_PURGE
  10UL * 60 * 1000,   // SANITIZE
  10UL * 60 * 1000,   // SANI_RETURN
  10UL * 60 * 1000    // PRESSURE
};

// Single source of truth for per-phase duration. Maps each recipe phase to its
// configured timer × latched keg-size modifier. Used by the phase handlers, the
// display countdown (KegDisplay), and the MQTT remaining-time publish so they
// can never disagree.
unsigned long stageTimerFor(byte phase) {
  unsigned long base;
  switch (phase) {
    case PHASE_DIRTY_DRAIN:    base = drainModeLatched ? fullDrainTimer
                                                       : dirtyDrainTimer; break;
    case PHASE_DIRTY_RINSE:    base = dirtyRinseTimer; break;
    case PHASE_DIRTY_PURGE:    base = dirtyPurgeTimer; break;
    case PHASE_WASHING:        base = washTimer;       break;
    case PHASE_CAUSTIC_RETURN: base = causticRtnTimer; break;
    case PHASE_RINSING:        base = rinseTimer;      break;
    case PHASE_RINSE_PURGE:    base = rinsePurgeTimer; break;
    case PHASE_SANITIZE:       base = saniTimer;       break;
    case PHASE_SANI_RETURN:    base = saniRtnTimer;    break;
    case PHASE_PRESSURE:       base = purgeTimer;      break;
    default:                   return 0;
  }
  return timers_adjustForKegSize(base, kegSizeLatched);
}

// ---------- Shared UI / input helpers (de-dup) ----------
// Flashing alert banner — one shared 500 ms toggle (only one screen is active at
// a time; each re-primes on its own (re)draw).
static unsigned long smBannerLastMs  = 0;
static bool          smBannerVisible = false;

static void smBannerPrime(const char* text, word color) {
  smBannerVisible = true;
  smBannerLastMs  = millis();
  display_flashBanner(text, color, true);
}
static void smBannerTick(const char* text, word color, unsigned long periodMs = 500UL) {
  if (millis() - smBannerLastMs >= periodMs) {
    smBannerVisible = !smBannerVisible;
    display_flashBanner(text, color, smBannerVisible);
    smBannerLastMs = millis();
  }
}

// Edge detect on a (debounced) button flag.
static inline bool smRising(bool now, bool prev)  { return now && !prev; }
static inline bool smFalling(bool now, bool prev) { return !now && prev; }

// Bench-gated resource gate. Returns true when ok. On failure it sets the error
// code, shows it, aborts (→ MACH_ABORTED), and returns false. Sensor checks are
// skipped in BENCH_MODE (no plumbing wired), like the legacy WASHING check.
static bool requireResource(bool ok, byte err) {
  if (!kwBenchMode && !ok) {
    errorCode = err;
    display_showError(diagnostics_getErrorMessage(err));
    stateMachine_abort();
    return false;
  }
  return true;
}

// ---------- Init ----------
void stateMachine_init() {
  machineState = MACH_IDLE;
  recipePhase  = PHASE_NONE;
  idleSub      = IDLE_INIT;
  timers_resetStateTimer();
}

// ---------- Per-loop dispatch ----------
static void checkStateTimeout() {
  unsigned long maxDur;
  if (machineState == MACH_EXECUTE) {
    if (recipePhase >= NUM_PHASES) return;
    maxDur = phaseMaxDuration[recipePhase];
  } else {
    if (machineState >= NUM_MACH_STATES) return;
    maxDur = machStateMaxDuration[machineState];
  }
  if (maxDur == 0) return;
  if (timers_getStateElapsed() > maxDur) {
    diagnostics_logEvent("State timeout");
    errorCode = ERR_STATE_TIMEOUT;
    stateMachine_abort();
  }
}

// Per-tick safety monitor for the running recipe. Aborts if the resource a phase
// is *actively using* drops out. Runs only while EXECUTE; sensor-dependent checks
// are bench-gated. (Enclosure overtemp is no longer monitored here — the fan is a
// standalone self-regulating unit, off the controller.)
static void monitorActiveResources() {
  if (machineState != MACH_EXECUTE) return;
  if (kwBenchMode) return;   // no plumbing wired — per-phase resource gates off

  // CO2 phases (SANI_RETURN/PRESSURE) are not monitored here: the only CO2
  // sensor is the keg-side switch downstream of co2Out, whose state during an
  // open-circuit blowback is indeterminate. PRESSURE verifies CO2 end-to-end
  // in its own handler (switch must trip before the stage timer).
  bool needAir = false, needWater = false, needCaustic = false;
  switch (recipePhase) {
    case PHASE_DIRTY_DRAIN:    needAir   = (timers_getStateElapsed() < AIR_BURST_DURATION); break;
    case PHASE_DIRTY_PURGE:
    case PHASE_CAUSTIC_RETURN:
    case PHASE_RINSE_PURGE:    needAir   = true; break;
    case PHASE_DIRTY_RINSE:
    case PHASE_RINSING:        needWater = true; break;
    case PHASE_WASHING:        needCaustic = true; break;
    default: break;  // SANITIZE recirculates only — nothing pressurized to lose
  }

  byte err = ERR_NONE;
  if      (needAir   && !isAirOk)                                       err = ERR_AIR_PRESSURE;
  else if (needWater && !isWaterOk)                                     err = ERR_WATER_PRESSURE;
  else if (needCaustic && hardware_getCausticTemp() < minCausticTemp) err = ERR_CAUSTIC_TEMP;

  if (err != ERR_NONE) {
    errorCode = err;
    display_showError(diagnostics_getErrorMessage(err));
    stateMachine_abort();
  }
}

void stateMachine_process() {
  checkStateTimeout();
  monitorActiveResources();   // no-op unless EXECUTE

  switch (machineState) {
    case MACH_IDLE:     state_idle();     break;
    case MACH_STARTING: state_starting(); break;
    case MACH_EXECUTE:  dispatchPhase();  break;
    case MACH_HELD:
      // Frozen mid-recipe: outputs held safe (set at HELD entry), countdown
      // frozen. Only the pause-timeout safety bound runs here.
      if (millis() - pauseStartMs > pauseMaxMs) {
        diagnostics_logEvent("Pause timeout");
        errorCode = ERR_PAUSE_TIMEOUT;
        stateMachine_abort();
      }
      break;
    case MACH_COMPLETE: state_complete(); break;
    case MACH_STOPPING: state_stopping(); break;
    case MACH_STOPPED:  state_stopped();  break;
    case MACH_CLEARING: state_clearing(); break;
    case MACH_ABORTED:  state_aborted();  break;
    default:
      changeMachineState(MACH_IDLE);
      break;
  }
}

// ---------- Transition rules (Axis A) ----------
static bool canMachTransition(byte from, byte to) {
  if (to == MACH_ABORTED) return true;  // Abort (ESTOP/fault) from anywhere
  switch (from) {
    case MACH_IDLE:     return to == MACH_STARTING || to == MACH_EXECUTE;
    case MACH_STARTING: return to == MACH_EXECUTE;
    case MACH_EXECUTE:  return to == MACH_HELD || to == MACH_COMPLETE || to == MACH_STOPPING;
    case MACH_HELD:     return to == MACH_EXECUTE || to == MACH_STOPPING;
    case MACH_COMPLETE: return to == MACH_EXECUTE || to == MACH_IDLE;
    case MACH_STOPPING: return to == MACH_STOPPED;
    case MACH_STOPPED:  return to == MACH_IDLE;
    case MACH_CLEARING: return to == MACH_STOPPED;
    case MACH_ABORTED:  return to == MACH_CLEARING || to == MACH_IDLE;
    default:            return false;
  }
}

// ---------- Recipe-phase choreography (Axis B) ----------
// Valves asserted on phase entry / cleared on phase exit. SAFETY-CRITICAL: the
// chem-never-to-drain combos (RETURN phases force drain+pump OFF) are preserved
// exactly from the pre-split firmware. The relay wiring also enforces this.
static void exitPhase(byte phase) {
  switch (phase) {
    case PHASE_DIRTY_DRAIN:    hardware_setDrain(false); hardware_setAir(false);   break;
    case PHASE_DIRTY_RINSE:    hardware_setWater(false); hardware_setDrain(false); break;
    case PHASE_DIRTY_PURGE:    hardware_setAir(false);   hardware_setDrain(false); break;
    case PHASE_WASHING:        hardware_setCaustic(false); hardware_setPump(false); break;
    case PHASE_CAUSTIC_RETURN: hardware_setAir(false);   hardware_setCaustic(false); break;
    case PHASE_RINSING:        hardware_setWater(false); hardware_setDrain(false); break;
    case PHASE_RINSE_PURGE:    hardware_setAir(false);   hardware_setDrain(false); break;
    case PHASE_SANITIZE:       hardware_setSanitizer(false); hardware_setPump(false); break;
    case PHASE_SANI_RETURN:    hardware_setCo2(false);   hardware_setSanitizer(false); break;
    case PHASE_PRESSURE:       hardware_setCo2(false);   break;
    default: break;
  }
}

// Returns false if the phase's preconditions failed and the caller should abort
// the entry (errorCode + an abort to MACH_ABORTED will already be set).
static bool enterPhase(byte phase) {
  switch (phase) {
    case PHASE_DIRTY_DRAIN:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setDrain(true);
      // Air burst (first 5 s) happens in the DIRTY_DRAIN handler.
      return true;
    case PHASE_DIRTY_RINSE:
      if (!requireResource(isWaterOk, ERR_WATER_PRESSURE)) return false;
      hardware_setWater(true);
      hardware_setDrain(true);
      return true;
    case PHASE_DIRTY_PURGE:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setAir(true);
      hardware_setDrain(true);
      return true;
    case PHASE_WASHING:
      if (!requireResource(hardware_getCausticTemp() >= minCausticTemp,
                           ERR_CAUSTIC_TEMP)) return false;
      hardware_setCaustic(true);
      hardware_setPump(true);
      return true;
    case PHASE_CAUSTIC_RETURN:
      // Air pushes caustic back to its reservoir via the caustic valve.
      // Chem must NEVER reach the drain — drain + pump stay OFF.
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setDrain(false);
      hardware_setPump(false);
      hardware_setCaustic(true);
      hardware_setAir(true);
      return true;
    case PHASE_RINSING:
      if (!requireResource(isWaterOk, ERR_WATER_PRESSURE)) return false;
      hardware_setWater(true);
      hardware_setDrain(true);
      return true;
    case PHASE_RINSE_PURGE:
      if (!requireResource(isAirOk, ERR_AIR_PRESSURE)) return false;
      hardware_setAir(true);
      hardware_setDrain(true);
      return true;
    case PHASE_SANITIZE:
      // Sanitizer recirculates from its reservoir via the pump — no pressurized
      // resource to gate on at entry.
      hardware_setSanitizer(true);
      hardware_setPump(true);
      return true;
    case PHASE_SANI_RETURN:
      // CO2 pushes sanitizer back to its reservoir via the sanitizer valve.
      // Chem must NEVER reach the drain — drain + pump stay OFF.
      // No CO2 precondition: the only switch is downstream of co2Out (reads
      // keg/line pressure, 0 until the valve opens). Supply is assumed; a
      // missing bottle surfaces as the PRESSURE phase timing out.
      hardware_setDrain(false);
      hardware_setPump(false);
      hardware_setSanitizer(true);
      hardware_setCo2(true);
      return true;
    case PHASE_PRESSURE:
      hardware_setCo2(true);   // closed-loop: handler shuts it on switch trip
      return true;
    default:
      return true;
  }
}

// ---------- Machine-state choreography (Axis A) ----------
static void machineExit(byte s) {
  switch (s) {
    case MACH_STARTING:
      hardware_setCausticHeater(false);
      break;
    case MACH_STOPPING:
      // Turn off every evacuation output regardless of which kind ran.
      hardware_setAir(false);
      hardware_setDrain(false);
      hardware_setCaustic(false);
      hardware_setSanitizer(false);
      hardware_setCo2(false);
      hardware_setPump(false);
      break;
    case MACH_COMPLETE:
      hardware_setKegsDone(false);
      hardware_setAlarm(false);
      break;
    case MACH_ABORTED:
      hardware_setAlarm(false);
      break;
    default:
      break;
  }
}

// Returns false if entry aborted (only possible entering EXECUTE).
static bool machineEnter(byte s) {
  switch (s) {
    case MACH_IDLE:
      idleSub = IDLE_INIT;
      return true;
    case MACH_EXECUTE:
      return enterPhase(recipePhase);   // recipePhase set by the caller
    case MACH_STARTING:
      // Heating is driven by state_starting(); nothing to assert here.
      return true;
    case MACH_STOPPING:
      // Evacuate the keg to the correct destination. Heater + pump always off
      // (no dry run / no heat). Chem never reaches the drain.
      hardware_setCausticHeater(false);
      hardware_setPump(false);
      if (evacKind == EVAC_CAUSTIC) {        // air-blow caustic -> caustic reservoir
        hardware_setDrain(false);
        hardware_setCaustic(true);
        hardware_setAir(true);
      } else if (evacKind == EVAC_SANI) {    // CO2-blow sanitizer -> sani reservoir
        hardware_setDrain(false);
        hardware_setSanitizer(true);
        hardware_setCo2(true);
      } else if (evacKind == EVAC_WATER) {   // air-blow water/product -> drain
        hardware_setAir(true);
        hardware_setDrain(true);
      }
      // EVAC_NONE (e.g. STOP during PRESSURE — gas only): nothing to evacuate.
      return true;
    case MACH_STOPPED:
      hardware_allStop();          // drops every output (sets alarm HIGH)...
      hardware_setAlarm(false);    // ...but a graceful STOP is silent
      return true;
    case MACH_COMPLETE:
      // Cycle complete is a SUCCESS — do NOT light the red stacklight (IO4 =
      // alarmOut is reserved for fault / E-stop only; red should never mean "done").
      // Positive indicators: green START button breathes (loop() lamp policy) and
      // the swap-kegs signal on CCIO-A7 goes high for any external indicator.
      hardware_setKegsDone(true);
      return true;
    case MACH_ABORTED:
      hardware_setAlarm(true);
      washerInitialized = false;   // re-run pre-check after a fault
      return true;
    case MACH_CLEARING:
    default:
      return true;
  }
}

static void changeMachineState(byte newMach) {
  if (!canMachTransition(machineState, newMach)) {
    diagnostics_logEvent("Invalid machine transition");
    return;
  }
  // Leaving EXECUTE: safe-shutdown the active phase's outputs first.
  if (machineState == MACH_EXECUTE) exitPhase(recipePhase);
  machineExit(machineState);

  machineState = newMach;
  timers_resetStateTimer();
  if (newMach != MACH_EXECUTE) recipePhase = PHASE_NONE;

  if (!machineEnter(newMach)) {
    // machineEnter aborted (entering EXECUTE and a precondition failed); it has
    // already redirected to MACH_ABORTED. Nothing more to do.
    return;
  }
  diagnostics_logEvent(machStateNames[newMach]);
}

// Begin a fresh recipe at the first phase (IDLE/STARTING/COMPLETE → EXECUTE).
static void startRecipe() {
  recipePhase = PHASE_FIRST;
  changeMachineState(MACH_EXECUTE);   // machineEnter(EXECUTE) → enterPhase(PHASE_FIRST)
}

// Advance to the next recipe phase within EXECUTE (no machine-state change).
static void advancePhase(byte nextPhase) {
  exitPhase(recipePhase);
  recipePhase = nextPhase;
  timers_resetStateTimer();
  if (!enterPhase(nextPhase)) return;  // aborted to MACH_ABORTED
  diagnostics_logEvent(phaseNames[nextPhase]);
}

// ---------- Hold / Unhold (PackML Held; the PAUSE/RESUME overlay) ----------
void stateMachine_setPause(bool wantPaused) {
  if (wantPaused) {
    if (machineState != MACH_EXECUTE) return;
    pauseStartMs = millis();
    exitPhase(recipePhase);          // per-phase safe shutdown (no dry pump etc.)
    machineState = MACH_HELD;        // recipePhase preserved for resume
    diagnostics_logEvent("Cycle paused (HELD)");
  } else {
    if (machineState != MACH_HELD) return;
    // Re-read the latching DRAIN switch on resume. Unlike the keg-size selector
    // (which scales the whole cycle and stays latched), DRAIN is a deliberate
    // mode toggle affecting only DIRTY_DRAIN; an operator who flips it while
    // paused expects the resumed cycle to honor it. No-op once DIRTY_DRAIN has
    // passed (nothing else reads drainModeLatched).
    drainModeLatched = isFullDrainOn;
    machineState = MACH_EXECUTE;
    timers_shiftStateStart(millis() - pauseStartMs);  // don't count paused time
    diagnostics_logEvent("Cycle resumed");
    if (!enterPhase(recipePhase)) return;  // re-assert; may abort if a resource dropped
    display_showOperatingScreen();   // refresh subtitle/countdown for the (maybe new) mode
  }
}

void stateMachine_togglePause() {
  stateMachine_setPause(machineState != MACH_HELD);
}

// ---------- Operator STOP / RESTART ----------
static byte evacKindForPhase(byte phase) {
  switch (phase) {
    case PHASE_WASHING:
    case PHASE_CAUSTIC_RETURN: return EVAC_CAUSTIC;
    case PHASE_SANITIZE:
    case PHASE_SANI_RETURN:    return EVAC_SANI;
    case PHASE_DIRTY_DRAIN:
    case PHASE_DIRTY_RINSE:
    case PHASE_DIRTY_PURGE:
    case PHASE_RINSING:
    case PHASE_RINSE_PURGE:    return EVAC_WATER;
    default:                   return EVAC_NONE;  // PRESSURE: gas only, nothing to evacuate
  }
}

// Evacuation duration = the configured "normal drain" timer for that contents
// (keg-size scaled), not a fixed time.
static unsigned long evacDuration() {
  switch (evacKind) {
    case EVAC_CAUSTIC: return stageTimerFor(PHASE_CAUSTIC_RETURN);
    case EVAC_SANI:    return stageTimerFor(PHASE_SANI_RETURN);
    case EVAC_WATER:   return stageTimerFor(PHASE_DIRTY_DRAIN);
    default:           return 0;
  }
}

void stateMachine_stop() {
  if (machineState != MACH_EXECUTE && machineState != MACH_HELD) return;
  evacKind = evacKindForPhase(recipePhase);
  hardware_setCausticHeater(false);  // immediate: heater off
  diagnostics_logEvent("STOP/DRAIN: draining keg");
  changeMachineState(MACH_STOPPING); // machineEnter(STOPPING) asserts the evac
}

// RESTART: re-run the current phase from the top of its timer. Same phase, same
// contents — no evacuation. Unholds first if held.
void stateMachine_restart() {
  if (machineState == MACH_HELD) {
    machineState = MACH_EXECUTE;
    if (!enterPhase(recipePhase)) return;  // re-assert; may abort if a resource dropped
  } else if (machineState != MACH_EXECUTE) {
    return;
  }
  drainModeLatched = isFullDrainOn;  // re-read the latching DRAIN switch on an explicit restart
  timers_resetStateTimer();          // back to the beginning of this phase's timer
  display_showOperatingScreen();     // timer jumped to full; refresh subtitle/countdown too
  diagnostics_logEvent("Phase restarted");
}

// ---------- Abort / recovery ----------
void stateMachine_abort() {
  // If the abort interrupts a running/paused cycle, remember the phase so recovery
  // returns to PAUSE (HELD) at that phase — same landing whether the trigger was
  // E-stop or any other fault.
  if ((machineState == MACH_EXECUTE || machineState == MACH_HELD) &&
      recipePhase != PHASE_NONE) {
    abortResumePhase = recipePhase;
  }
  changeMachineState(MACH_ABORTED);  // errorCode already set by the caller
}

void stateMachine_reset() {
  switch (machineState) {
    case MACH_ABORTED:
      // Same unified recovery as the panel: refuse while the fault is still
      // active, else land at PAUSE (cycle preserved) or READY.
      if (faultConditionActive(errorCode)) {
        diagnostics_logEvent("Reset ignored (fault active)");
        break;
      }
      diagnostics_logEvent("Remote acknowledge");
      abortRecover();
      break;
    case MACH_STOPPED:
    case MACH_COMPLETE:
      changeMachineState(MACH_IDLE);
      break;
    default:
      diagnostics_logEvent("Reset ignored");
      break;
  }
}

// ---------- IDLE (ready / not-ready) ----------
static void showHeatingProgress() {
  int currentTemp = hardware_getCausticTemp();
  int targetTemp  = optimalCausticTemp;
  int pct = constrain(map(currentTemp, heatingStartTemp, targetTemp, 0, 100), 0, 100);

  if (!isHeaterActive) {
    display_showProgress("Heating", pct, currentTemp, targetTemp);
    return;
  }
  unsigned long elapsedMs = millis() - heatingStartTime;
  if (elapsedMs > 120000UL && currentTemp > heatingStartTemp) {
    int degPerMin = (int)((currentTemp - heatingStartTemp) * 60000UL / elapsedMs);
    if (degPerMin > 0) {
      int remainMin = (targetTemp - currentTemp) / degPerMin + 1;
      display_showProgress("Heating", pct, currentTemp, targetTemp, remainMin);
      return;
    }
  }
  display_showProgress("Heating", pct, currentTemp, targetTemp);
}

static void state_idle() {
  switch (idleSub) {
    case IDLE_INIT:
      errorCode = ERR_NONE;
      hardware_setAlarm(false);   // IDLE/READY/NOT_READY is never a fault — IO4 (red) off
      diagnostics_logEvent("Idle: init");
      // Pre-check runs once, at power-up. After init, returning to IDLE (parked
      // via DRAIN, or recovered from STOPPED) skips straight to READY. A fault
      // clears washerInitialized so the check re-runs after recovery.
      idleSub = washerInitialized ? IDLE_READY : IDLE_NOT_READY;
      break;

    case IDLE_NOT_READY: {
      bool waterOk = isWaterOk;
      bool airOk   = isAirOk;
      bool co2Ok   = isCo2Ok;
      bool estopOk = !isEstopActive;

      static bool prevWater = false, prevAir = false, prevCo2 = false, prevEstop = false;
      static bool prevLevel = false;
      static bool drawn = false;
      bool levelOk = isCausticLevelOk;

      bool changed = (!drawn || waterOk != prevWater || airOk != prevAir ||
                      co2Ok != prevCo2 || estopOk != prevEstop || levelOk != prevLevel);
      if (changed) {
        display_showNotReady(waterOk, airOk, co2Ok, estopOk);
        drawn = true;
        prevWater = waterOk; prevAir = airOk; prevCo2 = co2Ok; prevEstop = estopOk;
        prevLevel = levelOk;
      }
      // No flashing banner here — the amber title + signal rows convey readiness
      // (banner is reserved for action/attention states).

      if (hardware_allSystemsGo()) {
        errorCode = ERR_NONE;
        drawn = false;
        prevWater = prevAir = prevCo2 = prevEstop = true;
        washerInitialized = true;       // pre-check passed — don't repeat per cycle
        idleSub = IDLE_READY;
        diagnostics_logEvent("Systems ready");
      }
      break;
    }

    case IDLE_READY: {
      static bool drawn = false;
      static byte lastSig = 0xFF;

      // Live sensor dots: redraw when any input changes. (In BENCH_MODE
      // allSystemsGo() is forced true so the READY->NOT_READY transition
      // below never fires — without this the screen showed stale green
      // dots after a sensor dropped.)
      bool lvlOk = isCausticLevelOk;
      byte sig = (byte)((lvlOk << 4) | (isWaterOk << 3) | (isAirOk << 2) |
                        (isCo2Ok << 1) | (byte)isEstopActive);
      if (sig != lastSig) { lastSig = sig; drawn = false; }

      if (!drawn) {
        display_showReadyScreen();
        drawn = true;
      }
      // No flashing banner — green title + START button say it all.

      if (!hardware_allSystemsGo()) {   // a system went offline
        drawn = false;
        idleSub = IDLE_NOT_READY;
        diagnostics_logEvent("Systems lost");
        break;
      }

      if (isCycleStartPressed) {
        kegSizeLatched   = isLargeKeg;
        drainModeLatched = isFullDrainOn;
        diagnostics_logEvent(kegSizeLatched ? "Cycle start (LARGE)" : "Cycle start (SMALL)");
        if (drainModeLatched) diagnostics_logEvent("Full-drain mode (DRAIN latched)");
        drawn = false;
        // Washable threshold: fw mode heats to the operator target before the
        // first cycle; ext mode (probe thermostat keeps the tank hot) only
        // needs the wash floor — the thermostat keeps climbing regardless.
        if (kwBenchMode || hardware_getCausticTemp() >=
              (heaterExternal ? minCausticTemp : optimalCausticTemp)) {
          startRecipe();          // bench: no heater plumbed — skip STARTING
        } else {
          diagnostics_logEvent("Startup: heating");
          changeMachineState(MACH_STARTING);
        }
      }
      break;
    }

    case IDLE_SETTINGS:
    default:
      // Phase 5 — on-screen settings editor (not yet wired).
      break;
  }
}

// ---------- STARTING (caustic warm-up) ----------
static void state_starting() {
  static bool heatingDrawn = false;
  static unsigned long lastHeatDisplayMs = 0;

  // fw: heat to the operator target. ext: the probe thermostat owns the burn;
  // we only wait until the tank is washable (wash floor) and move on.
  if (hardware_getCausticTemp() >=
        (heaterExternal ? minCausticTemp : optimalCausticTemp)) {
    hardware_setCausticHeater(false);   // no-op in ext mode (permit persists)
    heatingDrawn = false;
    diagnostics_logEvent("Heating complete");
    startRecipe();
    return;
  }

  if (heaterExternal) {
    // Permit logic runs every tick from hardware_readInputs(); a dropped
    // permit for low level is a fault here (we're waiting on that heat).
    if (!isCausticLevelOk) {
      errorCode = ERR_CAUSTIC_LEVEL;
      display_showError(diagnostics_getErrorMessage(ERR_CAUSTIC_LEVEL));
      stateMachine_abort();
      return;
    }
  } else if (!isHeaterActive) {
    if (!isCausticLevelOk) {
      errorCode = ERR_CAUSTIC_LEVEL;
      display_showError(diagnostics_getErrorMessage(ERR_CAUSTIC_LEVEL));
      stateMachine_abort();
      return;
    }
    hardware_setCausticHeater(true);
    heatingDrawn = false;
  }

  if (!heatingDrawn || millis() - lastHeatDisplayMs >= 3000UL) {
    showHeatingProgress();
    heatingDrawn = true;
    lastHeatDisplayMs = millis();
  }
  // No banner — amber "HEATING" title + the live progress (CUR/TGT/%) convey it.
}

// ---------- EXECUTE: recipe phase handlers ----------
// Valves are asserted in enterPhase() / cleared in exitPhase(); these handlers
// just run the per-phase timer and advance. The only in-handler valve nuance is
// DIRTY_DRAIN's 5 s start air burst.
static void phase_dirtyDrain() {
  hardware_setAir(timers_getStateElapsed() < AIR_BURST_DURATION);
  if (timers_isStateDone(stageTimerFor(PHASE_DIRTY_DRAIN))) {
    hardware_setAir(false);
    advancePhase(PHASE_DIRTY_RINSE);
  }
}

static void dispatchPhase() {
  switch (recipePhase) {
    case PHASE_DIRTY_DRAIN:
      phase_dirtyDrain();
      break;
    case PHASE_DIRTY_RINSE:
      if (timers_isStateDone(stageTimerFor(PHASE_DIRTY_RINSE)))    advancePhase(PHASE_DIRTY_PURGE);
      break;
    case PHASE_DIRTY_PURGE:
      if (timers_isStateDone(stageTimerFor(PHASE_DIRTY_PURGE)))    advancePhase(PHASE_WASHING);
      break;
    case PHASE_WASHING:
      // Caustic-temp safety is handled by monitorActiveResources() before this.
      if (timers_isStateDone(stageTimerFor(PHASE_WASHING)))        advancePhase(PHASE_CAUSTIC_RETURN);
      break;
    case PHASE_CAUSTIC_RETURN:
      if (timers_isStateDone(stageTimerFor(PHASE_CAUSTIC_RETURN))) advancePhase(PHASE_RINSING);
      break;
    case PHASE_RINSING:
      if (timers_isStateDone(stageTimerFor(PHASE_RINSING)))        advancePhase(PHASE_RINSE_PURGE);
      break;
    case PHASE_RINSE_PURGE:
      if (timers_isStateDone(stageTimerFor(PHASE_RINSE_PURGE)))    advancePhase(PHASE_SANITIZE);
      break;
    case PHASE_SANITIZE:
      if (timers_isStateDone(stageTimerFor(PHASE_SANITIZE)))       advancePhase(PHASE_SANI_RETURN);
      break;
    case PHASE_SANI_RETURN:
      if (timers_isStateDone(stageTimerFor(PHASE_SANI_RETURN)))    advancePhase(PHASE_PRESSURE);
      break;
    case PHASE_PRESSURE:
      // Closed-loop charge. The regulator runs well above the keg target (it
      // also blows sanitizer back), so the keg must NOT equilibrate to it:
      // shut the solenoid the moment the downstream switch trips (= keg at
      // setpoint; exitPhase closes co2Out on the COMPLETE transition). The
      // stage timer is a fail timeout — switch never tripping means no CO2,
      // an unsealed keg, or a leak. Bench (no plumbing): timer = duration.
      if (kwBenchMode) {
        if (timers_isStateDone(stageTimerFor(PHASE_PRESSURE)))     changeMachineState(MACH_COMPLETE);
      } else if (isCo2Ok && timers_getStateElapsed() >= PRESSURE_SETTLE_MS) {
        diagnostics_logEvent("Keg at pressure");
        changeMachineState(MACH_COMPLETE);
      } else if (timers_isStateDone(stageTimerFor(PHASE_PRESSURE))) {
        errorCode = ERR_CO2_PRESSURE;
        display_showError(diagnostics_getErrorMessage(ERR_CO2_PRESSURE));
        stateMachine_abort();
      }
      break;
    default:
      break;
  }
}

// ---------- COMPLETE ----------
static void state_complete() {
  static bool drawn = false;
  static bool prevPress = false;

  if (!drawn) {
    display_showFinishedScreen();
    drawn = true;
    smBannerPrime("SWAP KEG", BLUE);
  }
  smBannerTick("SWAP KEG", BLUE);  // blue flashing = mandatory operator action (title already says COMPLETE)

  bool nowPressed = isCycleStartPressed;

  // One-shot throughput: a single START silences the alarm AND launches the next
  // cycle. Goes straight to EXECUTE@DIRTY_DRAIN — no pre-check, no re-heat.
  if (smRising(nowPressed, prevPress)) {
    kegSizeLatched   = isLargeKeg;     // re-latch in case the switches moved
    drainModeLatched = isFullDrainOn;
    diagnostics_logEvent(kegSizeLatched ? "Next cycle (LARGE)" : "Next cycle (SMALL)");
    if (drainModeLatched) diagnostics_logEvent("Full-drain mode (DRAIN latched)");
    drawn = false;
    prevPress = nowPressed;
    startRecipe();   // alarm off via machineExit(COMPLETE)
    return;
  }

  // SILENCE (touch button / MQTT cmd/silence): park at IDLE, no new cycle.
  // (Was the DRAIN press — IO2 is the latching full-drain switch now.)
  if (display_takeTouchSilence()) {
    drawn = false;
    prevPress = nowPressed;
    changeMachineState(MACH_IDLE);   // washerInitialized → READY, no pre-check
    return;
  }

  prevPress = nowPressed;
}

// ---------- ABORTED (PackML fault landing) ----------
static const char* errorBannerText(byte code) {
  switch (code) {
    case ERR_WATER_PRESSURE:  return "WATER FAULT";
    case ERR_AIR_PRESSURE:    return "AIR FAULT";
    case ERR_CO2_PRESSURE:    return "CO2 FAULT";
    case ERR_CAUSTIC_TEMP:    return "TEMP FAULT";
    case ERR_ESTOP:           return "ESTOP";
    case ERR_HEATING_TIMEOUT: return "HEAT TIMEOUT";
    case ERR_HEATING_RATE:    return "HEAT SLOW";
    case ERR_CAUSTIC_LEVEL:   return "CAUSTIC LOW";
    case ERR_STATE_TIMEOUT:   return "TIMEOUT";
    case ERR_SD_INIT:         return "SD FAIL";
    case ERR_CONFIG_FILE:     return "CONFIG FAIL";
    case ERR_HEATER_OVERTEMP: return "HEAT OVERTEMP";
    case ERR_SENSOR_FAULT:    return "SENSOR FAIL";
    case ERR_PAUSE_TIMEOUT:   return "PAUSE TIMEOUT";
    default:                  return "ERROR";
  }
}

// The recovery ACTION for the flashing banner (the title/message already say what
// the fault IS). E-stop self-clears on release; everything else clears with a
// 2 s START hold (or the touch RECOVER button) once the cause is fixed.
static const char* recoveryHintText(byte code) {
  return (code == ERR_ESTOP) ? "RELEASE E-STOP" : "HOLD START 2s";
}

// Is the underlying condition for a given fault STILL present right now? Drives
// the IO4 policy (steady = active, flash = waiting) and gates recovery (you can't
// clear a fault whose cause is still there — mirrors "can't reset E-stop while
// engaged"). Faults with no persistent condition (timeouts, pause-timeout) report
// cleared immediately.
static bool faultConditionActive(byte code) {
  switch (code) {
    case ERR_ESTOP:           return isEstopActive;
    case ERR_WATER_PRESSURE:  return !isWaterOk;
    case ERR_AIR_PRESSURE:    return !isAirOk;
    // ERR_CO2_PRESSURE is a charge-timeout fault. Its sensor reads keg-side
    // pressure, which is legitimately 0 once outputs are safe — there is no
    // standing condition to wait out, so it's operator-clearable immediately.
    case ERR_CO2_PRESSURE:    return false;
    case ERR_CAUSTIC_TEMP:    return hardware_getCausticTemp() <  minCausticTemp;
    case ERR_HEATER_OVERTEMP: return hardware_getCausticTemp() >= maxCausticTemp;
    case ERR_CAUSTIC_LEVEL:   return !isCausticLevelOk;
    case ERR_SENSOR_FAULT:    return causticTempSensorError;
    default:                  return false;  // transient — no standing condition to clear
  }
}

// Unified recovery landing (same regardless of how the fault was acknowledged):
// PAUSE (HELD) at the interrupted phase if a cycle was running, else IDLE → READY.
// Per ISO 13850 this only PERMITS restart — it does not auto-resume; the operator
// taps RESUME to continue or STOP/DRAIN to bail.
static void abortRecover() {
  errorCode = ERR_NONE;
  hardware_setAlarm(false);
  if (abortResumePhase != PHASE_NONE) {
    recipePhase = abortResumePhase;
    abortResumePhase = PHASE_NONE;
    machineState = MACH_HELD;            // direct set preserves recipePhase
    timers_resetStateTimer();
    pauseStartMs = millis();
    diagnostics_logEvent("Recovered -> PAUSE");
  } else {
    changeMachineState(MACH_IDLE);       // → pre-check → READY (logs "IDLE")
  }
}

static void state_aborted() {
  static byte lastShown = 255;
  static bool prevPress = false;
  static unsigned long pressStartMs = 0;
  static bool longFired = false;
  static unsigned long io4LastMs = 0;
  static bool io4On = true;
  static unsigned long clearedSince = 0;
  const unsigned long LONG_PRESS_MS     = 2000;
  const unsigned long IO4_FLASH_MS      = 250;
  const unsigned long CLEAR_DEBOUNCE_MS = 300;

  if (errorCode != lastShown) {            // new fault → full redraw
    display_showError(diagnostics_getErrorMessage(errorCode));
    lastShown = errorCode;
    smBannerPrime(recoveryHintText(errorCode), RED);
    clearedSince = 0;
    io4On = true;
  }
  smBannerTick(recoveryHintText(errorCode), RED);   // banner = the ACTION; title/msg = the fault

  bool active = faultConditionActive(errorCode);

  // IO4 (red stacklight): STEADY ON while the fault condition is present; FLASH
  // 250 ms once it clears and we're waiting for the operator to acknowledge.
  if (active) {
    clearedSince = 0;
    if (!io4On) { hardware_setAlarm(true); io4On = true; }
  } else {
    if (clearedSince == 0) clearedSince = millis();
    if (millis() - io4LastMs >= IO4_FLASH_MS) {
      io4On = !io4On;
      hardware_setAlarm(io4On);
      io4LastMs = millis();
    }
  }

  bool conditionCleared = !active && clearedSince != 0 &&
                          (millis() - clearedSince) >= CLEAR_DEBOUNCE_MS;

  // Recovery, gated on the condition being gone:
  //  - E-stop auto-acknowledges on release (the release IS the ack).
  //  - Any other fault: operator long-presses START (2 s) — the on-screen
  //    instruction tells them; the touch RECOVER button (Pass 2) does the same.
  bool nowPressed = isCycleStartPressed;
  if (smRising(nowPressed, prevPress)) { pressStartMs = millis(); longFired = false; }

  bool recover = false;
  if (errorCode == ERR_ESTOP) {
    recover = conditionCleared;
  } else if (conditionCleared && nowPressed && !longFired &&
             (millis() - pressStartMs) >= LONG_PRESS_MS) {
    longFired = true;
    recover = true;
  }
  prevPress = nowPressed;

  if (recover) {
    lastShown = 255; io4On = true; clearedSince = 0; longFired = false;
    diagnostics_logEvent(errorCode == ERR_ESTOP ? "E-stop released" : "Fault acknowledged");
    abortRecover();
    return;
  }
}

// ---------- CLEARING (fault-resolution gate, PackML path B) ----------
static void state_clearing() {
  // Fault acknowledged. Currently an instant pass-through to the de-energized
  // STOPPED landing (the value of path B is that explicit STOPPED stop before
  // re-arming). Could later gate on hardware_allSystemsGo() before advancing.
  diagnostics_logEvent("Cleared -> stopped");
  changeMachineState(MACH_STOPPED);
}

// ---------- STOPPING (operator STOP/DRAIN evacuation) ----------
static void state_stopping() {
  static bool drawn = false;
  unsigned long evac = evacDuration();   // 0 = nothing to evacuate (e.g. PRESSURE)

  static unsigned long lastTickMs = 0;
  if (evac > 0) {
    if (!drawn) { display_showStopping(); drawn = true; lastTickMs = 0; }
    if (millis() - lastTickMs >= 1000UL) {   // 1 Hz countdown — the operator
      lastTickMs = millis();                 // can see the evacuation moving
      unsigned long el = timers_getStateElapsed();
      display_updateStoppingTimer(el < evac ? evac - el : 0UL);
    }
    if (!timers_isStateDone(evac)) return;   // still evacuating
  }

  drawn = false;
  diagnostics_logEvent("Keg clear -> stopped");
  changeMachineState(MACH_STOPPED);          // machineEnter(STOPPED) de-energizes, silent
}

// ---------- STOPPED (de-energized halt — momentary) ----------
static void state_stopped() {
  // The "GET READY" ack screen is gone (operator request 2026-06-12): IDLE_READY
  // already requires an explicit START before anything moves, so the second
  // acknowledgment added a tap without adding safety — and its button shared
  // pixels with READY's START, where tap-bounce chained into an instant start.
  // PackML Reset is now automatic; STOPPED is a momentary de-energized landing.
  diagnostics_logEvent("Stopped -> idle");
  changeMachineState(MACH_IDLE);             // washerInitialized → READY, no pre-check
}
