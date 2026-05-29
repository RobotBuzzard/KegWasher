# Documentation Inventory & Gaps

Reference-doc census for the KegWasher project as of 2026-05-29. Covers the
display (4D Systems gen4-uLCD-43DT / Diablo16 / ViSi-Genie / Workshop4) and the
controller (Teknic ClearCore + CCIO-8) sides. Lists what we have locally and
what's missing relative to the work remaining (display bring-up, bank flashing,
production hardening).

## Have — display / 4D Systems side (strong coverage)

| Doc | Location | Covers |
|---|---|---|
| DIABLO-16 Processor Datasheet R2.11 | `~/Downloads/DIABLO-16_Processor_Datasheet_R_2_11.pdf` | Chip electrical/pinout, flash banks, RESET behavior |
| DIABLO16 Internal Functions R2.15 | `docs/` (pdf+txt) + Wine Manuals | 4DGL built-in function reference |
| DIABLO16 Serial Command Manual R2.1 | Wine Manuals (pdf+txt) | SPE runtime serial command set (used by spe_smoke.py) |
| ViSi-Genie Reference Manual R3.0 | `docs/` (pdf+txt) | Genie host protocol, object/form model |
| ViSi-Genie Reference Manual R2.5 | Wine Manuals | Older Genie protocol rev |
| ViSi-Genie User Guide | `docs/ViSi-Genie_UG.{pdf,txt}` | Genie environment walkthrough |
| ViSi-Genie Ref (short) | `docs/ViSi-Genie_Ref.{pdf,txt}` | Quick reference |
| Workshop4 User Manual R2.5 | `docs/` (pdf+txt) | IDE: Destination/Bank/uSD, Tools menu, RMPet |
| Workshop4 userguide R2.3 | Wine Manuals | Older IDE guide |
| Workshop4 Widgets Reference R1.2 | Wine Manuals | Widget catalog (USER_LED, LED_DIGITS, Strings…) |
| 4DGL Programmers Reference R6.6 | Wine Manuals | 4DGL language |
| gen4-uLCD-43-70D Series Datasheet R2.2 | `~/Downloads/` | THIS display: FFC pinout, RESET pin 22, programming options |
| gen4-uLCD-xxD Series User Guide | `docs/` (pdf+txt) | Display family overview, Genie Magic |
| 4D ClearCore Adaptor Datasheet R1.3 | `~/Downloads/` | Adapter: J1 jumper, RESET-EN, 5-way header, schematic |
| uUSB-PA5 Datasheet R2.3 | `~/Downloads/` | Backup programmer (uUSB-PA5-II ordered) |
| 4D-AN-00106 First Project | `~/Downloads/4D-AN-00106-FirstProject.zip` | Genie getting-started |
| 4D ClearCore HMI example | `~/Downloads/4D-Systems-ClearCore-HMI-example-2024-07-01.zip` | Official CC+display example (no PmmC flow) |

Wine bundle also has full Goldelox / Picaso / Pixxi manuals at
`~/.wine32/drive_c/users/Public/Documents/4D Labs/4DUpdates/Manuals/` — not
relevant now (those are other chip families) but present if ever needed.

## Have — controller / Teknic side (thin)

| Doc | Location | Covers |
|---|---|---|
| teknic-clearcore-cli README | `~/dev/teknic-clearcore-cli/README.md` | Verified compile/flash workflow, flash.sh, FQBN, Linux gotchas |
| libClearCore inc-doc | `~/dev/cc-extracted/.../libClearCore/inc-doc/ClearCoreOverview.doc` | Library overview (Word doc, not rendered) |

## Missing — relevant to finishing the project

Ranked by how much each blocks remaining work. URLs are the canonical 4D / Teknic
sources; none fetched yet (ask before pulling — outbound).

1. **4D-AN-00036 "How to Update the PmmC for DIABLO-16"** — the official PmmC/driver
   flashing app note. We reverse-engineered a working recipe this session
   (see memory `cc_adapter_diablo16_pmmc_blocked`), but the official procedure
   would confirm the Bank 0 / Bank 5 steps and any timing spec.
   `https://resources.4dsystems.com.au/appnotes/4D-AN-00036/`

2. **App note on "Update Bank(s) and Run" / DIABLO16 multi-bank / RunBankN naming** —
   directly answers the open question: is the Bank 0 stub a *one-time* flash after
   which SD-card swaps suffice, or does each project update need a re-flash? The
   Workshop4 User Manual R2.5 touches this (the push-pipeline workflow is mining it
   now) but there is likely a dedicated app note. Source: 4D appnotes index,
   search "Update Banks and Run" / "Flash Bank".

3. **4dcompiler.exe / SCRIPTC.EXE CLI usage** — NO documentation for headless
   compilation of a .4DGenie. May not be publicly published. (push-pipeline
   workflow is probing the binaries directly.) If unfindable, headless compile is
   not possible and Workshop4 GUI compile stays a manual step.

4. **gen4-IB Interface Board datasheet** — needed for the uUSB-PA5-II backup
   programming path. The gen4 display has no 5-way header; the uUSB-PA5-II needs
   the gen4-IB adapter board to reach the FFC. Confirm wiring before relying on the
   backup path. Source: 4D resources, "gen4-IB".

5. **Teknic ClearCore User Manual (hardware)** — pinout, COM0/COM1 assignment,
   power, I/O electrical specs. We have the *library* docs but not the hardware
   manual. Critical for Phase 1 hardware bring-up (wiring io-table.md to real pins).
   `https://www.teknic.com/files/downloads/clearcore_user_manual.pdf`

6. **Teknic CCIO-8 documentation** — the expansion module that carries
   drainOut/waterOut/airOut/causticOut/pumpOut/sanitizerOut (per io-table.md).
   Initialization + addressing. Needed for Phase 1. Source: Teknic ClearCore
   accessories / CCIO-8.

7. **ClearCore Arduino library API reference (rendered)** — full ConnectorCOM /
   CcioPort / analog API. Available online at `https://teknic-inc.github.io/ClearCore-library/`
   — worth mirroring a local copy for offline bench work.

## Permanent gap (not obtainable)

- **4D PmmC byte-level programming protocol** — confirmed NOT published by 4D
  (prior research; no open-source loader survives for Diablo16). Our substitute is
  the captured successful-flash strace at `tools/pmmc-successful-flash.strace`
  (gitignored, 10 MB) plus the documented manual-RESET recipe.

## Notes

- All large vendor PDFs in `docs/` are gitignored (`docs/*.pdf`); the `.txt`
  extracts are committed for grep-ability. The `~/Downloads` and Wine-bundle PDFs
  are outside the repo.
- When fetching any missing doc, drop the PDF in `~/Downloads` (outside repo) and,
  if useful for grep, add a `pdftotext` extract to `docs/*.txt`.
