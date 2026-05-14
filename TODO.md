# KegWasher TODO

Live roadmap from current bench-only build to a production-ready keg cleaner controller. Reorganised when the project's state changes meaningfully — not just a backlog dump. Deeper per-item rationale lives in [`docs/reliability-todo.md`](docs/reliability-todo.md).

## Status (2026-05-14)

Bench-only development build. Firmware compiles, flashes, runs through a complete BENCH_MODE cycle (STARTUP → DRAINING → RINSING → WASHING → SANITIZE → PRESSURE → FINISHED) in ~25 s with the display updating correctly on every transition. Hardware watchdog active and bench-validated. ESTOP polarity is correct for normally-closed wiring. No real plumbing is connected; sensor gates are bypassed via `#define BENCH_MODE` in `KegConfig.h`.

Ethernet cable just plugged in. Firmware doesn't touch the network yet — Phase 0 below is where that work starts.

---

## Phase 0 — Ethernet & remote access (in progress)

The cable's in. Goal: an HTTP control panel for start/silence/reset plus a JSON status endpoint, so we can monitor and drive the bench (and eventually production rigs) from any browser on the LAN.

- [ ] **Init Ethernet + DHCP** — minimal setup() addition using the ClearCore-bundled `Ethernet` library. Print the assigned IP via `diagnostics_logEvent` and on the boot splash. Verify `ping` works from the dev machine.
- [ ] **Network log mirror** — `diagnostics_logEvent()` writes to a configured TCP/UDP endpoint *in addition to* USB Serial. Side-steps the Serial-Monitor-port-holder gotcha and lets us tail logs from anywhere.
- [ ] **`GET /` static HTML status page** — single self-contained page with current state, mm:ss remaining, sensor readings (best-effort under BENCH_MODE), last error. Meta-refresh @ 1 Hz for v1 (no JS) so even a phone browser works.
- [ ] **`GET /api/status` JSON** — same data as the HTML page, machine-readable. `{ "state": "WASHING", "remaining_ms": 142000, ... }`.
- [ ] **`POST /api/start` / `/api/silence` / `/api/reset`** — same effects as the physical buttons. Shared-key auth via `X-API-Key` header. `start` only valid in STARTUP_READY / FINISHED.
- [ ] **WDT cooperation** — request handling must fit under the 8 s watchdog. Kick the WDT in the HTTP loop. Document the constraint in code comments.
- [ ] **LAN-only by default** — listen on the link-local interface; no auth bypass; future: TLS or VPN for off-network access.

## Phase 1 — Hardware bring-up (gated on real machine existing)

When the actual keg-washer chassis is built and wired, this is the I/O verification pass. Bench mode stays on for the start of this; we turn it off only after every input + output is confirmed.

- [ ] **Wire each line in `docs/io-table.md`** — every output (drainOut, waterOut, …, causticHeaterOut) to the corresponding relay/solenoid; every input (airOk, co2Ok, …) to the corresponding switch/sensor.
- [ ] **Diagnostic mode pass** — hold DRAIN + START in STARTUP/FINISHED to enter `diagnostics_runTest()`. Verify each output drives the right relay (visible / audible click), each input reads the expected state when stimulated.
- [ ] **Verify ESTOP both ways** — confirm pressing E-STOP kills outputs via the ISR (instant), and that the main-loop ERROR transition fires and renders the screen.
- [ ] **Verify watchdog with real loads** — if any peripheral (CCIO, Goldelox) hangs at load, confirm WDT recovers; document any new long-blocking paths that need bracketing.
- [ ] **Sensor calibration** — `causticTemp` and `enclosureTemp` against known reference points (ice water 0°C, boiling 100°C). Persist offsets (path TBD — likely NVM or `washer.config`).
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
- [ ] **Power-fail recovery** ([`reliability-todo.md` P2](docs/reliability-todo.md)) — persist `currentState`, `errorCode`, and stage elapsed time to NVM on each state transition. On boot, prompt the operator to resume or abort if the previous cycle was interrupted.
- [ ] **Config checksum + EEPROM/NVM backup** ([`reliability-todo.md` P3](docs/reliability-todo.md)) — CRC on `washer.config`; if the SD copy is corrupt, fall back to the NVM-saved copy instead of compiled defaults.
- [ ] **State history ring buffer** ([`reliability-todo.md` P4](docs/reliability-todo.md)) — last N state changes with millis timestamps; exposed via `/api/history` for post-incident analysis.
- [ ] **SD card cycle logging** ([`reliability-todo.md` P6](docs/reliability-todo.md)) — append-only log of completed cycles (timestamp, keg size, duration per stage, peak/min temps). Mind the SD wear budget — daily file rotation, not per-event flush.
- [ ] **Heap + loop-time monitoring** ([`reliability-todo.md` P6](docs/reliability-todo.md)) — periodic `diagnostics_logEvent` with free RAM and longest recent loop iteration. Visible in network log + status JSON.
- [ ] **`docs/clearcore-reference.md`** ([`reliability-todo.md` P0](docs/reliability-todo.md)) — index of permalinks to ClearCore datasheet, CCIO-8 protocol, SAME53 datasheet, Goldelox library docs; inline `// See: <ref> §<section>` comments at every hardware-specific touch point.
- [ ] **Enhanced state validation** ([`reliability-todo.md` P4](docs/reliability-todo.md)) — preconditions on every state entry, not just WASHING (e.g. CO2 OK before PRESSURE, water OK before RINSING).

## Phase 4 — Long-term stability (continuous)

Cross-cutting items for the weeks/months-between-resets goal. Most can land any time after Phase 0 starts.

- [ ] **Display health check** — periodic Goldelox ACK round-trip; on no-response, re-run `display_init()` to re-sync.
- [ ] **Idle self-test** — FINISHED + N hours of inactivity → read all sensors, verify in-range, log heartbeat. No actuator output.
- [ ] **Uptime + cycle counters** — `uint64_t` seconds-uptime accumulator (rolls in centuries), `unsigned long` cycle count. Display on a diagnostics page; published via `/api/status`.
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

---

## Recently done (last ~2 weeks)

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
