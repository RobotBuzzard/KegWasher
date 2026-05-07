// ======================================================================
// KegUtils.cpp
// ======================================================================
#include "KegUtils.h"
#include <string.h>
#include <stdio.h>

unsigned long utils_millisToSeconds(unsigned long ms) {
  return ms / 1000;
}

unsigned long utils_secondsToMillis(unsigned long seconds) {
  return seconds * 1000;
}

const char* utils_formatTime(unsigned long timeInMillis) {
  static char buffer[10];   // MM:SS + null
  unsigned long seconds = timeInMillis / 1000;
  unsigned long minutes = seconds / 60;
  seconds %= 60;
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu", minutes, seconds);
  return buffer;
}

bool utils_parseConfigLine(const char* line, char* key, char* value) {
  if (line[0] == '#' || line[0] == '\0' || line[0] == '\r' || line[0] == '\n') {
    return false;
  }

  const char* equals = strchr(line, '=');
  if (!equals) return false;

  size_t keyLen = equals - line;
  size_t valueLen = strlen(equals + 1);
  if (keyLen == 0 || keyLen >= KEY_MAX_LENGTH || valueLen >= VALUE_MAX_LENGTH) {
    return false;
  }

  strncpy(key, line, keyLen);
  key[keyLen] = '\0';

  strncpy(value, equals + 1, VALUE_MAX_LENGTH - 1);
  value[VALUE_MAX_LENGTH - 1] = '\0';

  // Trim trailing whitespace from the value
  char* end = value + strlen(value) - 1;
  while (end > value && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
    *end-- = '\0';
  }
  return true;
}
