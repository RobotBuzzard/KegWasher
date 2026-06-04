# Keg Washer State Table (as-built FSM)

> This is the **operational reference** for the firmware's flat 15-value
> `currentState` enum exactly as it runs (`KegConfig.h`, `KegStateMachine.cpp`).
> For the standards model — PackML machine states vs ISA‑88 recipe phases vs
> modes, and how each state below classifies — see **`state-taxonomy.md`**.

## Main state map

| ID | `STATE_*` | PackML | Active outputs | Timer | Hard timeout | → on completion |
|----|-----------|--------|----------------|-------|--------------|------------------|
| 0 | STARTUP | Idle/Starting | heater (HEATING substate) | — | none | DIRTY_DRAIN |
| 1 | DIRTY_DRAIN | Execute | drain (+air 5 s) | `dirtyDrainTimer` | 10 min | DIRTY_RINSE |
| 2 | DIRTY_RINSE | Execute | water+drain | `dirtyRinseTimer` | 10 min | DIRTY_PURGE |
| 3 | DIRTY_PURGE | Execute | air+drain | `dirtyPurgeTimer` | 10 min | WASHING |
| 4 | WASHING | Execute | caustic+pump | `washTimer` | 20 min | CAUSTIC_RETURN |
| 5 | CAUSTIC_RETURN | Execute | caustic+air *(drain/pump OFF)* | `causticRtnTimer` | 10 min | RINSING |
| 6 | RINSING | Execute | water+drain | `rinseTimer` | 10 min | RINSE_PURGE |
| 7 | RINSE_PURGE | Execute | air+drain | `rinsePurgeTimer` | 10 min | SANITIZE |
| 8 | SANITIZE | Execute | sanitizer+pump | `saniTimer` | 10 min | SANI_RETURN |
| 9 | SANI_RETURN | Execute | sanitizer+CO₂ *(drain/pump OFF)* | `saniRtnTimer` | 10 min | PRESSURE |
| 10 | PRESSURE | Execute | CO₂ | `purgeTimer` | 10 min | FINISHED |
| 11 | FINISHED | Complete | alarm | — | none | DIRTY_DRAIN (START) / STARTUP (DRAIN) |
| 12 | ERROR | Aborted | alarm | — | none | STARTUP (long-press START) |
| 13 | STOPPING | Stopping | evac per `evacKind` | `evacDuration()` | 2 min | HALTED |
| 14 | HALTED | Stopped | none (silent) | — | none | STARTUP (START) |

`NUM_STATES = 15`. Operating range (RUNNING / PackML `Execute`) =
`STATE_OP_FIRST` (DIRTY_DRAIN=1) … `STATE_OP_LAST` (PRESSURE=10).

**Overlay mode (not a state):** `cyclePaused` (PackML `Held`) freezes any RUNNING
state. PAUSE = `Hold`, RESUME = `Unhold`. Pause has a `PAUSE_MAX_MS` (10 min)
bound → `ERR_PAUSE_TIMEOUT` → ERROR.

## STARTUP sub-states (`startupSubState`)

| ID | `STARTUP_*` | Description | Exit |
|----|-------------|-------------|------|
| 0 | INIT | clear error, branch on init latch | → READY if `washerInitialized`, else → NOT_READY |
| 1 | HEATING | heat caustic to `OPTIMAL_CAUSTIC_TEMP` | temp reached → DIRTY_DRAIN *(non-bench, only if cold at START)* |
| 2 | SETTINGS | on-screen settings editor | *not yet wired — Phase 5* (repurposed dead IO_CHECK slot) |
| 3 | READY | wait for START | START → DIRTY_DRAIN (or HEATING if cold) |
| 4 | NOT_READY | shows which systems are offline | all-systems-go → READY |

The systems-go pre-check + heat-up run **once** at power-up. Once `washerInitialized`
is set, returning to STARTUP (parking via DRAIN, recovering from HALTED) goes
straight to READY — no repeat pre-check. Any fault clears the latch so the check
re-runs on recovery. The common between-kegs path (FINISHED→START→DIRTY_DRAIN)
bypasses STARTUP entirely.

## Per-state entry / exit / monitors

See `state-taxonomy.md` §3c (entry gates, exit choreography) and §3d (per-phase
active-resource safety monitors). Summary of unconditional safety (works even in
BENCH_MODE): ESTOP (ISR + main-loop), hard state timeouts, watchdog. Sensor-gated
entry preconditions and per-tick resource monitors are `#ifndef BENCH_MODE`.

## Transition rules (`canTransitionTo`)

- Linear cycle: STARTUP → DIRTY_DRAIN → DIRTY_RINSE → DIRTY_PURGE → WASHING →
  CAUSTIC_RETURN → RINSING → RINSE_PURGE → SANITIZE → SANI_RETURN → PRESSURE →
  FINISHED.
- Any RUNNING state → ERROR (fault/ESTOP) or → STOPPING (operator STOP).
- FINISHED → DIRTY_DRAIN (one-shot next keg) | STARTUP (park) | ERROR.
- ERROR → STARTUP. STOPPING → HALTED | ERROR. HALTED → STARTUP | ERROR.

## State flow

```
                 ┌──────── ERROR (Aborted) ◄── ESTOP/fault from any state
                 │              ▲
                 ▼              │
   STARTUP ──► DIRTY_DRAIN ─► DIRTY_RINSE ─► DIRTY_PURGE ─► WASHING ─► CAUSTIC_RETURN
   (Idle)         │                                                          │
     ▲            │  (operator STOP from any RUNNING state)                  ▼
     │            └──────────────► STOPPING ─► HALTED          RINSING ◄─────┘
     │                             (Stopping)  (Stopped)          │
     │                                 │  START                   ▼
     │                                 └──► STARTUP          RINSE_PURGE ─► SANITIZE
     │                                                                          │
     │                                                                          ▼
     │                                                       PRESSURE ◄── SANI_RETURN
     │                                                          │
     │   START = next keg (one-shot)                            ▼
     └────────────────────────────────────────────────────  FINISHED (Complete)
```

(Overlay: PAUSE/RESUME = `Held` on any RUNNING state; RESTART = re-run current
phase from the top of its timer, no evacuation.)

## Error codes

Full list with I/O mapping in `io-table.md`. Codes 0–16; all faults land in
ERROR (PackML `Aborted`) with the `errorCode` set. Recovery: silence
(`manualDrain` / short START) then long-press START to reset to STARTUP.
