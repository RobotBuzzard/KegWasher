# Keg Washer State Table (as-built FSM)

> **As-built operational reference** for the **two-axis** firmware:
> `machineState` (PackML `MACH_*`) × `recipePhase` (ISA-88 `PHASE_*`), as
> implemented in `KegStateMachine.cpp` (branch `feat/packml-rearchitecture`,
> bench-verified). The canonical *lexicon / standards mapping* is
> **`state-taxonomy.md`**; this file is the I/O- and transition-level
> cross-reference (what's energized, what times out, what goes where). Error
> code → I/O mapping is in **`io-table.md`**.
>
> The two axes are orthogonal: `recipePhase` is meaningful **only** while
> `machineState == MACH_EXECUTE`; outside EXECUTE it is forced to `PHASE_NONE`.

## Axis A — machine states (`machineState`, PackML)

| ID | `MACH_*` | Name string | PackML type | Active outputs / indicators | Hard timeout | → on completion |
|----|----------|-------------|-------------|------------------------------|--------------|------------------|
| 0 | IDLE | `IDLE` | wait | none (display only; READY/NOT-READY via `idleSub`) | none | EXECUTE (START) · STARTING (cold, non-bench) |
| 1 | STARTING | `STARTING` | acting | caustic heater | none † | EXECUTE (caustic ≥ `OPTIMAL_CAUSTIC_TEMP`) |
| 2 | EXECUTE | `EXECUTE` | dual | per-phase (Axis B below) | per-phase | COMPLETE (after PRESSURE) |
| 3 | HELD | `HELD` | wait | none (phase outputs cleared on entry; timer frozen) | `pauseMaxMs` ‡ | EXECUTE (RESUME/RESTART) |
| 4 | COMPLETE | `COMPLETE` | wait | **none** — no alarm; blue "SWAP KEG" banner on-screen | none | EXECUTE (START=next keg) · IDLE (DRAIN=park) |
| 5 | STOPPING | `STOPPING` | acting | evac outputs per `evacKind` | 2 min | STOPPED |
| 6 | STOPPED | `STOPPED` | wait | none (de-energized, **silent** — alarm forced off) | none | IDLE (START=Reset) |
| 7 | CLEARING | `CLEARING` | acting | none | none | STOPPED *(defined; see Recovery — currently unreached)* |
| 8 | ABORTED | `ABORTED` | wait | alarm / IO4 red stacklight (3-state, see Recovery) | none | HELD or IDLE via `abortRecover()` |

`NUM_MACH_STATES = 9`. Timeouts live in `machStateMaxDuration[]`; only STOPPING
is bounded (2 min). `0` = operator-paced / self-bounded.

† STARTING isn't time-bounded here — the heater is bounded by the
hardware heating monitor (rate/overtemp/level → `ERR_HEATING_*` / `ERR_CAUSTIC_LEVEL`).
‡ HELD is bounded by `pauseMaxMs` (seeded from `PAUSE_MAX_MS` = 10 min) checked
against `pauseStartMs`; expiry → `ERR_PAUSE_TIMEOUT` → ABORTED.

**Abort is universal:** `canMachTransition()` permits `→ MACH_ABORTED` from
*any* state (ESTOP ISR / fault). All other transitions are restricted per the
table above.

## Axis B — recipe phases (`recipePhase`, ISA-88; valid only in EXECUTE)

Valves are asserted in `enterPhase()` and cleared in `exitPhase()`; the per-phase
handler just runs the timer and advances. **Chem never reaches the drain** — the
two RETURN phases force `drainOut` + `pumpOut` LOW (firmware *and* relay wiring).

| ID | `PHASE_*` | Title label | Outputs ON (entry) | Cleared (exit) | Monitored resource | Timer | Hard timeout | → next |
|----|-----------|-------------|--------------------|-----------------|--------------------|-------|--------------|--------|
| 1 | DIRTY_DRAIN | `DIRTY DRAIN` | drain (+air burst ≤5 s) | drain, air | air (during burst) | `dirtyDrainTimer` | 10 min | DIRTY_RINSE |
| 2 | DIRTY_RINSE | `DIRTY RINSE` | water, drain | water, drain | water | `dirtyRinseTimer` | 10 min | DIRTY_PURGE |
| 3 | DIRTY_PURGE | `DIRTY PURGE` | air, drain | air, drain | air | `dirtyPurgeTimer` | 10 min | WASHING |
| 4 | WASHING | `WASHING` | caustic, pump | caustic, pump | caustic temp ≥ `MIN_CAUSTIC_TEMP` | `washTimer` | **20 min** | CAUSTIC_RETURN |
| 5 | CAUSTIC_RETURN | `CAUSTIC RTN` | caustic, air *(drain/pump forced OFF)* | air, caustic | air | `causticRtnTimer` | 10 min | RINSING |
| 6 | RINSING | `RINSING` | water, drain | water, drain | water | `rinseTimer` | 10 min | RINSE_PURGE |
| 7 | RINSE_PURGE | `RINSE PURGE` | air, drain | air, drain | air | `rinsePurgeTimer` | 10 min | SANITIZE |
| 8 | SANITIZE | `SANITIZE` | sanitizer, pump | sanitizer, pump | — (recirc only) | `saniTimer` | 10 min | SANI_RETURN |
| 9 | SANI_RETURN | `SANI RTN` | sanitizer, CO₂ *(drain/pump forced OFF)* | CO₂, sanitizer | CO₂ | `saniRtnTimer` | 10 min | PRESSURE |
| 10 | PRESSURE | `PRESSURE` | CO₂ (valves closed) | CO₂ | CO₂ | `purgeTimer` | 10 min | → COMPLETE |

`PHASE_FIRST = DIRTY_DRAIN (1)`, `PHASE_LAST = PRESSURE (10)`, `NUM_PHASES = 11`
(index 0 = `PHASE_NONE`). Per-phase timers are keg-size scaled
(`stageTimerFor()` → `timers_adjustForKegSize(base, kegSizeLatched)`), the single
source of truth shared by the handlers, the display countdown, and the MQTT
remaining-time publish. Timeouts are in `phaseMaxDuration[]`.

**Entry gates** (`enterPhase`, `#ifndef BENCH_MODE`): the monitored resource is
also checked *before* entry — a missing resource aborts the entry to ABORTED with
the matching `ERR_*`. **Per-tick monitors** (`monitorActiveResources`, EXECUTE
only, bench-gated): the in-use resource is re-checked every loop, plus a blanket
`enclosure ≥ MAX_ENCLOSURE_TEMP → ERR_ENCLOSURE_TEMP` across all phases. See
`state-taxonomy.md` §3c/§3d for the full gate/monitor matrix.

## IDLE sub-states (`idleSub`, valid only while `machineState == MACH_IDLE`)

| ID | `IDLE_*` | Description | Exit |
|----|----------|-------------|------|
| 0 | INIT | one-shot: clears `errorCode`, branches on the init latch | → READY if `washerInitialized`, else → NOT_READY |
| 1 | NOT_READY | unified readiness screen; shows which of water/air/CO₂/estop are offline (amber title, no START) | `hardware_allSystemsGo()` → READY (sets `washerInitialized`) |
| 2 | READY | same screen, all-OK: green title + START button | START → EXECUTE (bench / hot) or STARTING (cold, non-bench); a system dropping out → NOT_READY |
| 3 | SETTINGS | on-screen settings editor | *not yet wired — Phase 5* |

The systems-go pre-check + heat-up are a **one-time init ceremony**, not per-keg:
once `washerInitialized` is set, returning to IDLE (parked via DRAIN, recovered
from STOPPED) goes straight to READY. Any abort clears the latch
(`machineEnter(ABORTED)`), so the check re-runs on recovery. The common
between-kegs path (COMPLETE→START→EXECUTE) bypasses IDLE entirely.

## HELD overlay (PackML `Hold`/`Unhold` — the PAUSE/RESUME control)

HELD is a first-class machine state (not a flag). PAUSE (`stateMachine_setPause(true)`)
is valid only from EXECUTE: it records `pauseStartMs`, runs the current phase's
`exitPhase()` safe-shutdown, and sets HELD with `recipePhase` **preserved**.
RESUME re-enters EXECUTE, shifts the state-start so paused time isn't counted, and
re-asserts the phase (`enterPhase`, which may itself abort if a resource dropped
while held). RESTART re-runs the current phase from the top of its timer (unholds
first if held; no evacuation). HELD is bounded by `pauseMaxMs` →
`ERR_PAUSE_TIMEOUT` → ABORTED.

## Transition rules (`canMachTransition`)

```
IDLE      → STARTING | EXECUTE
STARTING  → EXECUTE
EXECUTE   → HELD | COMPLETE | STOPPING
HELD      → EXECUTE | STOPPING
COMPLETE  → EXECUTE | IDLE
STOPPING  → STOPPED
STOPPED   → IDLE
CLEARING  → STOPPED
ABORTED   → CLEARING | IDLE
(any)     → ABORTED          # Abort: ESTOP ISR / any fault, from every state
```

Recipe-phase advance inside EXECUTE is a separate linear chain (Axis B table
above): DIRTY_DRAIN → … → PRESSURE → (machine) COMPLETE. Advancing the phase is
*not* a machine-state change — `machineState` stays EXECUTE throughout.

## Recovery (abort → resume)

All faults (and ESTOP) land in **ABORTED** with `errorCode` set. The handler
`state_aborted()` and the MQTT `cmd/reset` path share one **unified recovery**
(`abortRecover()`):

- **Landing:** if a cycle was running/held when the fault hit (`abortResumePhase`
  captured the phase), recovery lands at **HELD** at that phase — the cycle is
  preserved, and per ISO 13850 the operator must then tap RESUME to continue (no
  auto-resume) or STOP/DRAIN to bail. If nothing was running, recovery goes to
  **IDLE → READY**.
- **Gate:** recovery is refused while the fault's underlying condition is still
  present (`faultConditionActive()`) — you can't clear a fault whose cause is
  still there (mirrors "can't reset E-stop while engaged"). Transient faults
  (timeouts, pause-timeout) report cleared immediately.
- **Acknowledge:**
  - **E-stop** auto-acknowledges on release (the release *is* the ack).
  - **Any other fault** clears with a 2 s START long-press (or the on-screen
    blue RECOVER button), allowed only once the condition is gone.
- **IO4 red stacklight (3-state):** OFF normal · STEADY while the fault condition
  is active · FLASH 250 ms once it clears and is waiting for acknowledgment.

> **As-built note on `CLEARING`:** the PackML "path B" (ABORTED → CLEARING →
> STOPPED → Reset → IDLE) is *defined* — `state_clearing()` exists and the
> transitions `ABORTED→CLEARING→STOPPED` are permitted — but **no code path
> currently enters CLEARING**. `abortRecover()` sets HELD directly (a raw
> assignment, bypassing `canMachTransition`) or calls `changeMachineState(IDLE)`.
> CLEARING/STOPPED-as-recovery is reserved for a future "verify cleared before
> re-arming" gate; today it's dead code. Documented here so the table and the
> behavior don't silently disagree.

## Error codes

Codes 0–16 (`KegConfig.h`); full I/O mapping in `io-table.md`, PackML view in
`state-taxonomy.md` §5. Banner text per code is `errorBannerText()`
(e.g. `ESTOP`, `AIR FAULT`, `PAUSE TIMEOUT`); the recovery-action banner is
`recoveryHintText()` (`RELEASE E-STOP` for estop, else `HOLD START 2s`).

## State flow

```
                          ┌───── ABORTED (alarm) ◄── ESTOP / any fault, from ANY state
                          │          │
          abortRecover()  │          │ abortRecover()
       (cycle running) ───┘          └──► IDLE ──► READY     (nothing was running)
              │                              ▲
              ▼                              │ START (Reset)
   IDLE ─START─► [STARTING] ─► EXECUTE ──────┴──────────────────────┐
   (READY)        (cold)         │  ▲                               │
     ▲                    HELD ──┘  │ RESUME/RESTART                │ STOP/DRAIN
     │ DRAIN (park)     (PAUSE)     │                               ▼
     │                              │                          STOPPING
     │   ┌── recipe (Axis B) ───────┘                              │ evac done
     │   │  DIRTY_DRAIN→DIRTY_RINSE→DIRTY_PURGE→WASHING→           ▼
     │   │  CAUSTIC_RTN→RINSING→RINSE_PURGE→SANITIZE→          STOPPED ─START─► IDLE
     │   │  SANI_RTN→PRESSURE                                  (silent)
     │   └────────────────────────► COMPLETE ──START──► EXECUTE (next keg)
     └────────────────────────────────  │
                                         └─ DRAIN ─► IDLE (park)
```
