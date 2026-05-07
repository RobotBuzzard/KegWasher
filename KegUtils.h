// ======================================================================
// KegUtils.h - Time formatting + config line parsing
// ======================================================================
#ifndef KEG_UTILS_H
#define KEG_UTILS_H

#include <Arduino.h>
#include "KegConfig.h"

unsigned long utils_millisToSeconds(unsigned long ms);
unsigned long utils_secondsToMillis(unsigned long seconds);
const char*   utils_formatTime(unsigned long timeInMillis);
bool          utils_parseConfigLine(const char* line, char* key, char* value);

#endif // KEG_UTILS_H
