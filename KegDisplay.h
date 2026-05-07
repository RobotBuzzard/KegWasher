// ======================================================================
// KegDisplay.h - 4D Systems Goldelox 128x128 wrapper (serial on COM1)
// ======================================================================
#ifndef KEG_DISPLAY_H
#define KEG_DISPLAY_H

#include <Arduino.h>
#include "KegConfig.h"
#include "Goldelox_Serial_4DLib.h"
#include "Goldelox_Const4D.h"

extern Goldelox_Serial_4DLib Display;

// Wrappers around Display.println that work around a Goldelox quirk:
// the first byte after a CR/LF sequence is occasionally dropped while
// the display is advancing the cursor. These prefix a sacrificial
// space so the visible text isn't truncated. Use these instead of
// Display.println() for any user-facing line.
void display_println(const char* s);
void display_println();

void display_init();
void display_showStartup();
void display_update();
void display_showState(byte state);
void display_showError(const char* errorMsg);
void display_showMessage(const char* message);
void display_showProgress(const char* label, int percentage,
                          int currentValue, int targetValue,
                          int remainingMinutes = -1);
void display_showTimer(unsigned long elapsedTime);
void display_showStatus(bool waterStatus, bool airStatus, bool co2Status,
                        bool estopStatus, int causticTemp);
void display_clear();

#endif // KEG_DISPLAY_H
