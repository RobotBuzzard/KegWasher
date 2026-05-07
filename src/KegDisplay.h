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
