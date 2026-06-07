// ======================================================================
// KegConfig.cpp - SD config load/save with bounded validation
// ======================================================================
#include "KegConfig.h"
#include "KegUtils.h"
#include "KegDisplay.h"
#include "KegDiagnostics.h"
#include "KegSecrets.h"   // compiled MQTT broker/cred defaults (seed the runtime globals)

// Stringize the 4 broker-IP octet macros into a "a.b.c.d" literal for the
// compile-time default of mqttBrokerIp (static init can't run snprintf).
#define KW_STR2(x) #x
#define KW_STR(x)  KW_STR2(x)

// Per-stage default durations (ms). The washer is a TWO-HEAD system — one cycle
// cleans 2 kegs in parallel — so these are per-cycle (both kegs), not per keg.
// Production defaults total ≈ 4.4 min/cycle; tune per process via WASHER.CFG or
// the on-screen settings editor. BENCH_MODE compresses every stage to 5 s so a
// cardless bench cycle runs in ~50 s. (5 s multiples → clean ±5 s editor steps.)
#ifdef BENCH_MODE
  #define DEF_DIRTY_DRAIN_MS  5000UL
  #define DEF_DIRTY_RINSE_MS  5000UL
  #define DEF_DIRTY_PURGE_MS  5000UL
  #define DEF_WASH_MS         5000UL
  #define DEF_CAUSTIC_RTN_MS  5000UL
  #define DEF_RINSE_MS        5000UL
  #define DEF_RINSE_PURGE_MS  5000UL
  #define DEF_SANI_MS         5000UL
  #define DEF_SANI_RTN_MS     5000UL
  #define DEF_PRESSURE_MS     5000UL
#else
  #define DEF_DIRTY_DRAIN_MS  20000UL   // drain old beer (+5 s air burst)
  #define DEF_DIRTY_RINSE_MS  20000UL   // pre-rinse
  #define DEF_DIRTY_PURGE_MS  10000UL   // air blow to drain
  #define DEF_WASH_MS         90000UL   // caustic recirc — the real clean
  #define DEF_CAUSTIC_RTN_MS  15000UL   // air-blow caustic back to tank
  #define DEF_RINSE_MS        25000UL   // post-caustic rinse
  #define DEF_RINSE_PURGE_MS  10000UL   // air blow to drain
  #define DEF_SANI_MS         45000UL   // sanitizer recirc
  #define DEF_SANI_RTN_MS     15000UL   // CO2-blow sani back to tank
  #define DEF_PRESSURE_MS     15000UL   // CO2 charge / seal
#endif

// Stage durations, in cycle order. See KegConfig.h for the stage each drives.
unsigned long dirtyDrainTimer = DEF_DIRTY_DRAIN_MS;
unsigned long dirtyRinseTimer = DEF_DIRTY_RINSE_MS;
unsigned long dirtyPurgeTimer = DEF_DIRTY_PURGE_MS;
unsigned long washTimer       = DEF_WASH_MS;
unsigned long causticRtnTimer = DEF_CAUSTIC_RTN_MS;
unsigned long rinseTimer      = DEF_RINSE_MS;
unsigned long rinsePurgeTimer = DEF_RINSE_PURGE_MS;
unsigned long saniTimer       = DEF_SANI_MS;
unsigned long saniRtnTimer    = DEF_SANI_RTN_MS;
unsigned long purgeTimer      = DEF_PRESSURE_MS;
double        largeKegMod     = 1.5;
unsigned long pauseMaxMs      = PAUSE_MAX_MS;   // runtime; SD-overridable (Phase 3)

// Temperature thresholds (°C) — seeded from DEFAULT_*; SD-overridable.
int minCausticTemp     = DEFAULT_MIN_CAUSTIC_TEMP;
int optimalCausticTemp = DEFAULT_OPTIMAL_CAUSTIC_TEMP;
int maxCausticTemp     = DEFAULT_MAX_CAUSTIC_TEMP;
int maxEnclosureTemp   = DEFAULT_MAX_ENCLOSURE_TEMP;
int fanOnTemp          = DEFAULT_FAN_ON_TEMP;
int fanOffTemp         = DEFAULT_FAN_OFF_TEMP;

// MQTT broker config — seeded from KegSecrets.h; SD-overridable per device.
char mqttBrokerIp[16]  = KW_STR(MQTT_BROKER_IP_0) "." KW_STR(MQTT_BROKER_IP_1) "."
                         KW_STR(MQTT_BROKER_IP_2) "." KW_STR(MQTT_BROKER_IP_3);
int  mqttBrokerPort    = MQTT_BROKER_PORT;
char mqttUser[32]      = MQTT_USER;
char mqttPass[32]      = MQTT_PASS;
char mqttClientId[32]  = MQTT_CLIENT_ID;
char mqttTopicRoot[32] = MQTT_TOPIC_ROOT;

// Touch calibration (host-side affine: screen=[a b c; d e f].[rawx rawy 1]).
// KegDisplaySerial has its own baked defaults; these only override when the SD
// config supplies touchCalA..F (which sets cfgTouchCalValid → display_init applies).
float cfgTouchCal[6]  = {1, 0, 0, 0, 1, 0};
bool  cfgTouchCalValid = false;
bool  cfgLoadedFromSD  = false;   // set by config_init(); drives the boot-screen SD line

static File settingsFile;

// Accept timer values from 5s to 30min. The 5s floor allows the genuinely short
// purge/return/charge stages (air blow-down, CO2 charge) while still rejecting
// obvious typos (e.g. a forgotten trailing zero / a 0-length stage).
static const unsigned long TIMER_MIN_MS = 5000UL;
static const unsigned long TIMER_MAX_MS = 1800000UL;
static const double KEGMOD_MIN = 1.0;
static const double KEGMOD_MAX = 3.0;
static const int  TEMP_MIN_C   = 0;        // plausible threshold range (°C)
static const int  TEMP_MAX_C   = 120;
static const unsigned long PAUSE_MIN_MS   = 60000UL;       // 1 min
static const unsigned long PAUSE_LIMIT_MS = 3600000UL;     // 60 min

static void logOutOfRange(const char* key, const char* value) {
  diagnostics_logEvent((String("Config out of range: ") + key + "=" + value).c_str());
}

static bool applyTimerKey(const char* key, const char* value, unsigned long& target) {
  unsigned long v = strtoul(value, nullptr, 10);
  if (v < TIMER_MIN_MS || v > TIMER_MAX_MS) { logOutOfRange(key, value); return false; }
  target = v;
  return true;
}

// Bounded integer (temperature thresholds).
static bool applyTempKey(const char* key, const char* value, int& target) {
  int v = atoi(value);
  if (v < TEMP_MIN_C || v > TEMP_MAX_C) { logOutOfRange(key, value); return false; }
  target = v;
  return true;
}

// Free-form string value → fixed buffer (snprintf null-terminates + truncates).
static void applyStrKey(const char* value, char* target, size_t cap) {
  snprintf(target, cap, "%s", value);
}

void config_init() {
  analogReadResolution(adcResolution);

  if (config_loadFromSD()) {
    cfgLoadedFromSD = true;
  } else {
    cfgLoadedFromSD = false;
    config_setDefaults();
    // The boot summary screen reports "SD CONFIG: DEFAULT"; no separate banner.
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
        logOutOfRange(key, value);
      } else {
        largeKegMod = v;
      }
    }
    else if (strcmp(key, "pauseMaxMs")      == 0) {
      unsigned long v = strtoul(value, nullptr, 10);
      if (v < PAUSE_MIN_MS || v > PAUSE_LIMIT_MS) logOutOfRange(key, value);
      else pauseMaxMs = v;
    }
    // Temperature thresholds (°C)
    else if (strcmp(key, "minCausticTemp")     == 0) applyTempKey(key, value, minCausticTemp);
    else if (strcmp(key, "optimalCausticTemp") == 0) applyTempKey(key, value, optimalCausticTemp);
    else if (strcmp(key, "maxCausticTemp")     == 0) applyTempKey(key, value, maxCausticTemp);
    else if (strcmp(key, "maxEnclosureTemp")   == 0) applyTempKey(key, value, maxEnclosureTemp);
    else if (strcmp(key, "fanOnTemp")          == 0) applyTempKey(key, value, fanOnTemp);
    else if (strcmp(key, "fanOffTemp")         == 0) applyTempKey(key, value, fanOffTemp);
    // MQTT broker config (override the KegSecrets.h compiled defaults)
    else if (strcmp(key, "mqttBrokerIp")    == 0) applyStrKey(value, mqttBrokerIp,  sizeof(mqttBrokerIp));
    else if (strcmp(key, "mqttBrokerPort")  == 0) {
      long p = strtol(value, nullptr, 10);
      if (p < 1 || p > 65535) logOutOfRange(key, value);
      else mqttBrokerPort = (int)p;
    }
    else if (strcmp(key, "mqttUser")        == 0) applyStrKey(value, mqttUser,      sizeof(mqttUser));
    else if (strcmp(key, "mqttPass")        == 0) applyStrKey(value, mqttPass,      sizeof(mqttPass));
    else if (strcmp(key, "mqttClientId")    == 0) applyStrKey(value, mqttClientId,  sizeof(mqttClientId));
    else if (strcmp(key, "mqttTopicRoot")   == 0) applyStrKey(value, mqttTopicRoot, sizeof(mqttTopicRoot));
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
  settingsFile.print("pauseMaxMs=");      settingsFile.println(pauseMaxMs);

  // Temperature thresholds
  settingsFile.print("minCausticTemp=");     settingsFile.println(minCausticTemp);
  settingsFile.print("optimalCausticTemp="); settingsFile.println(optimalCausticTemp);
  settingsFile.print("maxCausticTemp=");     settingsFile.println(maxCausticTemp);
  settingsFile.print("maxEnclosureTemp=");   settingsFile.println(maxEnclosureTemp);
  settingsFile.print("fanOnTemp=");          settingsFile.println(fanOnTemp);
  settingsFile.print("fanOffTemp=");         settingsFile.println(fanOffTemp);

  // NOTE: the MQTT broker block is intentionally NOT written here. This save does
  // SD.remove + full rewrite, so it INTENTIONALLY drops any MQTT block from the
  // file — the device then falls back to compiled KegSecrets.h creds on next boot.
  // Rationale: the on-device editor only tunes timers/thresholds; previously it
  // rewrote MQTT from RAM, which let a save clobber the broker creds with stale/
  // placeholder values (and wrote the password to the card on every timer edit).
  // Consequence: don't rely on SD-only MQTT creds AND the on-device editor on the
  // same unit — provision MQTT via KegSecrets.h (or the future host configurator)
  // if operators will edit timers on the panel.
  settingsFile.close();
}

void config_setDefaults() {
  dirtyDrainTimer = DEF_DIRTY_DRAIN_MS;
  dirtyRinseTimer = DEF_DIRTY_RINSE_MS;
  dirtyPurgeTimer = DEF_DIRTY_PURGE_MS;
  washTimer       = DEF_WASH_MS;
  causticRtnTimer = DEF_CAUSTIC_RTN_MS;
  rinseTimer      = DEF_RINSE_MS;
  rinsePurgeTimer = DEF_RINSE_PURGE_MS;
  saniTimer       = DEF_SANI_MS;
  saniRtnTimer    = DEF_SANI_RTN_MS;
  purgeTimer      = DEF_PRESSURE_MS;
  largeKegMod     = 1.5;
  pauseMaxMs      = PAUSE_MAX_MS;

  minCausticTemp     = DEFAULT_MIN_CAUSTIC_TEMP;
  optimalCausticTemp = DEFAULT_OPTIMAL_CAUSTIC_TEMP;
  maxCausticTemp     = DEFAULT_MAX_CAUSTIC_TEMP;
  maxEnclosureTemp   = DEFAULT_MAX_ENCLOSURE_TEMP;
  fanOnTemp          = DEFAULT_FAN_ON_TEMP;
  fanOffTemp         = DEFAULT_FAN_OFF_TEMP;

  snprintf(mqttBrokerIp, sizeof(mqttBrokerIp), "%d.%d.%d.%d",
           MQTT_BROKER_IP_0, MQTT_BROKER_IP_1, MQTT_BROKER_IP_2, MQTT_BROKER_IP_3);
  mqttBrokerPort = MQTT_BROKER_PORT;
  snprintf(mqttUser,      sizeof(mqttUser),      "%s", MQTT_USER);
  snprintf(mqttPass,      sizeof(mqttPass),      "%s", MQTT_PASS);
  snprintf(mqttClientId,  sizeof(mqttClientId),  "%s", MQTT_CLIENT_ID);
  snprintf(mqttTopicRoot, sizeof(mqttTopicRoot), "%s", MQTT_TOPIC_ROOT);
}
