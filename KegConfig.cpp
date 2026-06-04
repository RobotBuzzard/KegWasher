// ======================================================================
// KegConfig.cpp - SD config load/save with bounded validation
// ======================================================================
#include "KegConfig.h"
#include "KegUtils.h"
#include "KegDisplay.h"
#include "KegDiagnostics.h"

// Default cycle timer.
//   Production: 3 min per stage (15 min full cycle).
//   BENCH_MODE: 5 sec per stage (~30 sec full cycle), so bench demos
//   can exercise the entire state machine in well under a minute.
//   The validation bounds [30s, 30min] still apply to SD-loaded values
//   — the override only affects compiled defaults.
#ifdef BENCH_MODE
  #define DEFAULT_STAGE_MS 5000UL
#else
  #define DEFAULT_STAGE_MS 180000UL
#endif

// Stage durations, in cycle order. See KegConfig.h for the stage each drives.
unsigned long dirtyDrainTimer = DEFAULT_STAGE_MS;
unsigned long dirtyRinseTimer = DEFAULT_STAGE_MS;
unsigned long dirtyPurgeTimer = DEFAULT_STAGE_MS;
unsigned long washTimer       = DEFAULT_STAGE_MS;
unsigned long causticRtnTimer = DEFAULT_STAGE_MS;
unsigned long rinseTimer      = DEFAULT_STAGE_MS;
unsigned long rinsePurgeTimer = DEFAULT_STAGE_MS;
unsigned long saniTimer       = DEFAULT_STAGE_MS;
unsigned long saniRtnTimer    = DEFAULT_STAGE_MS;
unsigned long purgeTimer      = DEFAULT_STAGE_MS;
double        largeKegMod     = 1.5;

// Touch calibration (host-side affine: screen=[a b c; d e f].[rawx rawy 1]).
// KegDisplaySerial has its own baked defaults; these only override when the SD
// config supplies touchCalA..F (which sets cfgTouchCalValid → display_init applies).
float cfgTouchCal[6]  = {1, 0, 0, 0, 1, 0};
bool  cfgTouchCalValid = false;

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
    else if (strcmp(key, "washTimer")       == 0) applyTimerKey(key, value, washTimer);
    else if (strcmp(key, "causticRtnTimer") == 0) applyTimerKey(key, value, causticRtnTimer);
    else if (strcmp(key, "rinseTimer")      == 0) applyTimerKey(key, value, rinseTimer);
    else if (strcmp(key, "rinsePurgeTimer") == 0) applyTimerKey(key, value, rinsePurgeTimer);
    else if (strcmp(key, "saniTimer")       == 0) applyTimerKey(key, value, saniTimer);
    else if (strcmp(key, "saniRtnTimer")    == 0) applyTimerKey(key, value, saniRtnTimer);
    else if (strcmp(key, "purgeTimer")      == 0) applyTimerKey(key, value, purgeTimer);
    else if (strcmp(key, "largeKegMod")     == 0) {
      double v = atof(value);
      if (v < KEGMOD_MIN || v > KEGMOD_MAX) {
        diagnostics_logEvent((String("Config out of range: largeKegMod=") + value).c_str());
      } else {
        largeKegMod = v;
      }
    }
    else if (strncmp(key, "touchCal", 8) == 0 && key[8] >= 'A' && key[8] <= 'F' && key[9] == '\0') {
      cfgTouchCal[key[8] - 'A'] = atof(value);   // touchCalA..F -> affine coeffs
      cfgTouchCalValid = true;
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
  settingsFile.print("washTimer=");       settingsFile.println(washTimer);
  settingsFile.print("causticRtnTimer="); settingsFile.println(causticRtnTimer);
  settingsFile.print("rinseTimer=");      settingsFile.println(rinseTimer);
  settingsFile.print("rinsePurgeTimer="); settingsFile.println(rinsePurgeTimer);
  settingsFile.print("saniTimer=");       settingsFile.println(saniTimer);
  settingsFile.print("saniRtnTimer=");    settingsFile.println(saniRtnTimer);
  settingsFile.print("purgeTimer=");      settingsFile.println(purgeTimer);
  settingsFile.print("largeKegMod=");     settingsFile.println(largeKegMod);

  settingsFile.close();
}

void config_setDefaults() {
  dirtyDrainTimer = DEFAULT_STAGE_MS;
  dirtyRinseTimer = DEFAULT_STAGE_MS;
  dirtyPurgeTimer = DEFAULT_STAGE_MS;
  washTimer       = DEFAULT_STAGE_MS;
  causticRtnTimer = DEFAULT_STAGE_MS;
  rinseTimer      = DEFAULT_STAGE_MS;
  rinsePurgeTimer = DEFAULT_STAGE_MS;
  saniTimer       = DEFAULT_STAGE_MS;
  saniRtnTimer    = DEFAULT_STAGE_MS;
  purgeTimer      = DEFAULT_STAGE_MS;
  largeKegMod     = 1.5;
}
