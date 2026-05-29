# KegWasher tools

Helper scripts for the dual-target KegWasher (ClearCore firmware + gen4-uLCD-43DT
ViSi-Genie display).

## push.sh — dual-target update helper

```
tools/push.sh --firmware   # compile + flash the ClearCore sketch
tools/push.sh --display    # push compiled Genie files to the display's uSD
tools/push.sh --all        # both
tools/push.sh --help
```

Built from a verified toolchain investigation (2026-05-29) — it only runs
commands confirmed against real evidence and **pauses with instructions** for
GUI-only or unverified steps rather than guessing.

### Iteration model

**Firmware (ClearCore SAME53) — pure CLI:**
1. Edit `KegWasher.ino` / `Keg*.cpp` / `Keg*.h`
2. Plug in the ClearCore (USB-C, front of board)
3. `tools/push.sh --firmware` — wraps the verified
   `~/dev/teknic-clearcore-cli/scripts/flash.sh` (arduino-cli compile → 1200-baud
   bootloader touch → bossac → app-mode verify). Refuses unless the Teknic VID
   2890 enumerates; disambiguates the `/dev/ttyACM*` node instead of hardcoding.

**Display (gen4-uLCD-43DT) — hybrid (compile is GUI-bound):**
1. Edit `KegWasher.4DGenie` in Workshop4
2. **Recompile in the Workshop4 GUI** — regenerates `KegWasher.4DGenieS`,
   `KEGW~UCD.{gci,dat,txf}`, and `KegWasher.4XE`. GUI-only: the
   `.4DGenie → .4DGenieS` ViSi-Genie codegen is embedded in WORKSHOP4.exe and
   the `.bin → .4XE` wrap (17-byte `AA 55` header) is not reproducible from any
   verified standalone tool. (The `.4DGenieS → .bin` stage *is* headless via
   `4dcompiler.exe` and byte-identical to the GUI build, but that doesn't help
   without the codegen + wrap stages.)
3. Put the uSD in the PC reader, `tools/push.sh --display` — copies the 3 media
   files verbatim + `KegWasher.4XE` as `RunBank1.4xe`, with a staleness guard
   (warns if `.4XE` is older than `.4DGenie`).
4. Eject uSD → reinsert in display → power-cycle. The Bank-0 stub auto-loads.

### One-time bootstrap (before SD-swap iteration works)

Both are Workshop4-GUI Tools-menu steps, subject to the manual RESET-short
timing (see memory `cc_adapter_diablo16_pmmc_blocked`):

- **Bank 0 "Update Bank(s) and Run" stub** — makes the display auto-load
  `RunBank1.4xe` + media from the uSD on each boot. (UTILS.CFG maps this to an
  internal `UpRunBank` handler — no CLI; GUI only.)
- **Bank 5 inherent widgets** ("Load Inherents into Bank 5") — without it the
  display throws on-screen `Error 30`.

### Open items the script can't resolve (need user confirmation)

- Exact Workshop4 GUI menu/button that recompiles `.4DGenie → .4XE` (not in any
  local doc; `F9` is File-Transfer, not compile).
- Confirm project **Destination = uSD** in Workshop4 (the `.4DGenie` records
  `Destination 1` but the RAM/Flash/uSD integer encoding is undocumented). The
  swap-SD steady state only holds if it's uSD.
- Whether Bank 0 + Bank 5 have actually been flashed yet.

## diablo_probe.py / diablo_flasher.py / spe_smoke.py

PmmC-bootstrap-era tools. `diablo_probe.py` confirms the Diablo16 bootloader
handshake (`4dgl`+`U` → `0x44 'D'` at 115200). `spe_smoke.py` sends SPE GFX
commands to verify the panel renders. `diablo_flasher.py` is a skeleton (the
byte-level PmmC flash protocol was never needed — the standalone PMMCLOADER.EXE
recipe worked; see memory).
