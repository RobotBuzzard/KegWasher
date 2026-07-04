# KegWasher tools

Bench helpers for the **gen4-uLCD-43DT (Diablo16)** display on the current
**raw SPE serial** path (no ViSi-Genie / Workshop4), plus a couple of ClearCore
recovery aids and the network config editor. Each `*/` sketch is a standalone
Arduino sketch (folder name == `.ino` name); the `.py` helpers and `kwcfg` run
on the dev host.

## Network config editor

| Tool | What it does |
|------|--------------|
| `kwcfg` | Stand-alone remote editor for the machine's `WASHER.CFG` over MQTT. `kwcfg get` (live config from the retained `kegwasher/cfg/*` mirror), `kwcfg set KEY=VALUE ...` (writes via `cmd/cfgset` — firmware validates with the SD-loader rules, persists to the card, acks on `cfg/ack`), `kwcfg edit` (fetch → `$EDITOR` → apply diff), `kwcfg watch`. Uses `mosquitto_pub/sub` (no Python deps); broker/creds default to `192.168.1.111` / rr1 from `~/mosquitto-credentials.txt`, overridable via `KWCFG_*` env or flags. Edits are refused while a cycle runs, and `mqtt*` keys are never remote-settable. |

> Firmware flashing is **not** here — use the canonical wrapper:
> `~/dev/teknic-clearcore-cli/scripts/flash.sh ~/dev/KegWasher /dev/ttyACM0`
> (arduino-cli compile → 1200-baud bootloader touch → bossac → app-mode verify).

## Display — raw SPE serial (current path)

| Tool | What it does |
|------|--------------|
| `spe_smoke.py` | Host-side smoke test: sends SPE GFX draw commands over the serial link to confirm the panel renders. |
| `diablo_draw_test/` | On-ClearCore proof of raw-serial drawing on the gen4-uLCD-43DT. |
| `diablo_ui_test/` | Exercises the `KegDisplaySerial` (`KDS::*`) module on real hardware. |
| `diablo_touch_cal/` | 3-point resistive-touch calibration; emits the affine coeffs that go into `WASHER.CFG` (`touchCalA..F`) — the canonical re-cal tool referenced from CLAUDE.md. |
| `diablo_touch_demo/` · `diablo_touch_test/` | Verify resistive touch / direct Diablo16 serial control. |
| `diablo_baud_probe/` | Find the panel's SPE comms baud (handshake at 9600, link bumped to 115200). |
| `diablo_cal/` | Measure the SPE font cell height (layout metrics for `KegDisplaySerial`). |

The `diablo_touch_cal/`, `diablo_touch_demo/`, and `diablo_ui_test/` sketches
carry their own copy of `KegDisplaySerial.{h,cpp}` (copied from the project root)
so they compile standalone.

## ClearCore recovery

| Tool | What it does |
|------|--------------|
| `cc_recover.sh` | Catches the ClearCore's post-reset USB window and flashes a known-good sketch — recovery for a board stuck off the USB bus (see the `clearcore_4d_lib_global_ctor_brick` note: a 4D `_Serial_4DLib` global ctor can brick the CC pre-USB). |
| `cc_serial_heartbeat/` | Minimal, guaranteed USB-safe sketch — flash it to get a board back on the bus, then reflash the real firmware. |

---

*Removed 2026-06-08:* the ViSi-Genie/Workshop4 push pipeline (`push.sh`) and the
PmmC-bootstrap / Wine-reset experiment scripts (`diablo_flasher.py`,
`diablo_probe.py`, `reset_test.py`, the `*_reset_knock` / `dtr_*` / `rts_*` /
`gpio*` family, `cp2104_config.py`, `bin2uf2.py`). All belonged to the abandoned
genie + display-flashing effort.
