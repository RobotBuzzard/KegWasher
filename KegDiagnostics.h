// ======================================================================
// KegDiagnostics.h - Diagnostic mode, logging, error message lookup
// ======================================================================
// Error code definitions live in KegConfig.h (single source of truth).
// This module is the consumer.
// ======================================================================
#ifndef KEG_DIAGNOSTICS_H
#define KEG_DIAGNOSTICS_H

#include <Arduino.h>
#include "KegConfig.h"

extern bool diagnosticMode;
extern byte errorCode;

void diagnostics_init();
void diagnostics_process();
void diagnostics_runTest();
void diagnostics_logEvent(const char* eventMsg);
const char* diagnostics_getErrorMessage(byte code);

#endif // KEG_DIAGNOSTICS_H
