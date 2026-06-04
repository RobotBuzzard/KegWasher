# PackML/ISA‑88 Re-architecture — Impact Analysis & Plan

> **Status:** proposal awaiting approval. **No firmware changes until signed off.**
> Goal: split the conflated `currentState` enum into the two orthogonal axes the
> standard defines (PackML machine state + ISA‑88 recipe phase), per
> `state-taxonomy.md`. This is the structural change deferred from the doc-only
> re-base.

---

## 1. What changes, conceptually

Today one variable means two things:

```
volatile byte currentState;   // is BOTH "am I running/held/stopped" AND "which wash phase"
```

Target — two axes:

```
volatile byte machineState;   // PackML: IDLE/STARTING/EXECUTE/HELD/STOPPED/ABORTED/...
byte          recipePhase;    // ISA-88: DIRTY_DRAIN..PRESSURE  (valid only when EXECUTE)
```

The 10 wash phases stop being top-level states and become the recipe that runs
*inside* `EXECUTE`. The lifecycle wrapper (Idle → Starting → Execute → Complete,
plus Held/Stopped/Aborted branches) becomes explicit.

---

## 2. Proposed target model

### Axis A — `MACH_*` (subset of PackML's 17, justified)

| `MACH_*` | Type | From (today) | Implement vs collapse |
|----------|------|--------------|------------------------|
| IDLE | wait | STARTUP_READY / NOT_READY | **implement** |
| STARTING | acting | STARTUP_HEATING + pre-exec prep | **implement** |
| EXECUTE | dual | the 10 running stages | **implement** (+ `recipePhase`) |
| HOLDING → HELD | acting→wait | pause | **implement** (HELD is the pause) |
| UNHOLDING | acting | resume | collapse → instant (or implement) |
| COMPLETING → COMPLETE | acting→wait | FINISHED | COMPLETE implement; COMPLETING collapse |
| STOPPING → STOPPED | acting→wait | STATE_STOPPING / HALTED | **implement** (STOPPING does evac) |
| ABORTING → ABORTED | acting→wait | ESTOP/fault → ERROR | ABORTED implement; ABORTING = ISR kill |
| CLEARING | acting | ack/silence | collapse → instant (or implement as fault-gate) |
| RESETTING | acting | recovery to IDLE | collapse → instant |
| SUSPENDING/SUSPENDED/UNSUSPENDING | — | *(none today)* | **defer** (future: suspend-on-sensor-blip instead of abort) |

🔸 **Sub-decision A:** implement the trivially-instant acting states
(UNHOLDING/COMPLETING/CLEARING/RESETTING) as real pass-through states for
PackML-faithfulness + clean SCADA reporting, or collapse them into immediate
transitions? (Recommend: collapse for now, leave hooks. CLEARING is the one worth
implementing — it's the explicit "fault must be resolved before reset" gate.)

🔸 **Sub-decision B:** after the re-base, should ABORTED (ERROR) recovery route
`Clear → Stopped → Reset → Idle` (PackML-correct) or keep today's direct
`ERROR → STARTUP`? (Recommend: PackML path; it's safer — forces an explicit
de-energized Stopped landing before re-arming.)

### Axis B — `PHASE_*` (ISA‑88, valid only in EXECUTE)

`PHASE_NONE` + the 10: DIRTY_DRAIN, DIRTY_RINSE, DIRTY_PURGE, WASHING,
CAUSTIC_RETURN, RINSING, RINSE_PURGE, SANITIZE, SANI_RETURN, PRESSURE. Phase
class (DRAIN/FLOW/PURGE/RECIRC/RETURN/CHARGE) drives valve template + monitor as
documented.

### Commands (clean split)

`Start` (Idle→Starting→Execute), `Reset` (Complete/Stopped→Idle),
`Hold/Unhold`, `Stop`, `Abort`, `Clear`. The physical START button stays
overloaded (context-resolves to Start or Reset by `machineState`); **MQTT already
has distinct `cmd/start` and `cmd/reset`** so that surface barely moves.

---

## 3. File-by-file impact (grounded in the consumer grep)

**39 `currentState` refs across 5 files; ~25 edit sites.**

### `KegConfig.h` / `KegStateMachine.h` — data model
- Add `MACH_*` + `PHASE_*` enums; add `machineState` + `recipePhase` externs.
- Keep `STATE_*` as **deprecated compat aliases** during migration (removed in the final step).
- `STATE_OP_FIRST..LAST` range check → replaced by `machineState == MACH_EXECUTE`.

### `KegStateMachine.cpp` — the bulk (18 refs, ~877 lines)
- `stateNames[]` → two tables: `machStateNames[]` + `phaseNames[]`.
- `stateMaxDuration[]` → per-phase timeouts (in EXECUTE) + per-machine-state timeouts.
- `canTransitionTo()` → two-level: PackML machine transitions + phase advancement.
- `enterState()/exitState()` → **split**: machine-state enter/exit (alarm, allStop, evac) vs **phase enter/exit (the valve choreography)**. ⚠️ *Safety-critical: the chem-never-to-drain valve combos must be re-keyed PHASE-for-STATE with zero behavior change.*
- dispatch `switch` → `machineState` switch; inside EXECUTE, `recipePhase` switch.
- `monitorActiveResources()` / `stageTimerFor()` → keyed on `recipePhase`.
- pause(→HELD) / stop(→STOPPING) / restart(re-run phase) → operate on EXECUTE+phase.
- `washerInitialized` / one-shot throughput (Complete→Start→Execute@DIRTY_DRAIN, no pre-check) preserved.

### `KegWasher.ino` (11 refs)
- MQTT: publish **two** topics — `state` (machineState) + new `phase` (recipePhase). Change-detect both. `startupSubName` folds into Starting/Idle.
- Operating-range check (`:743`) → `machineState == MACH_EXECUTE`.
- ESTOP guard (`:307,732`) → compare `MACH_ABORTED`.
- `stageTimerFor(currentState)` (`:531`) → `stageTimerFor(recipePhase)`.
- cmd flags: wire `cmd/start`→Start, `cmd/reset`→Reset cleanly (already separate).

### `KegDisplay.cpp` (6 refs)
- Operating frame title (`:122`) → `phaseNames[recipePhase]`.
- startScreen/operating detection (`:158-161`) → `machineState` (Complete/Stopped/Idle = start screens; Execute = operating).
- Touch dispatch (`:166-181`) → Execute + HELD.

### `KegDiagnostics.cpp` (2 refs)
- Diag-entry guard (`:29`) → `MACH_IDLE || MACH_COMPLETE`.
- Event log (`:133`) → log both axes.

### Unaffected
- `KegHardware.*` (no `currentState` refs), `KegTimers.*`, `KegDisplaySerial.*`,
  `KegUtils.*`, `KegConfig.cpp` SD loader. ESTOP ISR body unchanged (still bare-metal kill).

---

## 4. Risks & mitigations

| Risk | Mitigation |
|------|------------|
| **Valve-choreography regression** (chem→drain) when re-keying enter/exit | Re-key 1:1, no logic change; bench-verify every RETURN phase keeps drain+pump OFF; keep the relay interlock as backstop |
| Pause/resume safe-shutdown breaks across the split | HELD must call phase-exit; UNHOLD must call phase-enter — same calls as today, just relocated |
| One-shot throughput (no pre-check between kegs) lost | Explicit test: Complete→Start→Execute@DIRTY_DRAIN with no STARTING/pre-check |
| 39 consumers must all move at once (no shim, per decision) | Migrate core + all ~25 consumer sites in one coherent change; lean on the compiler to surface every miss; bench-test the whole behavior set before commit |
| BENCH_MODE gating drifts | `grep BENCH_MODE` audit per phase (existing rule) |
| Scope creep into Suspended/full-17 | Explicitly deferred above |

---

## 5. Migration (NO shim — one coherent change, then incremental bench-test)

Decision: no `legacyState()` bridge. The model split, the core FSM, and all ~25
consumer sites change together so the sketch compiles in one consistent state.
The work is still *sequenced internally*, but the first compiling checkpoint is
after the whole split. Order of editing:

1. **Model** — `KegConfig.h`/`KegStateMachine.h`: `MACH_*` + `PHASE_*` enums;
   `machineState` + `recipePhase`; drop `currentState` + `STATE_*` (no aliases).
2. **Core FSM** — `KegStateMachine.cpp`: split `enterState/exitState` into
   machine-state vs **phase choreography**; two-level `canTransitionTo`; dispatch
   `switch(machineState)` → inner `switch(recipePhase)`; re-key
   `monitorActiveResources`/`stageTimerFor` to `recipePhase`; clean Start/Reset;
   recovery path **B** (`Aborted→Clearing→Stopped→Reset→Idle`); implement CLEARING,
   collapse UNHOLDING/COMPLETING/RESETTING (sub-dec. A recommendation).
3. **Consumers** — `KegWasher.ino` (MQTT `state`+new `phase` topic, op-range →
   `MACH_EXECUTE`, ESTOP guard, `stageTimerFor(recipePhase)`), `KegDisplay.cpp`
   (title → phase name, screen/touch dispatch → `machineState`/HELD),
   `KegDiagnostics.cpp` (entry guard → IDLE/COMPLETE, log both axes).

**Then compile, then bench-test the behavior set incrementally** (camera snapshot
+ `mosquitto_sub` per the project rule), checking each independently so a failure
points at one behavior:
- full normal cycle (DIRTY_DRAIN → … → PRESSURE → Complete)
- **each RETURN phase: confirm drain LOW + pump LOW** (chem-never-to-drain)
- STOP → Stopping (evac) → Stopped; START → Idle
- ESTOP → Aborted → Clear → Stopped (recovery path B)
- Hold/Held/Unhold (pause), and RESTART (re-run current phase)
- one-shot next keg: Complete → Start → Execute@DIRTY_DRAIN, no pre-check

Estimated ~1–2 focused days. Final `grep` audits: `currentState`/`STATE_` (should
be zero) and `BENCH_MODE` (only `#ifdef`/`#ifndef`).

---

## 6. Decisions (resolved)

1. **Sub-decision A** — collapse UNHOLDING/COMPLETING/RESETTING to instant
   transitions; **implement CLEARING** as the fault-resolution gate. *(recommendation; confirm)*
2. **Sub-decision B** — ✅ **DECIDED: PackML recovery path** `Aborted→Clearing→Stopped→Reset→Idle`.
3. **No shim** — ✅ **DECIDED:** no `legacyState()`; clean one-shot migration (see §5).
4. **Branch** — ✅ **DECIDED:** merge `feat/10-stage-cycle-and-controls` → main first, then a fresh branch.
5. **Timing** — ✅ **DECIDED:** re-architect *before* the prototype/chassis move.
