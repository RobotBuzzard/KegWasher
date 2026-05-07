// ======================================================================
// KegConfig.cpp - SD config load/save with bounded validation
// ======================================================================
#include "KegConfig.h"
#include "KegUtils.h"
#include "KegDisplay.h"
#include "KegDiagnostics.h"

// Defaults (3 minutes per stage, 1.5x for large kegs)
unsigned long dirtyDrainTimer = 180000UL;
unsigned long dirtyRinseTimer = 180000UL;
unsigned long dirtyPurgeTimer = 180000UL;
unsigned long rinseTimer      = 180000UL;
unsigned long purgeTimer      = 180000UL;
unsigned long washTimer       = 180000UL;
unsigned long saniTimer       = 180000UL;
double        largeKegMod     = 1.5;

static File settingsFile;

// Accept timer values from 30s to 30min. Anything outside that is
// almost certainly a config typo (e.g. forgetting the trailing zeros).
static const unsigned long TIMER_MIN_MS = 30000UL;
static const unsigned long TIMER_MAX_MS = 1800000UL;
static const double KEGMOD_MIN = 1.0;
static const double KEGMOD_MAX = 3.0;

static bool applyTimerKey(const char* key, const char* value, unsigned long& target) {
  unsigned long v = strtoul(value, nullptr, 10);
  if (v < TIMER_MIN_MS || v > TIMER_MAX_MS) {
    diagnostics_logEvent((String("Config out of range: ") + key + "=" + value).c_str());
    return false;
  }
  target = v;
  return true;
}

void config_init() {
  analogReadResolution(adcResolution);

  if (!config_loadFromSD()) {
    config_setDefaults();
    display_showError("Using default settings");
    delay(1500);
  }
}

bool config_loadFromSD() {
  if (!SD.begin()) {
    diagnostics_logEvent("SD init failed");
    display_showError("SD card init failed");
    delay(1500);
    return false;
  }

  settingsFile = SD.open(settingsFileName, FILE_READ);
  if (!settingsFile) {
    diagnostics_logEvent("Config file not found");
    display_showError("Config file missing");
    delay(1500);
    return false;
  }

  char key[KEY_MAX_LENGTH] = {0};
  char value[VALUE_MAX_LENGTH] = {0};
  char line[KEY_MAX_LENGTH + VALUE_MAX_LENGTH + 2] = {0};

  while (settingsFile.available()) {
    size_t i = 0;
    while (settingsFile.available() && i < sizeof(line) - 1) {
      char c = settingsFile.read();
      if (c == '\n') break;
      line[i++] = c;
    }
    line[i] = '\0';

    if (!utils_parseConfigLine(line, key, value)) continue;

    if      (strcmp(key, "dirtyDrainTimer") == 0) applyTimerKey(key, value, dirtyDrainTimer);
    else if (strcmp(key, "dirtyRinseTimer") == 0) applyTimerKey(key, value, dirtyRinseTimer);
    else if (strcmp(key, "dirtyPurgeTimer") == 0) applyTimerKey(key, value, dirtyPurgeTimer);
    else if (strcmp(key, "rinseTimer")      == 0) applyTimerKey(key, value, rinseTimer);
    else if (strcmp(key, "purgeTimer")      == 0) applyTimerKey(key, value, purgeTimer);
    else if (strcmp(key, "washTimer")       == 0) applyTimerKey(key, value, washTimer);
    else if (strcmp(key, "saniTimer")       == 0) applyTimerKey(key, value, saniTimer);
    else if (strcmp(key, "largeKegMod")     == 0) {
      double v = atof(value);
      if (v < KEGMOD_MIN || v > KEGMOD_MAX) {
        diagnostics_logEvent((String("Config out of range: largeKegMod=") + value).c_str());
      } else {
        largeKegMod = v;
      }
    }
    // Unknown keys silently ignored — forward-compat with newer config files.
  }

  settingsFile.close();
  return true;
}

void config_saveToSD() {
  if (SD.exists(settingsFileName)) SD.remove(settingsFileName);

  settingsFile = SD.open(settingsFileName, FILE_WRITE);
  if (!settingsFile) {
    display_showError("Cannot create config");
    delay(1500);
    return;
  }

  settingsFile.print("dirtyDrainTimer="); settingsFile.println(dirtyDrainTimer);
  settingsFile.print("dirtyRinseTimer="); settingsFile.println(dirtyRinseTimer);
  settingsFile.print("dirtyPurgeTimer="); settingsFile.println(dirtyPurgeTimer);
  settingsFile.print("rinseTimer=");      settingsFile.println(rinseTimer);
  settingsFile.print("purgeTimer=");      settingsFile.println(purgeTimer);
  settingsFile.print("washTimer=");       settingsFile.println(washTimer);
  settingsFile.print("saniTimer=");       settingsFile.println(saniTimer);
  settingsFile.print("largeKegMod=");     settingsFile.println(largeKegMod);

  settingsFile.close();
}

void config_setDefaults() {
  dirtyDrainTimer = 180000UL;
  dirtyRinseTimer = 180000UL;
  dirtyPurgeTimer = 180000UL;
  rinseTimer      = 180000UL;
  purgeTimer      = 180000UL;
  washTimer       = 180000UL;
  saniTimer       = 180000UL;
  largeKegMod     = 1.5;
}
