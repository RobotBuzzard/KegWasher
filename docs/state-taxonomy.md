# KegWasher State-Machine Taxonomy & Lexicon

> **Canonical reference**, grounded in **PackML (ANSI/ISA‑TR88.00.02)** and
> **ISA‑88 / IEC 61512** batch-control terminology. `state-table.md`,
> `io-table.md`, and the Phase 6 operator manual derive from this document.
>
> **Status:** the firmware now **implements** this two-axis model — `machineState`
> (MACH_*) + `recipePhase` (PHASE_*) — as of commit `4222034` on branch
> `feat/packml-rearchitecture` (bench-verified, not yet merged to `main`). The
> earlier "flat `currentState`" framing is historical. See
> `packml-rearchitecture-plan.md` for the migration. Some sections below still
> describe the pre-split firmware as the "before" for context; the **Two axes**
> and **PackML mapping** sections are the current reality.

---

## 0. Why PackML/ISA‑88

A keg washer is a **batch process on a single unit** — precisely what ISA‑88
models. PackML adds a **standard machine-state model** with a clean separation
the project needs: *machine control state* (am I running / held / stopped /
aborted?) is a **different axis** from *recipe step* (am I in caustic wash or
sanitize?). Adopting it gives us:

- citable, audited vocabulary (useful if this ever talks to a PLC/SCADA/MES);
- a ready-made answer to "states vs modes" — PackML defines both, orthogonally;
- a target architecture that stops `currentState` from meaning two things at once.

---

## 1. The two axes (the core idea)

| Axis | Standard | Answers | KegWasher today |
|------|----------|---------|-----------------|
| **A. Machine state** | PackML state model | running / held / stopped / aborted / idle | mixed into `currentState` |
| **B. Recipe step** | ISA‑88 procedural model | which wash phase | mixed into `currentState` |
| **C. Mode** (orthogonal) | PackML modes | production / manual / maintenance | partial (`BENCH_MODE`, diag) |

The firmware currently flattens **A** and **B** into one enum. That's the single
biggest finding of the re-base and the thing the future re-architecture fixes.

---

## 2. Axis A — PackML machine-state model (verified)

**17 states, 3 types** (ISA‑TR88.00.02):

- **Wait** (resting; exit only on a command): `Idle` `Held` `Suspended` `Complete` `Stopped` `Aborted`
- **Acting** (transient; exit on internal *State Complete*, “SC”): `Starting` `Completing` `Resetting` `Holding` `Unholding` `Suspending` `Unsuspending` `Stopping` `Aborting` `Clearing`
- **Dual**: `Execute` (the productive state — the recipe of Axis B runs *inside* it)

**Commands:** `Start` `Hold` `Unhold` `Suspend` `Unsuspend` `Stop` `Abort` `Clear` `Reset` (+ internal `SC`).

**Canonical transitions:**

```
Idle --Start--> Starting --SC--> Execute --SC--> Completing --SC--> Complete --Reset--> Resetting --SC--> Idle
Execute --Hold--> Holding --SC--> Held --Unhold--> Unholding --SC--> Execute
Execute --Suspend--> Suspending --SC--> Suspended --Unsuspend--> Unsuspending --SC--> Execute
(many) --Stop--> Stopping --SC--> Stopped --Reset--> Resetting
(any)  --Abort--> Aborting --SC--> Aborted --Clear--> Clearing --SC--> Stopped
```

### 2a. KegWasher → PackML state mapping

| Firmware (`STATE_*` / substate / flag) | PackML state | Type | Notes |
|---|---|---|---|
| `STARTUP_READY` | **Idle** | wait | ready, waiting for `Start` |
| `STARTUP_NOT_READY` | **Idle** (not-ready) | wait | PackML has no separate "not ready"; it's `Idle` with the `Start` command gated by the systems-go precondition |
| `STARTUP_HEATING` | **Starting** | acting | machine warming caustic before `Execute` |
| `DIRTY_DRAIN … PRESSURE` (10) | **Execute** | dual | the ISA‑88 recipe (Axis B) runs here |
| `FINISHED` | **Complete** | wait | recover with `Reset` |
| `STATE_STOPPING` | **Stopping** | acting | graceful teardown (does the evac) |
| `STATE_HALTED` | **Stopped** | wait | recover with `Reset` |
| `STATE_ERROR` | **Aborted** | wait | fault/ESTOP landing; recover with `Clear`+`Reset` |
| `cyclePaused` (mode flag) | **Held** | wait | PAUSE=`Hold`, RESUME=`Unhold`. *PackML models this as a state; firmware keeps it a flag for now — reconciled in the re-architecture.* |

### 2b. Command mapping

| Operator action (KegWasher) | PackML command |
|---|---|
| START from READY | **Start** |
| START from FINISHED / HALTED · long-press from ERROR | **Reset** |
| PAUSE / RESUME | **Hold / Unhold** |
| STOP/DRAIN | **Stop** |
| ESTOP | **Abort** |
| DRAIN to silence/ack in ERROR | **Clear** |
| RESTART (re-run current stage) | *(none — Axis-B recipe action, re-runs the current phase)* |

### 2c. Gaps the standard exposes (resolved in the re-architecture, not now)

1. **`currentState` conflates Axis A + Axis B** — the headline gap.
2. **START is overloaded** across PackML's distinct `Start` and `Reset`.
3. **Missing acting states**: no distinct `Completing`, `Resetting`, `Clearing`,
   `Unholding`/`Unsuspending` — the firmware jumps through them instantly.
4. **No `Suspended`/`Suspending`**: a mid-cycle sensor dropout **aborts** today.
   PackML's intended use is to *suspend* on an external condition and auto-resume
   — a candidate future safety improvement (don't scrap a half-clean keg on a
   momentary air-pressure blip).
5. **`Held` is a flag, not a state** — kept as a mode per earlier decision; the
   re-architecture revisits whether to promote it.

---

## 3. Axis B — ISA‑88 recipe (the wash itself)

The 10 running stages are an **ISA‑88 procedure**, executed while PackML = `Execute`.
The home-grown grouping from the first draft lines up with ISA‑88 — only the names
change:

| ISA‑88 level | KegWasher | Members |
|---|---|---|
| **Procedure** | "Wash a keg" | the whole cycle |
| **Unit Procedure** | single unit | one washer = one unit procedure |
| **Operation** | *(was "segment")* | Pre‑Clean · Caustic · Rinse · Sanitize · Seal |
| **Phase** | *(was "stage")* | the 10 `STATE_*` running states |
| **Phase class** | *(was "stage kind")* | DRAIN · FLOW · PURGE · RECIRC · RETURN · CHARGE (reusable phase types) |

### 3a. Phase classes (reusable phase definitions)

Every wash phase is exactly one class; the class fixes the valve template, the
monitored resource, and the STOP-evac routing — so each phase row is mechanical.
Process words (caustic, sanitize, recirculate, purge, CO₂ charge) are standard
**CIP (Clean‑in‑Place)** terminology.

| Phase class | Valve template (outputs ON) | Drain | Monitored resource | Invariant |
|---|---|---|---|---|
| **DRAIN** | drain (+5 s air burst at start) | open | air (burst only) | — |
| **FLOW** | water + drain | open | water | — |
| **PURGE** | air + drain | open | air | — |
| **RECIRC** | chem + pump | closed | caustic temp (WASHING only) | — |
| **RETURN** | chem + push‑gas, **drain+pump OFF** | closed | push‑gas (air/CO₂) | **chem → reservoir, never drain** |
| **CHARGE** | gas only, valves closed | closed | CO₂ | — |

### 3b. The recipe (Operations → Phases)

| # | `STATE_*` | Screen label | Operation | Phase class | Fluid/Gas | Destination | Timer |
|---|-----------|--------------|-----------|-------------|-----------|-------------|-------|
| 1 | DIRTY_DRAIN | `DIRTY DRAIN` | Pre‑Clean | DRAIN | product (+air) | drain | `dirtyDrainTimer` |
| 2 | DIRTY_RINSE | `DIRTY RINSE` | Pre‑Clean | FLOW | water | drain | `dirtyRinseTimer` |
| 3 | DIRTY_PURGE | `DIRTY PURGE` | Pre‑Clean | PURGE | air | drain | `dirtyPurgeTimer` |
| 4 | WASHING | `WASHING` | Caustic | RECIRC | caustic | (loop) | `washTimer` |
| 5 | CAUSTIC_RETURN | `CAUSTIC RTN` | Caustic | RETURN | caustic ← air | caustic reservoir | `causticRtnTimer` |
| 6 | RINSING | `RINSING` | Rinse | FLOW | water | drain | `rinseTimer` |
| 7 | RINSE_PURGE | `RINSE PURGE` | Rinse | PURGE | air | drain | `rinsePurgeTimer` |
| 8 | SANITIZE | `SANITIZE` | Sanitize | RECIRC | sanitizer | (loop) | `saniTimer` |
| 9 | SANI_RETURN | `SANI RTN` | Sanitize | RETURN | sanitizer ← CO₂ | sani reservoir | `saniRtnTimer` |
| 10 | PRESSURE | `PRESSURE` | Seal | CHARGE | CO₂ | sealed in keg | `purgeTimer` |

*Canonical spoken names:* "Dirty Drain", "Dirty Rinse", "Dirty Purge", "Caustic
Wash", "Caustic Return", "Rinse", "Rinse Purge", "Sanitize", "Sani Return",
"Pressurize". Screen labels are the abbreviations above (title-bar width).

### 3c. Phase entry / exit / advance (the "organize entry/exit" ask)

| Phase | Entry gate † | Outputs on entry | Outputs cleared on exit | Advances when |
|-------|--------------|------------------|--------------------------|----------------|
| DIRTY_DRAIN | air | drain | drain, air | timer (air auto-off @5 s) |
| DIRTY_RINSE | water | water, drain | water, drain | timer |
| DIRTY_PURGE | air | air, drain | air, drain | timer |
| WASHING | caustic ≥ 50 °C | caustic, pump | caustic, pump | timer |
| CAUSTIC_RETURN | air | caustic, air (drain/pump forced OFF) | air, caustic | timer |
| RINSING | water | water, drain | water, drain | timer |
| RINSE_PURGE | air | air, drain | air, drain | timer |
| SANITIZE | — | sanitizer, pump | sanitizer, pump | timer |
| SANI_RETURN | CO₂ | sanitizer, CO₂ (drain/pump forced OFF) | CO₂, sanitizer | timer |
| PRESSURE | CO₂ | CO₂ | CO₂ | timer |

† Entry gates and the per-tick monitors (§3d) are `#ifndef BENCH_MODE` (live only
on the prototype). **`Abort` (ESTOP), hard state timeouts, and the watchdog are
unconditional in every state/phase.**

### 3d. Per-phase active-resource monitors (`monitorActiveResources`)

Aborts to `Aborted` if the resource a phase is *actively using* drops out.
(Enclosure overtemp is no longer monitored — the enclosure fan self-regulates
off-controller, so the controller has no enclosure-temp input.)

| Condition | Phases | Error |
|-----------|--------|-------|
| `!isAirOk` | DIRTY_DRAIN (≤5 s), DIRTY_PURGE, CAUSTIC_RETURN, RINSE_PURGE | `ERR_AIR_PRESSURE` |
| `!isWaterOk` | DIRTY_RINSE, RINSING | `ERR_WATER_PRESSURE` |
| caustic temp < `MIN_CAUSTIC_TEMP` | WASHING | `ERR_CAUSTIC_TEMP` |

CO₂ is **not** per-tick monitored (changed 2026-06-12): the only sensor (`co2Ok`,
DI7) sits downstream of the `co2Out` solenoid and reads keg/line pressure, which
is indeterminate during SANI_RETURN blowback and 0 whenever the valve is closed.
Instead the PRESSURE phase is closed-loop — it ends the moment the switch trips
(keg at setpoint; solenoid shut), and raises `ERR_CO2_PRESSURE` ("Keg not at
pressure") if the switch hasn't tripped when `purgeTimer` expires. That timeout
is the cycle's CO₂-supply + keg-seal verification.

Hard timeouts (`stateMaxDuration[]`): phases 10 min (WASHING 20), STOPPING 2 min;
Idle/Complete/Aborted/Stopped have none (operator-paced).

---

## 4. Axis C — Modes (PackML, orthogonal)

PackML modes are a third dimension governing which states/transitions are allowed.
KegWasher's existing mode-like concepts map as:

| KegWasher | PackML mode | Notes |
|-----------|-------------|-------|
| normal run (`BENCH_MODE` off) | **Production** | full automation, all interlocks live |
| diagnostics output-exercise (DRAIN+START) | **Manual** | step/jog individual outputs |
| *(none yet)* | **Maintenance** | candidate: relaxed interlocks for service |
| `BENCH_MODE` (compile-time) | ≈ commissioning/test | **not** a clean runtime mode — a build flag that bypasses sensor gates; flagged as tech-debt to convert into a real Manual/Maintenance runtime mode |

**Non-mode latches** (carry data, not a PackML mode): `kegSizeLatched`
(SMALL/LARGE timer scale), `washerInitialized` (one-time pre-check done),
`evacKind` (STOP evac routing).

---

## 5. Error / Abort taxonomy

All faults land in PackML `Aborted` (`STATE_ERROR`) with an `errorCode`:

| Code | `ERR_*` | PackML view |
|------|---------|-------------|
| 3 | WATER_PRESSURE | abort cause |
| 4 | AIR_PRESSURE | abort cause |
| 5 | CO2_PRESSURE | abort cause |
| 6 | CAUSTIC_TEMP | abort cause |
| 7 | *(retired)* | was ENCLOSURE_TEMP — enclosure temp off-controller; code not reused |
| 8 | ESTOP | `Abort` command |
| 9 | INVALID_STATE | abort cause |
| 10–13 | HEATING_*/CAUSTIC_LEVEL/HEATER_OVERTEMP | abort cause (Starting/heating) |
| 14 | STATE_TIMEOUT | abort cause (SC never arrived) |
| 15 | SENSOR_FAULT | abort cause |
| 16 | PAUSE_TIMEOUT | `Held` too long → abort |
| 1–2 | SD_INIT / CONFIG_FILE | startup abort cause |

Recovery: `Clear` (acknowledge/silence) then `Reset` (→ Idle). Firmware collapses
both into the long-press-START path today.

---

## 6. Indicator & banner colour policy (IEC 60204‑1)

Two on-screen elements, **distinct jobs**:
- **Title bar** (`chrome()`) — steady; the state/phase **name** + status colour. The
  at-a-glance "what state am I in."
- **Flashing banner** (`banner()`) — flashing; reserved for **attention / required
  action** (fault recovery instruction, "SWAP KEG"). *Pass 2 goal:* stop it
  duplicating the title on normal states.

Colours follow **IEC 60204‑1 §10.3** (indicator lights), applied to title bar,
banner, and any future stacklight tier alike:

| Colour | IEC meaning | KegWasher states |
|--------|-------------|------------------|
| 🟢 GREEN | normal operation | EXECUTE/washing, READY |
| 🟠 AMBER | abnormal / impending — monitor/intervene | NOT READY, HEATING (STARTING), HELD/PAUSED, STOPPING/DRAINING |
| 🔵 BLUE | mandatory operator action | COMPLETE (swap keg) |
| 🔴 RED | emergency / fault | ABORTED, ESTOP, all faults |
| ⚪ WHITE | power / status | (footer/info) |

**IO4 red stacklight** (the one physical tier wired today) is fault-only with a
3-state behaviour: **OFF** normal · **STEADY** while the fault condition is active ·
**FLASH 250 ms** once it clears and is waiting for acknowledgment. A future
Red/Amber/Green(/Blue) tower light maps straight onto the table above.

---

## Sources

- [ISA‑TR88.00.02‑2022, Machine and Unit States (ISA)](https://www.isa.org/products/isa-tr88-00-02-2022-machine-and-unit-states-an-imp)
- [ANSI/ISA‑TR88.00.02‑2015 preview (ANSI webstore)](https://webstore.ansi.org/preview-pages/ISA/preview_ISA+TR88.00.02-2015.pdf)
- [PackML States Explained — all 17 states & modes (Frostbyte)](https://frostbytesoftware.co.nz/blog/packml-states-explained)
- [PackML — Wikipedia](https://en.wikipedia.org/wiki/PackML)
- [ISA‑88 — Wikipedia](https://en.wikipedia.org/wiki/ISA-88)
- [ISA‑88 (S88) Batch Control Explained — PLC Academy](https://www.plcacademy.com/isa-88-s88-batch-control-explained/)
- [ISA‑88 Phases & Equipment Modules (SG Systems)](https://sgsystemsglobal.com/glossary/isa-88-phases-equipment-modules/)
