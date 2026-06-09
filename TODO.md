# KegWasher TODO

Live roadmap from current bench-only build to a production-ready keg cleaner controller. Reorganised when the project's state changes meaningfully — not just a backlog dump. Deeper per-item rationale lives in [`docs/reliability-todo.md`](docs/reliability-todo.md).

## Status (2026-06-08)

**Update 2026-06-08 (PackML re-arch + sensor bring-up — bench-verified on hardware):** The state
machine was re-architected to the **two-axis PackML / ISA-88 model** (`machineState` × `recipePhase`)
and merged to main. Config schema promoted temp thresholds + the MQTT broker block to runtime,
SD-overridable values (Phase 3); an **on-screen settings editor** lets the operator tune stage timers
on the panel (Phase 5); a single Linux-style **boot screen** confirms the SD-config read. Sensor
bring-up: the **ProSense ETS50N caustic probe on A10** (linear 4–20 mA map across a 470 Ω shunt) is
wired and **calibrated — reads true**. The **physical START button (IO1, active-high)** is confirmed
working and lit via the new **GREEN ready indicator on IO5** (RED fault/E-stop stacklight on IO4).
**Enclosure temp + the fan were removed from the controller** — the fan is now a standalone
self-regulating unit with its own thermometer, so **A9 + CCIO-A7 are free** and `ERR_ENCLOSURE_TEMP`
(code 7) is retired. `docs/` (state-table, io-table, state-taxonomy) are synced to the as-built model.

**Update 2026-06-04 (cycle + controls rework — bench-verified on hardware):** The cycle is now the
full **10-stage** sequence with chemical recovery: `DIRTY_DRAIN → DIRTY_RINSE → DIRTY_PURGE → WASHING
→ CAUSTIC_RETURN → RINSING → RINSE_PURGE → SANITIZE → SANI_RETURN → PRESSURE` (caustic/sani blown back
to their reservoirs, never to drain). Added: per-state safety monitoring + entry preconditions
(bench-gated), one-press-per-keg flow with a one-time power-up pre-check, on-screen + MQTT cycle
controls (PAUSE/RESUME, RESTART = re-run current stage, STOP/DRAIN = evacuate → HALTED), and a
countdown-skip fix. SD card formatted + `washer.config` written.

Bench-only development build. Firmware compiles, flashes, runs through a complete BENCH_MODE cycle (STARTUP → DRAINING → RINSING → WASHING → SANITIZE → PRESSURE → FINISHED) in ~25 s with the display updating correctly on every transition. Hardware watchdog active and bench-validated. ESTOP polarity is correct for normally-closed wiring. No real plumbing is connected; sensor gates are bypassed via `#define BENCH_MODE` in `KegConfig.h`.

**Display (2026-06-03):** dropped the planned ViSi-Genie path for **raw SPE serial** drawing on the gen4-uLCD-43DT (Diablo16, portrait 272×480, link bumped to 115200 for ~12× faster redraws). Per-state screens, flicker-free partial updates, footer (IP + MQTT pub/sub dots), host-side-calibrated resistive touch, and an on-screen START button — all verified on hardware. `genieArduinoDEV` removed. See CLAUDE.md "Display" + the `kegwasher_display_serial` memory. (A web/HTTP HMI was re-assessed as feasible 2026-06-04 — an option that revisits the Phase 0 "no HTTP" note below.)

Ethernet up, DHCP working, MQTT log mirror live to Mosquitto on CheapBourbon (`192.168.1.111:1883`). Footer of every running-cycle screen shows the device IP plus a purple `M*` indicator when the broker is connected.

Decided 2026-05-14: **no HTTP server on the ClearCore.** All remote status/control flows through MQTT pub/sub instead — fewer protocols, broker handles auth, LWT + retained topics give "is it alive?" and "what's it doing right now?" for free. Dashboards and command UIs live off-device on Node-RED. HTTP-server items previously listed in Phase 0 have been removed.

---

## Phase 0 — Remote operability via MQTT (in progress)

Goal: full status read-out + full command surface available to any MQTT subscriber on the LAN, then a Node-RED operator dashboard on top of those topics. Replaces the originally-planned on-device HTTP server.

- [x] **Init Ethernet + DHCP** — `setupEthernet()` in `KegWasher.ino`. Local IP exposed to the display footer.
- [x] **Network log mirror via MQTT** — every `diagnostics_logEvent` line is published to `kegwasher/log`. LWT publishes `kegwasher/online=false` on disconnect; retained `kegwasher/online=true` and `kegwasher/ip` give new subscribers immediate liveness.
- [x] **Footer indicators** — `M*` (purple) when MQTT is connected; HTTP `H*` slot reserved but unused (HTTP server is no longer planned, slot may be repurposed).
- [x] **MQTT status publish** — change-detected, retained publishes for `kegwasher/state`, `kegwasher/state/sub`, `kegwasher/keg`, `kegwasher/timer/{elapsed_s,remaining_s}` (seconds, not ms — dashboards consume mm:ss directly), `kegwasher/temp/{caustic,enclosure}`, `kegwasher/level/caustic`, `kegwasher/sensors/{water,air,co2,estop}`, `kegwasher/error/{code,message}`. Throttled to 250 ms; only publishes topics whose values actually changed since last call. All retained so dashboards land hot.
- [x] **MQTT command subscribe** — subscribe to `kegwasher/cmd/+`. Callback sets pending-command flags only (no `kwMqtt.publish` from callback context — PubSubClient shares one buffer for send/recv, mid-receive publishes corrupt the parser; bench-confirmed). `mqtt_applyCmdFlags()` runs in the safe loop context and OR's the flags into `isCycleStartPressed` / `isManualDrainPressed` so state handlers act on them through their existing button code paths. `cmd/start` works in STARTUP_READY (begin cycle) and FINISHED (next keg); `cmd/silence` works in ERROR/FINISHED (silence alarm); `cmd/reset` works only in ERROR (clear errorCode + transition to STARTUP). All commands refused if ESTOP is active. `cmd/keg_size` deferred — needs a remote-override mechanism since `isLargeKeg` is overwritten by `hardware_readInputs()` every tick.
- [x] **Heartbeat + free-RAM publish** — `kegwasher/heartbeat/{uptime_s, free_ram, loop_max_us}` published every 5 s, all retained. `loop_max_us` is per-tick max accumulator, **reset to 0 after each publish** so each window's reading is independent (otherwise a single boot-time display spike would dominate forever). Bench-validated: ~300 µs in STARTUP_READY, ~135 KB free of 192 KB total.
- [ ] **Node-RED dashboard on CheapBourbon** — install Node-RED + `node-red-dashboard` alongside Mosquitto on `.111`. Single page: state badge, mm:ss timer, temp gauges, sensor LED grid, Start/Silence/Reset/Keg-size buttons. Wired purely to the MQTT topics above; not coupled to the ClearCore beyond the broker.
- [ ] **Light alerting** — Node-RED flow: if `kegwasher/state` stays in `ERROR` for >5 min, ping somewhere (initially just a Node-RED notification; later email/SMS).

## Phase 0.5 — SD-card configurator app

Right now MQTT broker IP, credentials, topic root, and timer/threshold values are either hard-coded in firmware (`KegSecrets.h`) or hand-edited in `washer.config` on the SD card. Both are tedious and easy to typo in the field. Goal: a host-side configurator app (web or desktop) that produces a fully-validated `washer.config` written directly to a mounted SD card, with mandatory-field enforcement so an under-configured card can never produce a partially-broken cleaner.

- [ ] **Pick host-side stack** — likely a small Python/Tk or a tiny single-page web app (Flask + form, mounted in Node-RED, or static + browser-native file save). Decide based on cross-platform reach (Windows brewer laptop vs. Linux dev machine).
- [ ] **Define `washer.config` schema** — promote the current key=value file to a versioned schema. Mandatory fields: `mqtt.broker_ip`, `mqtt.broker_port`, `mqtt.user`, `mqtt.pass`, `mqtt.client_id`, `mqtt.topic_root`. Optional/overridable: every timer in `KegConfig.h` (`dirtyDrainTimer`, `rinseTimer`, …), every threshold (`MAX_CAUSTIC_TEMP`, `MIN_HEATING_RATE`, …), `keg_large_modifier`.
- [ ] **Host-side validation** — IP format + reachability ping, port 1-65535, non-empty creds, client_id charset, topic_root no-slash-prefix, all numerics within firmware-enforced ranges. Refuse to write a card with any mandatory field empty or any value out of range.
- [ ] **Output: full `washer.config` + a checksum line** — atomic write (tmp file + rename), include a CRC32 in the file so the firmware can detect corruption (ties into Phase 3's checksum item).
- [ ] **Firmware-side: load broker config from SD instead of `KegSecrets.h`** — make `MQTT_BROKER_IP_*` / user / pass / client_id / topic_root all SD-driven. Drop the gitignored secrets header for production builds; keep a fallback hard-coded set for emergency boot when no card is present (logs a loud "CONFIG MISSING" banner).
- [ ] **Pre-flight on the device** — at boot, if `washer.config` is missing or fails checksum, display "INSERT CONFIG SD" on the Goldelox and halt before arming any outputs. No silent default-to-bench mode in production.
- [ ] **Configurator app distributed alongside the firmware** — README pointer; packaged as a standalone runnable so a brewery tech with no Python install can run it from a USB stick.

## Phase 1 — Hardware bring-up (gated on real machine existing)

When the actual keg-washer chassis is built and wired, this is the I/O verification pass. Bench mode stays on for the start of this; we turn it off only after every input + output is confirmed.

- [ ] **Wire each line in `docs/io-table.md`** — every output (drainOut, waterOut, …, causticHeaterOut) to the corresponding relay/solenoid; every input (airOk, co2Ok, …) to the corresponding switch/sensor.
- [ ] **Diagnostic mode pass** — hold DRAIN + START in STARTUP/FINISHED to enter `diagnostics_runTest()`. Verify each output drives the right relay (visible / audible click), each input reads the expected state when stimulated.
- [ ] **Verify ESTOP both ways** — confirm pressing E-STOP kills outputs via the ISR (instant), and that the main-loop ERROR transition fires and renders the screen.
- [ ] **Verify watchdog with real loads** — if any peripheral (CCIO, Goldelox) hangs at load, confirm WDT recovers; document any new long-blocking paths that need bracketing.
- [x] **Caustic temp via ProSense ETS50N (4–20 mA)** — *Done + calibrated (2026-06-08; reads true on the bench).* A10 probe is a ProSense ETS50N‑150‑1001 (−50…150 °C), 4–20 mA. Wiring: Brown→24 V, Blue→GND, **White→A‑10 + a 470 Ω (measured 469.9 Ω) shunt from A‑10→GND**. Firmware `getCausticTemp()` is a linear 4–20 mA→°C map across the shunt; `<3.6 mA` = loop fault → fail cold. Span left at the native −50…150 °C. Black/PNP still available as an independent ~70 °C hardware cutoff into the E‑stop chain if wanted. *Loose end:* drop the temporary `debug/cau_adc` MQTT publish now that cal is confirmed (`debug/enc_adc` already removed).
- [x] **Enclosure temp + fan — REMOVED from the controller** — *Done (2026-06-08, commit `c50fb44`).* The enclosure fan is now a standalone self-regulating unit with its own thermometer, so the controller no longer senses enclosure temp or drives a fan. Removed: A9 thermistor + β-curve, `getEnclosureTemp`, `hardware_manageFan`, CCIO-A7 `cabinFanOut`, `ERR_ENCLOSURE_TEMP` + its overtemp monitor, and the `maxEnclosureTemp`/`fanOnTemp`/`fanOffTemp` config keys. **A9 + CCIO-A7 are free; code 7 retired.** (This obsoletes the old "lower MAX_ENCLOSURE_TEMP / re-range A9" plan.)
- [ ] **Switch off BENCH_MODE** — comment out `#define BENCH_MODE` in `KegConfig.h`; re-flash; verify `grep -r BENCH_MODE` returns only `#ifdef` references; verify boot splash no longer shows the banner; verify the systems-go gate now blocks STARTUP_READY when sensors aren't asserted.

## Phase 2 — Wet testing

Real fluids in stages. Each step assumes the previous passed.

- [ ] **Water-only cycle** — fill caustic reservoir with plain water, sanitizer reservoir with plain water; run a full cycle on a sacrificial keg. Verify timing, drain behaviour, no leaks.
- [ ] **Air burst verification** — observe the 5 s air bursts at start of DRAINING and end of RINSING actually displace fluid as designed.
- [ ] **Caustic-only test** — caustic in caustic reservoir, plain water in sanitizer. First real heater run — observe the `STARTUP_HEATING` substate progress in real time. Verify `MIN_HEATING_RATE`, `MAX_HEATING_TIME`, and `MAX_CAUSTIC_TEMP` interlocks all fire on out-of-range conditions (force each).
- [ ] **Full chemistry cycle** — actual caustic + actual sanitizer + actual CO2 pressurization. First end-to-end production-equivalent run.
- [ ] **Repeat-cycle stress** — back-to-back cycles, check display, watchdog, sensor stability across 10+ cycles.

## Phase 3 — Pre-production hardening

Items that don't block first wet test but should land before a customer ever sees the firmware.

- [ ] **Per-state display pages refactor** ([`reliability-todo.md` P0](docs/reliability-todo.md)) — replace the every-handler-writes-the-screen pattern with a pending-render queue. Dedicated layouts per state (heating ETA bar, mm:ss countdown grid, FINISHED banner, error code + recovery hint).
- [ ] **Button + indicator refinement** — *deferred by the user until the functions are all figured out (2026-06-08).* Minor changes to the physical buttons/indicators (IO1 START, IO2 DRAIN/ACK, IO4 RED fault, IO5 GREEN ready) and the on-screen controls (START / PAUSE-RESUME / RESTART / STOP-DRAIN / SETTINGS) — behavior, when each lights (solid vs flash), color, label, position. Specifics TBD. Bench-verified so far: START registers + the green ready indicator lights in the right states.
- [ ] **Screen UX overhaul** — once the cleaner is functionally complete (Phases 0-2 done), revisit display content for operator clarity. Things to consider: bigger fonts for the most-glanced values (mm:ss countdown, current state badge), prioritise what's on each screen by what the operator needs at that moment vs. what's nice-to-have, lose information that duplicates the dashboard, add at-a-glance status icons (heater on, valve states), use color more deliberately (red for FAIL, green for OK, etc.), maybe a dedicated "summary" screen for FINISHED with cycle time + peak temps. The current layout/footer-icon precedent lives in the code (`KegDisplaySerial` / `KegDisplay`) + the CLAUDE.md "Display" section.
- [ ] **Power-fail recovery** ([`reliability-todo.md` P2](docs/reliability-todo.md)) — persist `currentState`, `errorCode`, and stage elapsed time to NVM on each state transition. On boot, prompt the operator to resume or abort if the previous cycle was interrupted.
- [ ] **Config checksum + EEPROM/NVM backup** ([`reliability-todo.md` P3](docs/reliability-todo.md)) — CRC on `washer.config`; if the SD copy is corrupt, fall back to the NVM-saved copy instead of compiled defaults.
- [ ] **State history ring buffer** ([`reliability-todo.md` P4](docs/reliability-todo.md)) — last N state changes with millis timestamps; published to `kegwasher/history` on demand (subscribe-triggered) for post-incident analysis. Off-device historical trends are a separate deferred item (see Phase 4).
- [ ] **SD card cycle logging** ([`reliability-todo.md` P6](docs/reliability-todo.md)) — append-only log of completed cycles (timestamp, keg size, duration per stage, peak/min temps). Mind the SD wear budget — daily file rotation, not per-event flush.
- [ ] **Heap + loop-time monitoring** ([`reliability-todo.md` P6](docs/reliability-todo.md)) — periodic `diagnostics_logEvent` with free RAM and longest recent loop iteration. Visible in network log + status JSON.
- [ ] **`docs/clearcore-reference.md`** ([`reliability-todo.md` P0](docs/reliability-todo.md)) — index of permalinks to ClearCore datasheet, CCIO-8 protocol, SAME53 datasheet, Goldelox library docs; inline `// See: <ref> §<section>` comments at every hardware-specific touch point.
- [ ] **Enhanced state validation** ([`reliability-todo.md` P4](docs/reliability-todo.md)) — preconditions on every state entry, not just WASHING (e.g. CO2 OK before PRESSURE, water OK before RINSING).

## Phase 4 — Long-term stability (continuous)

Cross-cutting items for the weeks/months-between-resets goal. Most can land any time after Phase 0 starts.

- [ ] **Display health check** — periodic Goldelox ACK round-trip; on no-response, re-run `display_init()` to re-sync.
- [ ] **Idle self-test** — FINISHED + N hours of inactivity → read all sensors, verify in-range, log heartbeat. No actuator output.
- [ ] **Uptime + cycle counters** — `uint64_t` seconds-uptime accumulator (rolls in centuries), `unsigned long` cycle count. Display on a diagnostics page; published as retained `kegwasher/uptime_s` and `kegwasher/cycle_count`.
- [ ] **InfluxDB + Grafana for trends** (deferred — only when we want historical analysis of caustic-temp drift, cycle-count over time, heating-time over time). Node-RED writes via `node-red-contrib-influxdb` from the same flow that drives the dashboard — no Telegraf needed for a single device. Lives on CheapBourbon alongside Mosquitto + Node-RED.
- [ ] **Sensor drift recalibration prompt** — track expected vs observed at known points (e.g. caustic temp at end of fill vs ambient); flag a recommended re-cal when drift exceeds tolerance.
- [ ] **Output verification feedback** ([`reliability-todo.md` P1](docs/reliability-todo.md)) — *hardware-gated.* Needs flow / pressure / current-sense feedback inputs per actuator before it's implementable.
- [ ] **Redundant temperature sensors** ([`reliability-todo.md` P1](docs/reliability-todo.md)) — *hardware-gated.* Real redundancy requires a second physical sensor per critical channel.

## Phase 5 — Documentation & release

- [ ] **Operator manual** — startup, error recovery, regular maintenance, chemical handling, what every display screen means.
- [ ] **Wiring schematic** — formal drawing matching `docs/io-table.md`, including the E-STOP hardware loop.
- [ ] **State flow + valve timing diagrams** — visual reference for diagnosis.
- [ ] **Acceptance test procedure** — checklist for verifying a built unit before customer handoff.
- [ ] **Submit ClearCoreWatchdog to the Arduino Library Manager** — *done* (PR #8286 merged 2026-05-08). Listed here for the record.
- [ ] **README "Status" section** — drop the bench-only warnings once a unit has actually been through Phase 2; bump version to `v1.0.0`.

## Phase 6 — End-user documentation

Operator-facing materials. Distinct from the developer docs in `docs/`; these ship with the unit.

- [ ] **State-machine taxonomy / lexicon** — a single canonical vocabulary for the cycle: each state's
  operator-facing name + one-line meaning + what the operator should expect/do. The firmware's
  `stateNames[]` (KegStateMachine.cpp) is the source of truth; the lexicon must match the on-screen
  labels exactly so the manual and the panel never disagree. (A taxonomy draft is being prepared in a
  separate conversation — reconcile it against the firmware labels when it lands.)
- [ ] **Operator's manual** — startup, the one-press-per-keg workflow, PAUSE/RESTART/STOP-DRAIN and
  what each does, error recovery (error code → meaning → action, from the lexicon), routine
  maintenance, chemical handling. Incorporates the state lexicon above.
- [ ] **Quick-reference card** — at-a-glance state meanings + button cheat-sheet for the wall by the
  machine.

---

## Recently done (last ~2 weeks)

- ✅ **PackML / ISA-88 re-architecture** — split the monolithic `currentState` into `machineState` (PackML) × `recipePhase` (ISA-88); IDLE sub-states; HELD = Hold/Unhold; abort/recover. ESTOP-tested, merged to main.
- ✅ **10-stage cycle + chem recovery** — full `DIRTY_DRAIN…PRESSURE` with caustic/sani blown back to reservoirs (never to drain); per-state safety monitors + entry preconditions (bench-gated).
- ✅ **Operator flow + cycle controls** — one-press-per-keg with a one-time power-up pre-check; PAUSE/RESUME, RESTART (re-run stage), STOP-DRAIN (evacuate → HALTED); on-screen + MQTT command surface.
- ✅ **Config schema (Phase 3)** — temp thresholds + MQTT broker block promoted to runtime, SD-overridable. SD filename fixed to strict 8.3 `WASHER.CFG` (classic SD lib has no LFN — was silently never loading).
- ✅ **On-screen settings editor (Phase 5)** — tune stage timers on the panel; SAVE → `config_saveToSD()`.
- ✅ **Boot screen** — single Linux-style progressive screen (SD CONFIG / HARDWARE / NETWORK / MQTT / SENSORS → SYSTEMS GO) that confirms the SD-config read.
- ✅ **Indicators remapped** — GREEN ready lamp on IO5 (lit in IDLE-READY / COMPLETE / STOPPED), RED fault/E-stop stacklight on IO4. **IO1 START confirmed working + lit on the panel** (active-high; verified ClearCore inputs are negative-true).
- ✅ **`config_saveToSD` MQTT-clobber fix** — the on-device editor no longer rewrites the MQTT block (was clobbering broker creds + writing the password to the card on every timer edit).
- ✅ **ProSense ETS50N caustic probe on A10** — linear 4–20 mA→°C map across a 470 Ω shunt; **calibrated, reads true**.
- ✅ **Enclosure temp + fan removed from the controller** — fan is now standalone/self-regulating; A9 + CCIO-A7 free; `ERR_ENCLOSURE_TEMP` (code 7) retired.
- ✅ **Docs synced** — `state-table`, `io-table`, and `state-taxonomy` to the as-built two-axis model.
- ✅ Project rewrite from prior claude.ai conversation artifacts → consolidated canonical source tree
- ✅ Input debouncing, sensor validation + filtering, state timeouts, config validation
- ✅ Air-burst at DRAINING start + end of RINSING
- ✅ Display first-character drop + line-width fits
- ✅ ESTOP ISR refactored to be safe (no Serial/CCIO writes from interrupt)
- ✅ Reset cause logging from `RSTC.RCAUSE`
- ✅ **[`teknic-clearcore-cli`](https://github.com/RobotBuzzard/teknic-clearcore-cli)** — published reusable compile/flash workflow with `flash.sh`, preflight, udev rule, install script. Linux-on-the-bench gotchas (ModemManager, port-holders, stuck-bootloader) all documented.
- ✅ **[`ClearCoreWatchdog`](https://github.com/RobotBuzzard/ClearCoreWatchdog)** — published Arduino library filling the gap left by Teknic's WDT-less API. Available in the Arduino Library Manager.
- ✅ `BENCH_MODE` flag — full bench cycle in 25 s without sensors connected, banner everywhere it could be missed.
- ✅ `state_finished` proper render (was stale-stuck on last PRESSURE frame).
- ✅ ESTOP polarity fixed for NC wiring (LOW = closed = not pressed → input HIGH on press; ISR now RISING).
- ✅ Display refresh slowed to 1 Hz (was thrashing the 9600-baud Goldelox at 4 Hz).
- ✅ README pre-production warnings — bench-only state unmissable on the project front page.
- ✅ Ethernet + DHCP at boot; IP shown in display footer; live IP also retained-published to `kegwasher/ip`.
- ✅ MQTT log mirror to Mosquitto at `192.168.1.111:1883` with LWT + retained `online`/`ip` topics. Every `diagnostics_logEvent` line streams to `kegwasher/log`. Tail with `mosquitto_sub -h 192.168.1.111 -u rr1 -P <pass> -t 'kegwasher/#' -v`.
- ✅ Purple `M*` MQTT-connected indicator in display footer's bottom-right corner.
