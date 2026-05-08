// ======================================================================
// KegWatchdog.cpp
// ======================================================================
// SAMD51 watchdog timer via direct CMSIS register access.
//
// Clock source: OSC32KCTRL's CLK_OSC32K_1K (1.024 kHz tap from the
// always-on OSCULP32K oscillator). On SAMD51 this is wired internally
// to the WDT — unlike SAMD21, NO GCLK channel needs to be configured
// for the WDT. The 1k tap is enabled by default at reset; we set
// EN1K explicitly for safety.
//
// Period: WDT_CONFIG_PER_CYC8192 = 8192 cycles at 1024 Hz ≈ 8 sec.
//
// Safety: the WDT->CLEAR register only accepts the magic key 0xA5
// (WDT_CLEAR_CLEAR_KEY). Writing anything else triggers an immediate
// reset. Always wait on SYNCBUSY.CLEAR before writing, and never call
// watchdog_kick() from an ISR.
// ======================================================================
#include "KegWatchdog.h"
// sam.h is pulled in transitively by Arduino.h and provides WDT,
// OSC32KCTRL, MCLK pointers + the WDT_* / OSC32KCTRL_* constants.

void watchdog_enable() {
  // Make sure the 1 kHz tap from OSCULP32K is on. Default-on at reset
  // but be explicit — something else (low-power mode, etc.) might
  // have disabled it.
  OSC32KCTRL->OSCULP32K.bit.EN1K = 1;

  // Ensure APBA bus clock to the WDT peripheral is enabled. Default-on
  // at reset; defensive write.
  MCLK->APBAMASK.bit.WDT_ = 1;

  // If already running, disable first so we can reconfigure.
  if (WDT->CTRLA.bit.ENABLE) {
    WDT->CTRLA.bit.ENABLE = 0;
    while (WDT->SYNCBUSY.reg) { /* wait for ENABLE=0 to sync */ }
  }

  // Disable early-warning interrupt — we don't use it. Could be wired
  // later to log "about to die" messages via Serial before reset.
  WDT->INTENCLR.bit.EW = 1;

  // 8-second timeout (8192 cycles at 1024 Hz)
  WDT->CONFIG.reg = WDT_CONFIG_PER_CYC8192;

  // Enable
  WDT->CTRLA.reg = WDT_CTRLA_ENABLE;
  while (WDT->SYNCBUSY.reg) { /* wait for ENABLE=1 to sync */ }
}

void watchdog_disable() {
  if (!WDT->CTRLA.bit.ENABLE) return;
  WDT->CTRLA.bit.ENABLE = 0;
  while (WDT->SYNCBUSY.reg) { /* wait */ }
}

void watchdog_kick() {
  // Wait for any in-flight CLEAR sync to finish — writing the key
  // before the previous clear has settled triggers an immediate reset.
  while (WDT->SYNCBUSY.bit.CLEAR) { /* wait */ }
  WDT->CLEAR.reg = WDT_CLEAR_CLEAR_KEY;
}

bool watchdog_isEnabled() {
  return WDT->CTRLA.bit.ENABLE != 0;
}

byte watchdog_lastResetCause() {
  // SAME53 calls the reset controller RSTC (the SAMD-family generic
  // name is RSTCTRL — different on this chip).
  static byte cached = 0;
  static bool cached_valid = false;
  if (!cached_valid) {
    cached = RSTC->RCAUSE.reg;
    cached_valid = true;
  }
  return cached;
}
