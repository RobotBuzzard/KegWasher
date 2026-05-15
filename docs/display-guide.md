# Display Guide

A practical reference for working with the Goldelox 128×128 display in this firmware. Covers the hardware path, the wrapper API, the non-obvious quirks, and how to add a new screen without re-learning them.

## Hardware

| Property | Value |
|---|---|
| Module | 4D Systems Goldelox 128×128, portrait orientation |
| Connection | ClearCore COM1 (`Serial1` in Arduino terms), TTL |
| Baud | 9600 at boot, negotiated up to **115200** in `display_init()` |
| Reset line | COM1 **RTS pin** drives the Goldelox RESET input |
| Library | `Goldelox-Serial-Arduino-Library` (vendored under `~/Arduino/libraries/`) |
| Driver wrapper | `KegDisplay.h` / `KegDisplay.cpp` |

The RTS-as-reset wiring is the single most important thing to know about this display. Any code path that lets RTS float will reset the display.

## Boot sequence

`display_init()` does, in order:

1. `Serial1.begin(9600)` — Goldelox always boots at 9600.
2. RTS pulse via `ConnectorCOM1.RtsMode(LINE_ON)` → `delay(1000)` → `RtsMode(LINE_OFF)` → `delay(3000)`. The 3 s post-reset delay is the Goldelox firmware boot time.
3. `Display.setbaudWait(BAUD_115200)` — negotiates the link up to 115200 (drops full-screen redraw from ~400 ms to ~35 ms).
4. `gfx_ScreenMode(2)` → portrait orientation, then screensaver disables, black background, clear screen.

Total init time is dominated by the 4 s of RTS/boot delays. The watchdog is not yet armed during this window — see `KegWasher.ino` `setup()` for ordering.

### The baud-bump pitfall (do not "fix")

The library's stock `setbaudWait()` would call `Serial1.end()` → `SerialBase::PortClose()`, which switches the RTS pin from OUTPUT to INPUT. The moment RTS floats, the Goldelox sees a reset assertion and reboots back to 9600 while the MCU has already moved to 115200 — silent baud mismatch, dead display, ~24 s of stacked 2 s timeouts in `setup()` before anything else can come up.

The fix is a custom MCU-side baud-rate callback (`displaySetBaudRate()` in `KegDisplay.cpp`) that uses `ConnectorCOM1.Speed(newRate)` — which calls `PortDisable()` (peripheral-only) instead of `PortClose()` (peripheral + RTS-as-input), so RTS stays driven. The callback is registered via the Goldelox library's two-arg constructor.

**If you change the baud or otherwise mess with COM1**, route through `displaySetBaudRate()` or recreate its protections. Don't call `Serial1.end()`/`begin()` on a running display.

## The print API

Three layers, from low to high:

| Function | Purpose |
|---|---|
| `Display.<anything>` | Raw Goldelox library. Use only for graphics primitives (`gfx_RectangleFilled`, `txt_MoveCursor`, `txt_FGcolour`, etc.) and for positioned overlays. **Don't `print`/`println` directly** — see next row. |
| `display_println(const char* s)` / `display_println()` | The text-output workhorse. Prepends a sacrificial leading space to dodge the Goldelox first-char-after-CR/LF drop, then `Display.println()`s the body. |
| `display_showStartup()`, `display_showError()`, `display_showMessage()`, `display_showProgress()`, `display_update()` | Whole-screen renders. Each clears, prints its content, then calls `display_footer()`. |

### Why the sacrificial space

The Goldelox firmware drops the first character following a `\r\n` while the internal cursor is advancing. We work around it by prepending a single space to every line via `display_println()`. The cosmetic cost is one column of left margin on every screen; the alternative is dropped letters at the start of every line.

**Corollary**: do **not** embed `\n` inside a `display_println("foo\nbar")` call — the `bar` half loses its sacrificial-space protection. Split into two calls instead. (See `display_showError` for the canonical example.)

## Screen layout

The default Goldelox font cell is **~8 × 12 px** in this build, giving roughly a **16-column × 10-row** usable grid on the 128×128 panel.

```
+----------------+   col 0 ..... col 15
| Robot Keg Wash |    row 0   (titles use ~14-16 chars)
| ** BENCH MODE  |    row 1
|                |    row 2   (blank spacer)
| Press START    |    row 3   (per-state body)
| to begin       |    row 4
|                |    row 5..8   (free space — body extends here on
|                |               longer screens like display_update)
| 192.168.1.92   |    row 9   (footer: IP or "offline")
|             M* |    row 9-10 (status icons, columns 14-15 reserved)
+----------------+
```

### Footer

`display_footer()` is called as the **last** step of every persistent render. It emits:

- Either the device's local IP (`%u.%u.%u.%u`) or `offline` when Ethernet hasn't acquired a lease.
- Then overlays status icons at the right edge of the same row via `txt_MoveCursor(9, 14)` + `txt_FGcolour(VIOLET)` + `putstr("M*")`.

Reserved icon slots (cells 14-15 of row 9-10):

| Icon | Color | Meaning |
|---|---|---|
| `M*` | VIOLET (`0xEC1D`) | MQTT broker connection is live (`kwMqttReady == true`) |
| `H*` | (TBD) | HTTP-client-connected indicator — slot reserved but currently unused; HTTP server is no longer planned |

**Color gotcha**: the named constant `PURPLE` (`0x8010`) is half-brightness RGB565 — visually indistinguishable from background on this panel through the green acrylic enclosure on camera. Use `VIOLET`, `MAGENTA`, or a custom bright hex if you want "purple" to read as purple. See `KegDisplay.cpp` `display_footer()` for the precedent.

## Update cadence

The status panel (`display_update()`) refreshes every **1 000 ms** in `loop()`. Previously was 250 ms; we slowed it after observing partial-frame redraws on the 9600-baud link. At 115200 we could push the rate higher but there's no operator benefit — temps and timers tick at sub-second granularity already.

The Goldelox library is **synchronous** — every `Display.<call>` blocks until the chip ACKs. A full `display_update()` at 115200 is ~35 ms of blocked-loop time. Don't put a display call in any path that needs to stay under tight timing (ISRs especially — the ESTOP ISR is bare-metal for exactly this reason).

## The render-once pattern

Three screens use a `static bool drawn = false` guard to render exactly once on state entry, instead of redrawing every loop tick:

| State | Why |
|---|---|
| `STARTUP_READY` | Operator may wait minutes before pressing Start. Continuous redraw would burn UART cycles and produce visible flicker. |
| `STATE_FINISHED` | Same idea — operator-paced. |
| `STATE_ERROR` | The error display also silences the audible alarm on manual drain; redraw would re-fire those side effects. |

When a screen needs to re-render after a transition out and back, reset `drawn = false` on the transition path. See `STARTUP_READY` in `KegStateMachine.cpp` for the pattern.

`display_update()` itself doesn't need this guard — the 1 Hz refresh in `loop()` rate-limits it naturally, and the content (timers, sensor readings) is meant to change every refresh anyway.

## Adding a new screen

1. **Add a function** to `KegDisplay.cpp` — by convention, `display_showXyz(...)`.
2. **Start with `display_clear()`** to wipe the previous frame.
3. **Use `display_println(...)`** for every text line, not `Display.println()`.
4. **Don't embed `\n` mid-string** — call `display_println()` multiple times.
5. **Call `display_footer()` last** so every screen shows the IP + M\* indicator consistently.
6. **Declare the function** in `KegDisplay.h`.

If the screen is operator-paced (no auto-refresh required), invoke it through the render-once pattern from the state handler so the Goldelox isn't getting hammered with redundant frames.

## Failure modes

| Symptom | Likely cause | Recovery |
|---|---|---|
| Blank/dark screen after a code change | Baud mismatch between MCU and display — usually a `Serial1.end()` or `Serial1.begin()` somewhere new floated RTS. | Power-cycle the ClearCore, then route the offending baud change through `displaySetBaudRate()`. |
| Partial-frame redraws / flicker | Display update cadence faster than the link can complete a full frame. | Increase `DISPLAY_INTERVAL_MS` in `KegWasher.ino` or move per-state screens into the render-once pattern. |
| First character of a line missing | Bypass of the sacrificial-space wrapper (raw `Display.println()` or embedded `\n` in a single `display_println` call). | Use `display_println()` and split embedded newlines. |
| Display goes completely dead mid-cycle | Goldelox library is synchronous + RTS-driven reset — most likely cause is a watchdog reset on the ClearCore side. Display will recover next boot via the `display_init()` reset pulse. | None needed; verify nothing in the loop blocks for >8 s. |

## See also

- `KegDisplay.h` / `KegDisplay.cpp` — implementation
- `docs/io-table.md` — pin mapping for COM1 / RTS
- `~/.claude/projects/-home-buzz-dev/memory/clearcore_uart_baud_change.md` — the RTS-floats-on-PortClose gotcha in detail
- `~/Arduino/libraries/Goldelox-Serial-Arduino-Library/src/Goldelox_Serial_4DLib.{h,cpp}` — the underlying library
- `~/.arduino15/packages/ClearCore/hardware/sam/1.7.1/Teknic/libClearCore/src/SerialBase.cpp` — the ClearCore SERCOM driver, where the RTS-as-input transition lives at line 125
