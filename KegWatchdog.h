// ======================================================================
// KegWatchdog.h - SAMD51 hardware watchdog timer wrapper
// ======================================================================
// Teknic's ClearCore Arduino library does NOT expose any watchdog API
// (verified against teknic-inc.github.io/ClearCore-library/ — the
// SysManager class has only ConnectorByIndex() and ResetBoard()). So
// we go direct to the SAME53 WDT register block via the CMSIS headers
// that Arduino.h already pulls in.
//
// Behavior: when enabled, the WDT must be kicked (watchdog_kick()) at
// least every 8 seconds or the chip resets via the bootloader, which
// then jumps straight back to the application. Net effect of any hang
// >8s = a fresh boot sequence. The display, sensors, and state machine
// all re-init cleanly; the system enters STARTUP and re-checks
// hardware_allSystemsGo().
// ======================================================================
#ifndef KEG_WATCHDOG_H
#define KEG_WATCHDOG_H

#include <Arduino.h>

// Enable the WDT with an 8-second timeout. Idempotent — safe to call
// repeatedly. Reads the current state and reconfigures cleanly.
//
// CALL ORDER: must be called AFTER any setup-time delays longer than
// the timeout (display_init's 4s RTS reset window, config_init's SD-
// fail-message delays, etc.). The convention in this project is to
// call it as the last statement in setup().
void watchdog_enable();

// Disable the WDT. Wrap any deliberately-blocking code that may exceed
// the timeout (currently: diagnostics_runTest's ~10s of output exercises
// + the button-release wait). Re-enable with watchdog_enable() on exit.
void watchdog_disable();

// Kick the watchdog. Call once per loop() iteration, ideally as the
// last statement before delay(). NOT safe to call from an ISR — the
// SYNCBUSY wait may not complete under interrupt context.
void watchdog_kick();

// Whether the watchdog is currently enabled (introspection helper for
// modules that want to bracket their own long-blocking sections).
bool watchdog_isEnabled();

// Returns the latched RSTC.RCAUSE register from the most recent reset.
// Useful for postmortem ("did this thing reboot itself?"). Cached on
// first call so any later read still gives the original boot cause.
// Bits per SAME53 datasheet (note: the chip's reset controller is
// named RSTC, not RSTCTRL as on some other SAMD-family parts):
//   0x01 POR     0x10 EXT (external pin / bossac SYSRESETREQ-equiv)
//   0x02 BODCORE 0x20 WDT     ← what we're looking for after a hang
//   0x04 BODVDD  0x40 SYST    (Cortex-M software reset)
//   0x08 NVM     0x80 BACKUP
byte watchdog_lastResetCause();

#endif // KEG_WATCHDOG_H
