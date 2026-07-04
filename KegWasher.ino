// ======================================================================
// KegWasher.ino - Main sketch (setup + loop)
// ======================================================================
// Brewery keg washer for Teknic ClearCore. See README.md for hardware,
// docs/state-table.md for cycle behavior, docs/io-table.md for pinout.
// ======================================================================
#include "KegConfig.h"
#include "KegHardware.h"
#include "KegStateMachine.h"
#include "KegDisplay.h"
#include "KegDisplaySerial.h"   // KDS::fontMetrics (boot diagnostics)
#include "KegTimers.h"
#include "KegDiagnostics.h"
#include "KegUtils.h"
#include <ClearCoreWatchdog.h>     // RobotBuzzard/ClearCoreWatchdog
#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>
#include "KegSecrets.h"            // gitignored — see KegSecrets.h.example

// ----- Serial logging -----
// USB Serial is only used for diagnostics_logEvent output. The CCIO
// expansion talks over COM0 and the Goldelox display talks over COM1 —
// neither uses USB.
static const unsigned long DIAG_SERIAL_BAUD = 115200;

// ----- Ethernet -----
// DHCP lease state; populated once at boot, refreshed via Ethernet.maintain()
// in loop(). Stored as a 4-byte array (not Arduino's IPAddress class) so
// display / status code can include this without pulling in <Ethernet.h>
// and the whole lwIP transitive dependency.
uint8_t  kwLocalIP[4]      = {0, 0, 0, 0};
bool     kwEthernetReady   = false;

// Live count of HTTP clients currently being served. Read by the display
// footer to show a connection indicator. Will be incremented/decremented
// by the HTTP server when that lands (Phase 0 #3 onward); 0 for now.
volatile uint8_t kwHttpClients = 0;

// MQTT broker connectivity, surfaced to the display footer so the screen
// can show a purple "M*" indicator when the log mirror is live. Updated
// strictly from mqtt_try_connect / mqtt_loop, both of which run only
// from the main loop / setup — no concurrency, no volatile needed.
bool kwMqttReady = false;

// MQTT plumbing. EthernetClient wraps a TCP connection to the broker;
// PubSubClient handles framing, keepalive, and pub/sub semantics on top.
static EthernetClient kwEthClient;
static PubSubClient   kwMqtt(kwEthClient);

// Pre-built topic strings — saves re-snprintf-ing every publish.
static char kwTopicLog[48];
static char kwTopicOnline[48];
static char kwTopicIp[48];

// Reconnect throttle so a broker outage doesn't bury the loop in
// connect attempts (each connect() can block up to a few seconds).
static unsigned long kwMqttNextRetryMs = 0;
static const unsigned long MQTT_RETRY_INTERVAL_MS = 5000;

// Footer MQTT pub/sub indicator activity (drives the P/S dots on the panel).
static unsigned long kwMqttLastPubMs = 0;   // last publish (P indicator)
static unsigned long kwMqttLastRxMs  = 0;   // last received cmd (S indicator)
static unsigned long kwMqttIndNextMs = 0;   // footer-indicator refresh throttle

// Build a topic string under the runtime MQTT topic root into a shared buffer.
// PubSubClient copies its inputs immediately on publish/subscribe so
// reuse is safe. Defined here (not next to its other callers below)
// so mqtt_try_connect can subscribe at connect time.
static char kwTopicBuf[64];
static const char* kwTopic(const char* leaf) {
  snprintf(kwTopicBuf, sizeof(kwTopicBuf), "%s/%s", mqttTopicRoot, leaf);
  return kwTopicBuf;
}

// ----- Config mirror (the remote editor's read surface) -----
// Mirror the full non-secret runtime config to retained kegwasher/cfg/<key>
// topics: the machine's live config is always inspectable, and tools/kwcfg
// reads these and writes via cmd/cfgset. The mqtt* block is deliberately
// excluded — credentials are provisioning (KegSecrets.h / hand-edited card),
// not process config, and don't belong on the broker.
static void mqtt_cfgPubStr(const char* key, const char* v) {
  char t[48];
  snprintf(t, sizeof(t), "cfg/%s", key);
  kwMqtt.publish(kwTopic(t), v, true);
}
static void mqtt_cfgPubUL(const char* key, unsigned long v) {
  char b[16];
  snprintf(b, sizeof(b), "%lu", v);
  mqtt_cfgPubStr(key, b);
}
static void mqtt_cfgPubInt(const char* key, int v) {
  char b[16];
  snprintf(b, sizeof(b), "%d", v);
  mqtt_cfgPubStr(key, b);
}
static void mqtt_publishConfig() {
  mqtt_cfgPubUL("dirtyDrainTimer", dirtyDrainTimer);
  mqtt_cfgPubUL("dirtyRinseTimer", dirtyRinseTimer);
  mqtt_cfgPubUL("dirtyPurgeTimer", dirtyPurgeTimer);
  mqtt_cfgPubUL("washTimer",       washTimer);
  mqtt_cfgPubUL("causticRtnTimer", causticRtnTimer);
  mqtt_cfgPubUL("rinseTimer",      rinseTimer);
  mqtt_cfgPubUL("rinsePurgeTimer", rinsePurgeTimer);
  mqtt_cfgPubUL("saniTimer",       saniTimer);
  mqtt_cfgPubUL("saniRtnTimer",    saniRtnTimer);
  mqtt_cfgPubUL("purgeTimer",      purgeTimer);
  mqtt_cfgPubUL("fullDrainTimer",  fullDrainTimer);
  mqtt_cfgPubUL("pauseMaxMs",      pauseMaxMs);
  mqtt_cfgPubUL("maxHeatingMs",    maxHeatingMs);
  char b[16];
  snprintf(b, sizeof(b), "%.2f", largeKegMod);
  mqtt_cfgPubStr("largeKegMod", b);
  mqtt_cfgPubInt("minCausticTemp",     minCausticTemp);
  mqtt_cfgPubInt("optimalCausticTemp", optimalCausticTemp);
  mqtt_cfgPubInt("maxCausticTemp",     maxCausticTemp);
  mqtt_cfgPubInt("tempCalOffsetC10",   tempCalOffsetC10);
  mqtt_cfgPubInt("minHeatingRate",     minHeatingRate);
  snprintf(b, sizeof(b), "%.1f", (double)etsShuntOhms);
  mqtt_cfgPubStr("etsShuntOhms", b);
  mqtt_cfgPubStr("heaterMode", heaterExternal ? "ext" : "fw");
  mqtt_cfgPubStr("benchMode", kwBenchMode ? "on" : "off");
  mqtt_cfgPubStr("netMode", netStaticMode ? "static" : "dhcp");
  // Address fields only when set — an empty retained payload deletes the
  // topic (same rule as kegwasher/outputs), which is the correct "unset".
  if (netIp[0])   mqtt_cfgPubStr("netIp",   netIp);
  if (netMask[0]) mqtt_cfgPubStr("netMask", netMask);
  if (netGw[0])   mqtt_cfgPubStr("netGw",   netGw);
  if (cfgTouchCalValid) {
    for (int i = 0; i < 6; i++) {
      char k[12];
      snprintf(k, sizeof(k), "touchCal%c", 'A' + i);
      snprintf(b, sizeof(b), "%.6f", (double)cfgTouchCal[i]);
      mqtt_cfgPubStr(k, b);
    }
  }
  // Legacy dashboard aliases for the two thresholds (predate cfg/*).
  char b2[16];
  snprintf(b2, sizeof(b2), "%d", minCausticTemp);
  kwMqtt.publish(kwTopic("config/temp_floor"), b2, true);
  snprintf(b2, sizeof(b2), "%d", optimalCausticTemp);
  kwMqtt.publish(kwTopic("config/temp_target"), b2, true);
}

// Attempt one connection to the broker. Returns true on success.
// Publishes online=true (retained) and ip (retained) on connect, and
// registers a Last-Will-Testament so the broker auto-publishes
// online=false when this connection dies for any reason. Subscribes
// to the cmd/+ topics so the operator/dashboard can drive the cleaner.
static bool mqtt_try_connect() {
  if (!kwEthernetReady) return false;
  if (kwMqtt.connected()) return true;

  // LWT: when the broker stops hearing from us, it publishes "false"
  // to the online topic (retained, so any new subscriber sees the
  // last-known liveness).
  bool ok = kwMqtt.connect(
      mqttClientId, mqttUser, mqttPass,
      kwTopicOnline,  // will topic
      0,              // will QoS
      true,           // will retain
      "false");       // will message

  if (!ok) return false;

  // Announce ourselves on connect.
  kwMqtt.publish(kwTopicOnline, "true", true);
  char buf[64];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
           kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
  kwMqtt.publish(kwTopicIp, buf, true);

  // Firmware version + build identifier — retained so we can verify which
  // version is actually running on the ClearCore even when offline.
  snprintf(buf, sizeof(buf), "v%s %s %s", KW_FIRMWARE_VERSION, __DATE__, __TIME__);
  kwMqtt.publish(kwTopic("firmware"), buf, true);

  // Heater mode is fixed at boot (SD key, not panel-editable) — publish once
  // per connect so "which mode is this machine actually in?" is answerable
  // remotely. ext = ETS50N thermostat owns the burn, permit always asserted.
  kwMqtt.publish(kwTopic("mode/heater"), heaterExternal ? "ext" : "fw", true);

  // Full config mirror to retained cfg/<key> topics (+ the two legacy
  // config/temp_* aliases). Re-published after every accepted cfgset.
  mqtt_publishConfig();

  // Subscribe to the command surface. Single-level wildcard '+' matches
  // any leaf under cmd/ (start, silence, reset, future additions).
  // Subscriptions don't survive a disconnect, so we re-subscribe on
  // every successful (re)connect.
  kwMqtt.subscribe(kwTopic("cmd/+"));

  kwMqttReady = true;
  return true;
}

// Maintenance: call from loop(). Drives the keepalive ping, lets the
// library process incoming traffic (none yet, but plumbed for the
// upcoming command-topic subscriptions), and retries the broker
// connection with a throttle when down.
static void mqtt_loop() {
  if (!kwEthernetReady) return;
  if (kwMqtt.connected()) {
    kwMqtt.loop();
    return;
  }
  // Lost the connection — clear the flag so the footer indicator goes
  // away immediately, then schedule a throttled reconnect.
  kwMqttReady = false;
  if (millis() < kwMqttNextRetryMs) return;
  kwMqttNextRetryMs = millis() + MQTT_RETRY_INTERVAL_MS;
  mqtt_try_connect();
}

// Network log sender. Safe to call before MQTT comes up — it just
// short-circuits when the client isn't connected. Non-blocking;
// one MQTT PUBLISH per call.
void net_log_send(const char* msg) {
  if (!kwEthernetReady || !kwMqtt.connected()) return;
  kwMqtt.publish(kwTopicLog, msg, false);
}

// ---------------------------------------------------------------------
// MQTT command subscribe — Phase 0 #5 of the operator/dashboard surface.
// ---------------------------------------------------------------------
// Mirrors the physical buttons over MQTT so a Node-RED dashboard (or
// any other subscriber) can drive the cleaner from anywhere on the
// LAN. Topics:
//
//   kegwasher/cmd/start    Same as the physical cycleStart (IO1) press.
//                          STARTUP_READY  → start cycle.
//                          FINISHED       → next keg.
//                          ERROR          → silence alarm (the one-
//                                           tick MQTT pulse falls
//                                           under the 2 s long-press
//                                           threshold, so it's read
//                                           as a short press).
//   kegwasher/cmd/silence  Same as the physical manualDrain (IO2)
//                          press in ERROR / FINISHED — silences the
//                          alarm without changing state.
//   kegwasher/cmd/reset    Hard reset out of ERROR back to STARTUP.
//                          Equivalent to the physical 2 s long-press
//                          on START in ERROR.
//   kegwasher/cmd/keg_size DEFERRED — isLargeKeg is overwritten by
//                          hardware_readInputs() every loop, so any
//                          MQTT-set value is immediately clobbered.
//                          Needs a remote-override mechanism (out of
//                          scope here).
//   kegwasher/cmd/cfgset   Remote config editor write path. Payload is
//                          one KEY=VALUE (same schema as WASHER.CFG).
//                          Validated + persisted to SD + mirrored back
//                          to the retained cfg/* topics; every attempt
//                          is answered on kegwasher/cfg/ack ("OK ..."
//                          or "ERR ..."). See mqtt_handleCfgSet().
//
// Hardening:
//   - Any command is refused if ESTOP is currently active. The intent
//     is "an e-stop button must dominate any remote start"; trying to
//     remote-clear an e-stop with the button still held is a footgun.
//   - Every received command is logged via diagnostics_logEvent which
//     mirrors to kegwasher/log, giving a free audit trail.
//   - Payload is ignored for the button commands — presence of a publish
//     on the topic is the command. (Some MQTT button-source UIs send "1"
//     or "true" on press, others send empty; treat any payload as a
//     trigger.) cfgset is the exception: its payload is the KEY=VALUE.
//   - Auth is whatever the broker enforces. We don't add per-message
//     X-API-Key plumbing — the broker's username/password is the
//     authorisation surface.

// Pending command flags set by the MQTT callback, drained at the top
// of loop() into the button-pressed flags so the state machine acts on
// them through its existing code paths.
static volatile bool kwMqttCmdStart   = false;
static volatile bool kwMqttCmdSilence = false;
static volatile bool kwMqttCmdReset   = false;
static volatile bool kwMqttCmdPause   = false;
static volatile bool kwMqttCmdResume  = false;
static volatile bool kwMqttCmdStop    = false;
static volatile bool kwMqttCmdRestart = false;

// cmd/cfgset is the one command that carries a payload (KEY=VALUE — the remote
// config editor's write path). One pending slot: a second cfgset arriving
// before the first is processed is dropped; tools/kwcfg waits for the cfg/ack
// reply before sending the next key, so this never happens in practice.
static volatile bool kwMqttCmdCfgSet = false;
static char kwMqttCfgSetBuf[KEY_MAX_LENGTH + VALUE_MAX_LENGTH + 2];

// PubSubClient invokes this synchronously from kwMqtt.loop() when a
// message arrives on a subscribed topic. Topic and payload are valid
// only for the duration of the call.
//
// **Critical**: this callback MUST NOT call kwMqtt.publish() (directly
// or transitively via diagnostics_logEvent → net_log_send). PubSubClient
// shares a single 256-byte buffer between send and receive paths;
// publishing mid-receive corrupts the inbound packet parser and causes
// phantom callback re-entries on garbled topics. All logging and side
// effects are deferred to mqtt_applyCmdFlags() which runs in the safe
// main-loop context. See bench-confirmed failure 2026-05-14.
static void mqtt_callback(char *topic, byte *payload, unsigned int length) {
  kwMqttLastRxMs = millis();    // sub-activity (drives the footer 'S' dot)

  const char *leaf = strrchr(topic, '/');
  if (!leaf || !leaf[1]) return;
  leaf++;

  // Refuse commands while ESTOP is active. Silent at the callback —
  // mqtt_applyCmdFlags can't observe the refusal because we never set
  // the flag, so this won't show up in the audit log. That's the right
  // tradeoff: under ESTOP the operator's attention is on the physical
  // device, not the dashboard.
  if (isEstopActive) return;

  if (strcmp(leaf, "start") == 0) {
    kwMqttCmdStart = true;
  } else if (strcmp(leaf, "silence") == 0) {
    kwMqttCmdSilence = true;
  } else if (strcmp(leaf, "reset") == 0) {
    kwMqttCmdReset = true;
  } else if (strcmp(leaf, "pause") == 0) {
    kwMqttCmdPause = true;
  } else if (strcmp(leaf, "resume") == 0) {
    kwMqttCmdResume = true;
  } else if (strcmp(leaf, "stop") == 0) {
    kwMqttCmdStop = true;
  } else if (strcmp(leaf, "restart") == 0) {
    kwMqttCmdRestart = true;
  } else if (strcmp(leaf, "cfgset") == 0) {
    // Copy the payload out — it's only valid for the duration of this call.
    // Processing (validation, SD write, ack publish) happens in
    // mqtt_handleCfgSet() from the loop, never here (no publishing mid-receive).
    if (!kwMqttCmdCfgSet && length > 0 && length < sizeof(kwMqttCfgSetBuf)) {
      memcpy(kwMqttCfgSetBuf, payload, length);
      kwMqttCfgSetBuf[length] = '\0';
      kwMqttCmdCfgSet = true;
    }
  }
  // Unknown leaves are silently dropped. We can't safely log from here
  // for the same reason — see header comment. A dashboard publishing to
  // bogus topics is a dashboard bug, surfaced via mosquitto_sub on the
  // operator side, not via firmware logs.
}

// Apply one remote KEY=VALUE config edit — shared by MQTT cmd/cfgset and the
// embedded web editor (/set). Writes the human-readable outcome into ack
// ("OK key=value" or "ERR <reason>") and logs it. Rules documented on
// mqtt_handleCfgSet below.
static void kw_remoteCfgApply(const char* kvline, char* ack, size_t ackLen) {
  char line[KEY_MAX_LENGTH + VALUE_MAX_LENGTH + 2];
  snprintf(line, sizeof(line), "%s", kvline);
  char key[KEY_MAX_LENGTH] = {0};
  char value[VALUE_MAX_LENGTH] = {0};

  if (!utils_parseConfigLine(line, key, value)) {
    snprintf(ack, ackLen, "ERR not KEY=VALUE: %.60s", kvline);
  } else if (machineState != MACH_IDLE && machineState != MACH_COMPLETE &&
             machineState != MACH_ABORTED) {
    snprintf(ack, ackLen, "ERR busy (%s): %s", machStateNames[machineState], key);
  } else if (strncmp(key, "mqtt", 4) == 0) {
    snprintf(ack, ackLen, "ERR mqtt* keys are not remote-settable: %s", key);
  } else {
    int oldMin = minCausticTemp, oldOpt = optimalCausticTemp, oldMax = maxCausticTemp;
    byte r = config_applyKV(key, value);
    if (r == KW_CFG_APPLIED && !config_tempsOrdered()) {
      minCausticTemp = oldMin; optimalCausticTemp = oldOpt; maxCausticTemp = oldMax;
      snprintf(ack, ackLen, "ERR temps must be floor<=target<cutoff: %s=%s", key, value);
    } else if (r == KW_CFG_APPLIED) {
      // Web-auth credentials never echo back in cleartext (the ack goes to
      // the MQTT log mirror too).
      const char* shown = (strncmp(key, "web", 3) == 0 && value[0]) ? "****"
                                                                    : value;
      if (cfgLoadedFromSD) {
        config_saveToSD();
        snprintf(ack, ackLen, "OK %s=%s", key, shown);
      } else {
        snprintf(ack, ackLen, "OK %s=%s (RAM only - no SD)", key, shown);
      }
      mqtt_publishConfig();   // refresh the retained cfg/* mirror
    } else if (r == KW_CFG_UNKNOWN) {
      snprintf(ack, ackLen, "ERR unknown key: %s", key);
    } else {
      snprintf(ack, ackLen, "ERR out of range: %s=%s", key, value);
    }
  }
  char logbuf[110];
  snprintf(logbuf, sizeof(logbuf), "Cfgset %s", ack);
  diagnostics_logEvent(logbuf);
}

// Process a pending cmd/cfgset (KEY=VALUE) in safe loop context. Every outcome
// is answered on kegwasher/cfg/ack (non-retained): "OK key=value" or
// "ERR <reason>". Rules:
//   - refused while a cycle is active (EXECUTE/HELD/STARTING/STOPPING) — edit
//     config at IDLE/COMPLETE/ABORTED only;
//   - mqtt* keys refused (credentials are provisioning, not process config —
//     and the on-device saver intentionally never writes them to the card);
//   - same bounded validation as the SD loader (config_applyKV), plus the
//     temps-ordered rule enforced transactionally (violating set is rolled back);
//   - accepted values persist via config_saveToSD() when a card is present,
//     RAM-only otherwise (bench), and the cfg/* mirror is republished.
static void mqtt_handleCfgSet() {
  if (!kwMqttCmdCfgSet) return;
  char line[sizeof(kwMqttCfgSetBuf)];
  strncpy(line, kwMqttCfgSetBuf, sizeof(line));
  line[sizeof(line) - 1] = '\0';
  kwMqttCmdCfgSet = false;

  char ack[96];
  kw_remoteCfgApply(line, ack, sizeof(ack));
  kwMqtt.publish(kwTopic("cfg/ack"), ack, false);
}

// Drain the MQTT command flags into the button-pressed flags. Must run
// AFTER hardware_readInputs() (which overwrites them) but BEFORE
// stateMachine_process() (which reads them). One-tick pulses; the next
// hardware_readInputs() will reset to actual hardware state.
//
// All logging happens here, NOT in the callback — see the warning on
// mqtt_callback for why.
//
// `reset` skips the button path entirely because it has no physical-
// button equivalent that we can simulate — the long-press timing model
// in state_error doesn't translate cleanly to a single MQTT publish.
// We just clear errorCode and force the state transition directly.
static void mqtt_applyCmdFlags() {
  // On-screen START button (touch) — same one-tick pulse as the physical
  // start button. Refused under ESTOP, like the MQTT start command.
  if (display_takeTouchStart() && !isEstopActive) {
    isCycleStartPressed = true;
    diagnostics_logEvent("Touch: start");
  }
  if (kwMqttCmdStart) {
    kwMqttCmdStart = false;
    diagnostics_logEvent("Remote cmd: start");
    isCycleStartPressed = true;
  }
  if (kwMqttCmdSilence) {
    kwMqttCmdSilence = false;
    diagnostics_logEvent("Remote cmd: silence");
    display_requestSilence();   // same flag as the COMPLETE screen's SILENCE button
  }
  // On-screen cycle controls (touch).
  if (display_takeTouchPause()) {
    diagnostics_logEvent("Touch: pause toggle");
    stateMachine_togglePause();
  }
  if (display_takeTouchRestart()) {
    diagnostics_logEvent("Touch: restart");
    stateMachine_restart();    // re-run the current stage
  }
  if (display_takeTouchStop()) {
    diagnostics_logEvent("Touch: stop/drain");
    stateMachine_stop();       // evacuate, then halt
  }
  if (display_takeTouchRecover()) {
    diagnostics_logEvent("Touch: recover");
    stateMachine_reset();      // ABORTED → PAUSE/READY (refused while fault active)
  }
  if (kwMqttCmdPause) {
    kwMqttCmdPause = false;
    diagnostics_logEvent("Remote cmd: pause");
    stateMachine_setPause(true);
  }
  if (kwMqttCmdResume) {
    kwMqttCmdResume = false;
    diagnostics_logEvent("Remote cmd: resume");
    stateMachine_setPause(false);
  }
  if (kwMqttCmdStop) {
    kwMqttCmdStop = false;
    diagnostics_logEvent("Remote cmd: stop");
    stateMachine_stop();
  }
  if (kwMqttCmdRestart) {
    kwMqttCmdRestart = false;
    diagnostics_logEvent("Remote cmd: restart");
    stateMachine_restart();
  }
  if (kwMqttCmdReset) {
    kwMqttCmdReset = false;
    diagnostics_logEvent("Remote cmd: reset");
    // PackML Clear/Reset surface: ABORTED→Clearing (path B), STOPPED/COMPLETE→Idle.
    stateMachine_reset();
  }
  mqtt_handleCfgSet();   // pending remote config edit (validated + acked)
}

// ---------------------------------------------------------------------
// MQTT status publish — Phase 0 #2 of the operator/dashboard surface.
// ---------------------------------------------------------------------
// Every operating value the firmware knows is mirrored to a retained
// MQTT topic under MQTT_TOPIC_ROOT. "Retained" means a Node-RED (or
// other) subscriber that connects fresh sees the last known value of
// every topic immediately, no polling. We only publish when a value
// actually changes (vs. a cached copy), so steady-state traffic is just
// the per-second timer ticks.
//
// Topic layout (under MQTT_TOPIC_ROOT, default "kegwasher"):
//
//   state                — STARTUP|DRAINING|RINSING|WASHING|SANITIZE|
//                          PRESSURE|FINISHED|ERROR
//   state/sub            — INIT|HEATING|IO_CHECK|READY (cleared
//                          to "" when not in STARTUP)
//   keg                  — SMALL|LARGE
//   sensors/water        — OK|FAIL
//   sensors/air          — OK|FAIL
//   sensors/co2          — OK|FAIL
//   sensors/estop        — INACTIVE|ACTIVE
//   outputs              — CSV of driven actuators ("DRN,AIR"; "" = all off)
//   temp/caustic         — int °C
//   level/caustic        — int %
//   timer/elapsed_s      — seconds in current state
//   timer/remaining_s    — seconds remaining (0 outside operating states)
//   error/code           — int (0 == ERR_NONE)
//   error/message        — human-readable from diagnostics_getErrorMessage
//
// Topic strings are built on the fly via kwTopic() into a single shared
// buffer (defined further up, near mqtt_try_connect, so subscribe can
// use it at connect time). PubSubClient copies its inputs immediately
// on publish() so reuse is safe.

// Sentinel cache. Initial values are deliberately impossible
// (state=255, errorCode=255, temps=-999) so the first call to
// mqtt_publishStatus() flushes everything to the broker.
struct MqttStatusCache {
  byte          state         = 255;   // PackML machineState
  byte          phase         = 255;   // ISA-88 recipePhase
  byte          subState      = 255;   // IDLE sub-state
  bool          paused        = false;
  bool          pausedInit    = false;
  bool          isLargeKeg    = false;
  bool          kegInit       = false;
  byte          errorCode     = 255;
  bool          waterOk       = false;
  bool          airOk         = false;
  bool          co2Ok         = false;
  bool          estopActive   = false;
  bool          sensorsInit   = false;
  int           causticTemp   = -999;
  int           causticLevel  = -999;   // -1/0/1: sentinel / LOW / OK (float switch)
  int           outBits       = -1;     // driven actuators (hardware_getOutputBits)
  int           lowTempWarn   = -999;   // kwLowTempWarn (warn-only banner state)
  int           fullDrainMode = -999;   // latching DRAIN switch (IO2), live state
  unsigned long elapsedSec    = 0xFFFFFFFFUL;
  unsigned long remainingSec  = 0xFFFFFFFFUL;
  char          screen[16]    = "";    // panel screen id (display_currentScreen)
};
static MqttStatusCache kwMqttCache;

// Throttle the work itself; per-loop publish is unnecessary since
// nothing on this device changes faster than a few Hz. State
// transitions still propagate within ~250 ms of being committed.
static unsigned long kwMqttStatusNextMs = 0;
static const unsigned long MQTT_STATUS_INTERVAL_MS = 250;

// ----- Heartbeat -----
// Three retained topics published every 5 s as a "is the firmware loop
// itself healthy?" signal — separate from kegwasher/online which only
// reflects TCP-level liveness (the broker can think we're connected
// while the loop is wedged behind, e.g., a runaway display call).
//
//   heartbeat/uptime_s     monotonic seconds since boot
//   heartbeat/free_ram     bytes between current stack frame and heap
//                          end — useful as a memory-pressure trend
//   heartbeat/loop_max_us  longest single loop iteration in the last
//                          5 s window. Reset to 0 after each publish so
//                          the next reading reflects the next window
//                          fresh, not a peak that never decays.
//
// 5 s cadence picked because it gives a watching client enough margin
// to flag "no heartbeat for >10 s = wedged" without false positives
// from network jitter, and is 500x lower broker load than per-loop.
static unsigned long kwHeartbeatNextMs = 0;
static const unsigned long MQTT_HEARTBEAT_INTERVAL_MS = 5000;
// Per-tick max-loop accumulator, sampled in loop() between work and
// the pacing delay. 0 between publishes; the first loop after a
// heartbeat publish writes the new max.
static unsigned long kwLoopMaxUs = 0;

// newlib's sbrk(0) returns the current heap break — the address just
// past the end of allocated heap memory. Subtracting that from a
// stack-frame address gives the gap between the heap top and the
// stack bottom, which is the practical "headroom" measure on this
// chip. Doesn't account for fragmentation inside the heap.
extern "C" char* sbrk(int incr);
static int freeRam() {
  char stackTop;
  return (int)((char*)&stackTop - (char*)sbrk(0));
}

static const char* idleSubName(byte s) {
  switch (s) {
    case IDLE_INIT:      return "INIT";
    case IDLE_NOT_READY: return "NOT_READY";
    case IDLE_READY:     return "READY";
    case IDLE_SETTINGS:  return "SETTINGS";
    default:             return "?";
  }
}

static void mqtt_publishStatus() {
  if (!kwMqttReady) return;
  unsigned long now = millis();
  if (now < kwMqttStatusNextMs) return;
  kwMqttStatusNextMs = now + MQTT_STATUS_INTERVAL_MS;

  char buf[24];

  // ----- Machine state (PackML, Axis A) -----
  if (machineState != kwMqttCache.state) {
    kwMqttCache.state = machineState;
    if (machineState < NUM_MACH_STATES) {
      kwMqtt.publish(kwTopic("state"), machStateNames[machineState], true);
    }
    // Force the substate publish below to re-evaluate so leaving IDLE clears
    // the lingering substate value.
    kwMqttCache.subState = 254;
  }

  // ----- Recipe phase (ISA-88, Axis B) — only meaningful while EXECUTE -----
  if (recipePhase != kwMqttCache.phase) {
    kwMqttCache.phase = recipePhase;
    if (machineState == MACH_EXECUTE && recipePhase < NUM_PHASES) {
      kwMqtt.publish(kwTopic("phase"), phaseNames[recipePhase], true);
    } else {
      kwMqtt.publish(kwTopic("phase"), "", true);  // clear when not executing
    }
  }

  // ----- IDLE sub-state (only meaningful while IDLE) -----
  if (machineState == MACH_IDLE) {
    if (idleSub != kwMqttCache.subState) {
      kwMqttCache.subState = idleSub;
      kwMqtt.publish(kwTopic("state/sub"), idleSubName(idleSub), true);
    }
  } else if (kwMqttCache.subState != 255) {
    // Just left IDLE — clear the topic so the dashboard doesn't show a stale
    // "READY" alongside e.g. "EXECUTE".
    kwMqttCache.subState = 255;
    kwMqtt.publish(kwTopic("state/sub"), "", true);
  }

  // ----- Held (PackML; the PAUSE overlay) -----
  bool held = (machineState == MACH_HELD);
  if (!kwMqttCache.pausedInit || held != kwMqttCache.paused) {
    kwMqttCache.paused = held;
    kwMqttCache.pausedInit = true;
    kwMqtt.publish(kwTopic("paused"), held ? "YES" : "NO", true);
  }

  // ----- Panel screen id (the dashboard's display replica follows this) -----
  const char* scr = display_currentScreen();
  if (strcmp(scr, kwMqttCache.screen) != 0) {
    snprintf(kwMqttCache.screen, sizeof(kwMqttCache.screen), "%s", scr);
    kwMqtt.publish(kwTopic("screen"), scr, true);
  }

  // ----- Keg size -----
  if (!kwMqttCache.kegInit || isLargeKeg != kwMqttCache.isLargeKeg) {
    kwMqttCache.isLargeKeg = isLargeKeg;
    kwMqttCache.kegInit = true;
    kwMqtt.publish(kwTopic("keg"), isLargeKeg ? "LARGE" : "SMALL", true);
  }

  // ----- Error -----
  if (errorCode != kwMqttCache.errorCode) {
    kwMqttCache.errorCode = errorCode;
    snprintf(buf, sizeof(buf), "%u", errorCode);
    kwMqtt.publish(kwTopic("error/code"), buf, true);
    kwMqtt.publish(kwTopic("error/message"),
                   diagnostics_getErrorMessage(errorCode), true);
  }

  // ----- Sensors -----
  if (!kwMqttCache.sensorsInit || isWaterOk != kwMqttCache.waterOk) {
    kwMqttCache.waterOk = isWaterOk;
    kwMqtt.publish(kwTopic("sensors/water"),
                   isWaterOk ? "OK" : "FAIL", true);
  }
  if (!kwMqttCache.sensorsInit || isAirOk != kwMqttCache.airOk) {
    kwMqttCache.airOk = isAirOk;
    kwMqtt.publish(kwTopic("sensors/air"),
                   isAirOk ? "OK" : "FAIL", true);
  }
  if (!kwMqttCache.sensorsInit || isCo2Ok != kwMqttCache.co2Ok) {
    kwMqttCache.co2Ok = isCo2Ok;
    kwMqtt.publish(kwTopic("sensors/co2"),
                   isCo2Ok ? "OK" : "FAIL", true);
  }
  if (!kwMqttCache.sensorsInit || isEstopActive != kwMqttCache.estopActive) {
    kwMqttCache.estopActive = isEstopActive;
    kwMqtt.publish(kwTopic("sensors/estop"),
                   isEstopActive ? "ACTIVE" : "INACTIVE", true);
  }
  kwMqttCache.sensorsInit = true;

  // ----- Outputs (driven actuators, mirrored from KegHardware) -----
  // CSV of active outputs ("DRN,AIR"; "NONE" when all off — an empty retained
  // payload would delete the topic). Bit order matches hardware_getOutputBits /
  // the panel's OUT grid. 250 ms status throttle is fast enough to catch even
  // brief outputs (e.g. PRESSURE's switch-tripped CO2 shutoff).
  int outBits = hardware_getOutputBits();
  if (outBits != kwMqttCache.outBits) {
    kwMqttCache.outBits = outBits;
    static const char* const OUT_NAMES[8] =
        { "DRN", "WTR", "AIR", "CAU", "PMP", "SAN", "CO2", "HTR" };
    char outBuf[36] = "";
    byte pos = 0;
    for (byte i = 0; i < 8; i++) {
      if (outBits & (1 << i)) {
        pos += snprintf(outBuf + pos, sizeof(outBuf) - pos, "%s%s",
                        pos ? "," : "", OUT_NAMES[i]);
      }
    }
    kwMqtt.publish(kwTopic("outputs"), pos ? outBuf : "NONE", true);
  }

  // ----- Temps & level -----
  int causticT = hardware_getCausticTemp();
  if (causticT != kwMqttCache.causticTemp) {
    kwMqttCache.causticTemp = causticT;
    snprintf(buf, sizeof(buf), "%d", causticT);
    kwMqtt.publish(kwTopic("temp/caustic"), buf, true);
  }
  // Caustic "level" is the NC float switch — publish OK/LOW, not a number.
  int level = isCausticLevelOk ? 1 : 0;
  if (level != kwMqttCache.causticLevel) {
    kwMqttCache.causticLevel = level;
    kwMqtt.publish(kwTopic("level/caustic"), level ? "OK" : "LOW", true);
  }

  // Low-temp warning (amber banner state; warn-only, never a fault).
  int ltw = kwLowTempWarn ? 1 : 0;
  if (ltw != kwMqttCache.lowTempWarn) {
    kwMqttCache.lowTempWarn = ltw;
    kwMqtt.publish(kwTopic("warn/lowtemp"), ltw ? "ON" : "OFF", true);
  }

  // Full-drain mode: live state of the latching DRAIN switch (IO2).
  int fdm = isFullDrainOn ? 1 : 0;
  if (fdm != kwMqttCache.fullDrainMode) {
    kwMqttCache.fullDrainMode = fdm;
    kwMqtt.publish(kwTopic("mode/fulldrain"), fdm ? "ON" : "OFF", true);
  }

  // ----- Timers (operating states only) -----
  unsigned long elapsedMs = timers_getStateElapsed();
  // stageTimerFor() uses the LATCHED keg size and returns 0 for non-operating
  // states, so remaining-time the dashboard shows always matches the durations
  // the state machine actually runs. (Bug fixed 2026-05-29: was reading the
  // live isLargeKeg; now centralized in stageTimerFor.)
  unsigned long durationMs = stageTimerFor(recipePhase);
  unsigned long elapsedSec   = elapsedMs / 1000;
  unsigned long remainingMs  = (durationMs > elapsedMs) ? (durationMs - elapsedMs) : 0;
  // Ceiling to match the on-panel countdown (floor skips a value at 1 Hz).
  unsigned long remainingSec = (remainingMs + 999UL) / 1000UL;

  if (elapsedSec != kwMqttCache.elapsedSec) {
    kwMqttCache.elapsedSec = elapsedSec;
    snprintf(buf, sizeof(buf), "%lu", elapsedSec);
    kwMqtt.publish(kwTopic("timer/elapsed_s"), buf, true);
  }
  if (remainingSec != kwMqttCache.remainingSec) {
    kwMqttCache.remainingSec = remainingSec;
    snprintf(buf, sizeof(buf), "%lu", remainingSec);
    kwMqtt.publish(kwTopic("timer/remaining_s"), buf, true);
  }
}

// Publish loop-health heartbeat. Throttled to MQTT_HEARTBEAT_INTERVAL_MS
// (5 s); always publishes all three fields when the gate fires (no
// change-detection — these are the "is anything happening" signal, so
// a value that didn't change is meaningful information too).
static void mqtt_publishHeartbeat() {
  if (!kwMqttReady) return;
  unsigned long now = millis();
  if (now < kwHeartbeatNextMs) return;
  kwHeartbeatNextMs = now + MQTT_HEARTBEAT_INTERVAL_MS;
  kwMqttLastPubMs = now;   // pub-activity (drives the footer 'P' dot)

  char buf[16];

  snprintf(buf, sizeof(buf), "%lu", now / 1000);
  kwMqtt.publish(kwTopic("heartbeat/uptime_s"), buf, true);

  snprintf(buf, sizeof(buf), "%d", freeRam());
  kwMqtt.publish(kwTopic("heartbeat/free_ram"), buf, true);

  // Snapshot then reset so the next window measures fresh — otherwise
  // a single 400 ms display redraw at boot would dominate the reading
  // forever.
  unsigned long maxUs = kwLoopMaxUs;
  kwLoopMaxUs = 0;
  snprintf(buf, sizeof(buf), "%lu", maxUs);
  kwMqtt.publish(kwTopic("heartbeat/loop_max_us"), buf, true);
}

// ---------------------------------------------------------------------
// Embedded web config editor — http://<machine-ip>/
// ---------------------------------------------------------------------
// Browser-facing counterpart of the MQTT editor. Three routes, all GET:
//   /          the single-page editor (served from flash)
//   /cfg.json  live config + _state/_temp/_fw meta (what the page renders)
//   /set?K=V   one edit through kw_remoteCfgApply — the SAME gates and
//              validation as cmd/cfgset; the ack text is the response body
// Handling is bounded (~one request per loop tick, 250 ms read budget) so it
// can't starve the state machine or the watchdog. No auth — LAN-trust, same
// as the rest of the shop network; the busy/mqtt* gates still apply.
static EthernetServer kwHttpServer(80);
static bool kwHttpUp = false;
static unsigned long kwHttpLastReqMs = 0;   // last handled request (footer W dot)

// Base64 (RFC 4648) — just enough to compute the expected value of the
// "Authorization: Basic <token>" header when webUser/webPass are configured.
static void kw_b64enc(const char* in, char* out, size_t outCap) {
  static const char T[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t len = strlen(in), o = 0;
  for (size_t i = 0; i < len && o + 5 < outCap; i += 3) {
    uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
    if (i + 1 < len) v |= (uint32_t)(uint8_t)in[i + 1] << 8;
    if (i + 2 < len) v |= (uint8_t)in[i + 2];
    out[o++] = T[(v >> 18) & 63];
    out[o++] = T[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? T[v & 63] : '=';
  }
  out[o] = '\0';
}

static const char KW_HTTP_PAGE[] =
R"html(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KegWasher</title><style>
body{font-family:system-ui,sans-serif;background:#131313;color:#e8e8e8;margin:0 auto;padding:14px;max-width:720px}
header{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:4px}
h1{font-size:1.25em;margin:0}
.chip{padding:3px 12px;border-radius:12px;font-size:.78em;background:#3a3a3a}
.chip.run{background:#1d5c35}.chip.warn{background:#7a5a00}.chip.fault{background:#7a1f1f}
#meta{color:#8a8a8a;font-size:.78em;margin-bottom:8px}
table{width:100%;border-collapse:collapse}
tr.sec td{padding:18px 4px 5px;color:#6fbf8a;font-size:.72em;letter-spacing:.14em;border-bottom:1px solid #2c2c2c}
td{padding:9px 6px;border-bottom:1px solid #212121;vertical-align:top}
.lbl b{font-size:.95em;font-weight:600}
.hint{color:#9a9a9a;font-size:.78em;margin-top:2px;max-width:420px;line-height:1.35}
.key{color:#5c5c5c;font-family:monospace;font-size:.7em;margin-top:3px}
td.val{white-space:nowrap;text-align:right;width:120px}
td.act{width:60px;text-align:right}
input,select{width:104px;background:#222;color:#e8e8e8;border:1px solid #4a4a4a;padding:6px;border-radius:4px;font-size:.95em;box-sizing:border-box}
input.dirty,select.dirty{border-color:#e0a800}
input.bad{border-color:#c33;background:#2a1717}
.tog{appearance:none;position:relative;width:52px;height:26px;background:#454545;border:0;border-radius:13px;cursor:pointer;transition:background .15s;vertical-align:middle;padding:0}
.tog:checked{background:#2a7d4f}
.tog::before{content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#ddd;transition:left .15s}
.tog:checked::before{left:29px}
.eq{display:block;color:#8a8a8a;font-size:.72em;margin-top:4px;min-height:1em}
button{background:#2a7d4f;border:0;color:#fff;padding:7px 14px;border-radius:4px;cursor:pointer;font-size:.88em}
button:active{background:#1d5c35}
#msg{position:sticky;bottom:8px;background:#1c1c1c;border:1px solid #2c2c2c;border-radius:6px;padding:10px 12px;font-family:monospace;font-size:.85em;white-space:pre-wrap;margin-top:12px;display:none}
#msg.ok{border-color:#2a7d4f;color:#8fdc9f;display:block}
#msg.err{border-color:#a33;color:#f0a0a0;display:block}
</style></head><body>
<header><h1>KegWasher</h1><span class="chip" id="stchip">&hellip;</span><span class="chip warn" id="ltchip" style="display:none">LOW TEMP</span></header>
<div id="meta"></div>
<table id="t"></table>
<div id="msg"></div>
<script>
// NOTE (firmware side): everything in this <script> lives inside a C++ raw
// string. Arduino's ctags prototype generator does NOT understand raw strings
// — a top-level `function name(...)` here becomes a bogus C++ prototype and
// breaks the build (bitten 2026-07-04). Use `const name=(..)=>{}` arrows ONLY.
// [key, label, operator hint, unit, min, max] — unit drives the "= 10 s"
// helper and the editor type. ms=milliseconds, C=deg C, x=multiplier,
// cm=C/min, ohm, c10=tenths of a deg, tog=toggle switch, raw=free number.
// min/max mirror the firmware's validation (the firmware still re-checks).
// Sections are organized by the machine STATE each setting governs.
const GROUPS=[
["WASH CYCLE · runs during EXECUTE",[
["dirtyDrainTimer","Blow out old beer","Air + drain push the leftover product out. First stage of every cycle.","ms",5000,1800000],
["dirtyRinseTimer","First rinse","Water flush of the gross residue, straight to the drain.","ms",5000,1800000],
["dirtyPurgeTimer","Blow out first rinse","Air pushes the rinse water out to the drain.","ms",5000,1800000],
["washTimer","Caustic wash","Hot caustic recirculates through the kegs — the main clean.","ms",5000,1800000],
["causticRtnTimer","Caustic recovery","Air pushes the caustic back to its reservoir for reuse. Never goes to the drain.","ms",5000,1800000],
["rinseTimer","Post-wash rinse","Water rinse after the caustic wash, to the drain.","ms",5000,1800000],
["rinsePurgeTimer","Blow out post-rinse","Air pushes the post-wash rinse water out to the drain.","ms",5000,1800000],
["saniTimer","Sanitize","Sanitizer recirculates through the kegs.","ms",5000,1800000],
["saniRtnTimer","Sanitizer recovery","CO2 pushes the sanitizer back to its reservoir for reuse.","ms",5000,1800000],
["purgeTimer","CO2 fill limit","Longest the keg gets to reach pressure. Normally ends early when the pressure switch trips — hitting this limit is a fault (no CO2, unsealed keg, or leak).","ms",5000,1800000],
["fullDrainTimer","Full-keg drain","Replaces “Blow out old beer” when the DRAIN switch is latched — for kegs that come back full.","ms",5000,1800000],
["largeKegMod","Large-keg multiplier","Every stage time is multiplied by this when the keg-size switch is on LARGE (1/2 BBL).","x",1,3]]],
["WARM-UP · STARTING",[
["heaterMode","Heater control","OFF = fw: firmware runs the heater during warm-up only. ON = ext: the tank thermostat holds temperature all day — requires the permit relay wired in series with the thermostat.","tog"],
["optimalCausticTemp","Warm-up target (°C)","Tank temperature the heater aims for before the first cycle (fw mode).","C",0,120],
["maxHeatingMs","Warm-up time limit","If the tank is not at temperature by then, warm-up gives up with a fault.","ms",60000,7200000],
["minHeatingRate","Healthy heating rate","Warm-up slower than this gets flagged in the log (fw mode).","cm",1,50]]],
["PAUSE · HELD",[
["pauseMaxMs","Max pause time","Paused longer than this and the machine alarms and aborts — chemicals may be sitting in the keg.","ms",60000,3600000]]],
["WARNINGS & SAFETY · all states",[
["minCausticTemp","Wash floor (°C)","Below this tank temperature the amber LOW TEMP banner shows on the panel (and here). Warning only — it never stops a cycle.","C",0,120],
["maxCausticTemp","Overtemp cutoff (°C)","Safety limit: the heater is cut and the machine faults above this.","C",0,120]]],
["NETWORK · applies at next power-up",[
["netMode","Network mode","OFF = dhcp: the router assigns the address. ON = static: use the fixed address below. Changes take effect at the next power-up.","tog"],
["netIp","Static IP address","The machine's fixed address, e.g. 192.168.1.92. Required for static mode (an invalid one falls back to DHCP at boot).","ip"],
["netMask","Subnet mask","Usually 255.255.255.0 — leave empty for that default.","ip"],
["netGw","Gateway","The router's address. Leave empty to default to .1 on the IP's subnet.","ip"]]],
["SERVICE & CALIBRATION",[
["benchMode","Bench mode","ON: readiness and resource safety gates are bypassed for bench work (with this card's timers and temperatures). OFF: production gates active.","tog"],
["etsShuntOhms","Temp probe shunt","Measured value of the 4-20 mA shunt resistor (nominal 470 Ω). Set once per machine.","ohm",100,1000],
["tempCalOffsetC10","Temp reading trim","Added to the probe reading, in tenths of a degree. +10 makes it read 1.0 °C higher.","c10",-100,100],
["touchCalA","Touch cal A","Written by the touch-calibration tool — don’t hand-edit.","raw"],
["touchCalB","Touch cal B","Written by the touch-calibration tool — don’t hand-edit.","raw"],
["touchCalC","Touch cal C","Written by the touch-calibration tool — don’t hand-edit.","raw"],
["touchCalD","Touch cal D","Written by the touch-calibration tool — don’t hand-edit.","raw"],
["touchCalE","Touch cal E","Written by the touch-calibration tool — don’t hand-edit.","raw"],
["touchCalF","Touch cal F","Written by the touch-calibration tool — don’t hand-edit.","raw"]]]];
// Toggle wiring: [value when OFF, value when ON] + confirmation prompts.
const TOG={heaterMode:["fw","ext"],benchMode:["off","on"],netMode:["dhcp","static"]};
const TOGC={heaterMode_ext:"Switch heater control to EXT?\n\nThe tank thermostat will hold temperature ALL DAY.\nRequires the permit relay wired in series with the ETS50N.",
heaterMode_fw:"Switch heater control to FW (firmware warm-up only)?",
benchMode_on:"Turn BENCH MODE ON?\n\nReadiness and resource safety gates will be BYPASSED.",
benchMode_off:"Turn bench mode OFF (production gates active)?",
netMode_static:"Switch to a STATIC address at the next power-up?\n\nDouble-check the static IP below first — a wrong address makes the machine unreachable until the SD card is edited.",
netMode_dhcp:"Switch back to DHCP at the next power-up?"};
const VAL={};for(const[,ks]of GROUPS)for(const e of ks)if(e.length>4)VAL[e[0]]=[e[4],e[5]];
const stchip=document.getElementById("stchip"),ltchip=document.getElementById("ltchip"),
      meta=document.getElementById("meta"),msg=document.getElementById("msg");
const fmt=(u,v)=>{
 v=parseFloat(v);
 if(isNaN(v))return "";
 if(u=="ms"){const s=v/1000;
  if(s<60)return "= "+(s%1?s.toFixed(1):s)+" s";
  const m=Math.floor(s/60),r=Math.round(s%60);
  return "= "+m+" min"+(r?" "+r+" s":"");}
 if(u=="x")return "= "+Math.round((v-1)*100)+"% longer";
 if(u=="c10")return "= "+(v>=0?"+":"")+(v/10).toFixed(1)+" °C";
 if(u=="cm")return "°C per minute";
 if(u=="ohm")return "Ω";
 return "";}
const IPK={netIp:1,netMask:1,netGw:1};
const chk=(k,v)=>{
 if(IPK[k]){
  if(v=="")return null;   // empty = unset (documented default applies)
  const m=v.match(/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/);
  if(!m||m.slice(1).some(o=>+o>255))return "is not a valid IP address";
  return null;}
 const r=VAL[k];if(!r)return null;
 const n=parseFloat(v);
 if(isNaN(n)||!isFinite(n)||/[^0-9.eE+-]/.test(v))return "is not a number";
 if(n<r[0]||n>r[1])return "must be between "+r[0]+" and "+r[1];
 return null;}
const chip=(s)=>{
 stchip.textContent=s.state+" · "+s.temp+"°C";
 stchip.className="chip "+(s.state=="ABORTED"?"fault":
   (s.state=="HELD"||s.state=="STOPPING")?"warn":
   (s.state=="EXECUTE"||s.state=="STARTING")?"run":"");
 ltchip.style.display=s.warn=="1"?"":"none";}
const load=async(full)=>{
 const j=await (await fetch("/cfg.json")).json();
 chip({state:j._state,temp:j._temp,warn:j._warn});
 meta.textContent="firmware v"+j._fw+" · changes save to the SD card and apply to the next cycle · editing is locked out while a cycle runs";
 if(!full)return;
 const t=document.getElementById("t");t.innerHTML="";
 const done={};
 for(const[sec,keys]of GROUPS){
  const rows=keys.filter(e=>e[0]in j);
  if(!rows.length)continue;
  t.insertRow().innerHTML="<td colspan=3>"+sec+"</td>";
  t.rows[t.rows.length-1].className="sec";
  for(const[k,lbl,hint,u]of rows){done[k]=1;
   let ed,act="<button onclick=\"setk('"+k+"')\">set</button>",eq=fmt(u,j[k]);
   if(u=="tog"){
    const on=j[k]==TOG[k][1];
    ed="<input type=checkbox class=tog id=\"i_"+k+"\""+(on?" checked":"")+" onchange=\"togk('"+k+"')\">";
    act="";eq=j[k];}
   else ed="<input id=\"i_"+k+"\" data-o=\""+j[k]+"\" data-u=\""+u+"\" value=\""+j[k]+"\">";
   t.insertRow().innerHTML=
    "<td class=lbl><b>"+lbl+"</b><div class=hint>"+hint+"</div><div class=key>"+k+(u=="ms"?" · ms":"")+"</div></td>"+
    "<td class=val>"+ed+"<span class=eq id=\"e_"+k+"\">"+eq+"</span></td>"+
    "<td class=act>"+act+"</td>";}}
 const extra=Object.keys(j).filter(k=>k[0]!="_"&&!done[k]);
 if(extra.length){
  t.insertRow().innerHTML="<td colspan=3>OTHER</td>";
  t.rows[t.rows.length-1].className="sec";
  for(const k of extra)
   t.insertRow().innerHTML="<td class=lbl><div class=key>"+k+"</div></td>"+
    "<td class=val><input id=\"i_"+k+"\" data-o=\""+j[k]+"\" value=\""+j[k]+"\"></td>"+
    "<td class=act><button onclick=\"setk('"+k+"')\">set</button></td>";}
 t.oninput=e=>{const el=e.target,k=el.id.slice(2);
  if(el.type=="checkbox")return;
  el.classList.remove("bad");
  el.classList.toggle("dirty",el.value!=el.dataset.o);
  const eq=document.getElementById("e_"+k);
  if(eq&&el.dataset.u)eq.textContent=fmt(el.dataset.u,el.value);};
 t.onkeydown=e=>{if(e.key=="Enter"&&e.target.id&&e.target.type!="checkbox")setk(e.target.id.slice(2));};
}
const apply=async(k,v)=>{
 const r=await fetch("/set?"+k+"="+encodeURIComponent(v));
 const x=await r.text();
 msg.textContent=x;msg.className=x.startsWith("OK")?"ok":"err";
 return x.startsWith("OK");}
const setk=async(k)=>{
 const el=document.getElementById("i_"+k),v=el.value.trim();
 const bad=chk(k,v);
 if(bad){msg.textContent="not sent — value "+bad;msg.className="err";el.classList.add("bad");return;}
 el.classList.remove("bad");
 if(await apply(k,v)){el.dataset.o=el.value;el.classList.remove("dirty");load(false);}}
const togk=async(k)=>{
 const el=document.getElementById("i_"+k);
 const v=el.checked?TOG[k][1]:TOG[k][0];
 const warnMsg=TOGC[k+"_"+v];
 if(warnMsg&&!confirm(warnMsg)){el.checked=!el.checked;return;}
 if(await apply(k,v)){document.getElementById("e_"+k).textContent=v;load(false);}
 else el.checked=!el.checked;}
load(true);
setInterval(()=>load(false).catch(()=>{stchip.textContent="offline?";stchip.className="chip fault"}),5000);
</script></body></html>
)html";

// Build /cfg.json — same key set as the MQTT cfg/* mirror plus _meta fields.
static char kwHttpJson[1600];
static size_t kwHttpJsonLen;
static void httpJsonKV(const char* k, const char* v) {
  kwHttpJsonLen += snprintf(kwHttpJson + kwHttpJsonLen,
                            sizeof(kwHttpJson) - kwHttpJsonLen,
                            "%s\"%s\":\"%s\"", kwHttpJsonLen > 1 ? "," : "", k, v);
}
static void httpJsonUL(const char* k, unsigned long v) {
  char b[16]; snprintf(b, sizeof(b), "%lu", v); httpJsonKV(k, b);
}
static void httpJsonInt(const char* k, int v) {
  char b[16]; snprintf(b, sizeof(b), "%d", v); httpJsonKV(k, b);
}
static void http_buildCfgJson() {
  kwHttpJson[0] = '{'; kwHttpJson[1] = '\0'; kwHttpJsonLen = 1;
  httpJsonKV("_state", machStateNames[machineState]);
  httpJsonInt("_temp", hardware_getCausticTemp());
  httpJsonKV("_warn", kwLowTempWarn ? "1" : "0");   // LOW TEMP chip on the page
  httpJsonKV("_fw", KW_FIRMWARE_VERSION);
  httpJsonUL("dirtyDrainTimer", dirtyDrainTimer);
  httpJsonUL("dirtyRinseTimer", dirtyRinseTimer);
  httpJsonUL("dirtyPurgeTimer", dirtyPurgeTimer);
  httpJsonUL("washTimer",       washTimer);
  httpJsonUL("causticRtnTimer", causticRtnTimer);
  httpJsonUL("rinseTimer",      rinseTimer);
  httpJsonUL("rinsePurgeTimer", rinsePurgeTimer);
  httpJsonUL("saniTimer",       saniTimer);
  httpJsonUL("saniRtnTimer",    saniRtnTimer);
  httpJsonUL("purgeTimer",      purgeTimer);
  httpJsonUL("fullDrainTimer",  fullDrainTimer);
  char b[16];
  snprintf(b, sizeof(b), "%.2f", largeKegMod);
  httpJsonKV("largeKegMod", b);
  httpJsonUL("pauseMaxMs", pauseMaxMs);
  httpJsonInt("minCausticTemp",     minCausticTemp);
  httpJsonInt("optimalCausticTemp", optimalCausticTemp);
  httpJsonInt("maxCausticTemp",     maxCausticTemp);
  httpJsonKV("heaterMode", heaterExternal ? "ext" : "fw");
  httpJsonKV("benchMode", kwBenchMode ? "on" : "off");
  httpJsonKV("netMode", netStaticMode ? "static" : "dhcp");
  httpJsonKV("netIp",   netIp);     // empty = unset (defaults documented on
  httpJsonKV("netMask", netMask);   //  the page); JSON keeps empties so the
  httpJsonKV("netGw",   netGw);     //  editor always shows the fields
  httpJsonUL("maxHeatingMs", maxHeatingMs);
  httpJsonInt("minHeatingRate", minHeatingRate);
  snprintf(b, sizeof(b), "%.1f", (double)etsShuntOhms);
  httpJsonKV("etsShuntOhms", b);
  httpJsonInt("tempCalOffsetC10", tempCalOffsetC10);
  if (cfgTouchCalValid) {
    for (int i = 0; i < 6; i++) {
      char k[12];
      snprintf(k, sizeof(k), "touchCal%c", 'A' + i);
      snprintf(b, sizeof(b), "%.6f", (double)cfgTouchCal[i]);
      httpJsonKV(k, b);
    }
  }
  kwHttpJsonLen += snprintf(kwHttpJson + kwHttpJsonLen,
                            sizeof(kwHttpJson) - kwHttpJsonLen, "}");
}

// Write the whole body in send-buffer-friendly chunks. ClearCore's lwIP is
// small (TCP_SND_BUF 2.9 KB on a 4 KB heap): one oversized write stalls the
// connection mid-body and wedges the listener (bench-bitten 2026-07-04 —
// serving the 4.5 KB page froze port 80 while MQTT stayed up). Pump the stack
// (Ethernet.maintain → EthernetMgr.Refresh) so ACKs drain the queue and
// sndbuf recovers between chunks; bounded 3 s total, well under the 8 s WDT.
static void httpWriteAll(EthernetClient& c, const char* s, size_t len) {
  const size_t CHUNK = 1024;
  size_t off = 0;
  // Bail after 300 ms without a single byte accepted: sndbuf that never
  // recovers means the peer stalled or vanished mid-response (e.g. a browser
  // tab closed). A fixed overall budget instead of this stalled the whole
  // loop ~6 s per dead peer — most of the 8 s watchdog (bench 2026-07-04).
  unsigned long lastProgress = millis();
  while (off < len && millis() - lastProgress < 300) {
    size_t want = len - off;
    if (want > CHUNK) want = CHUNK;
    size_t n = c.write((const uint8_t*)s + off, want);
    if (n > 0) { off += n; lastProgress = millis(); }
    else { Ethernet.maintain(); delay(2); }   // sndbuf full — pump ACKs
  }
  // Give the tail segments a moment to transmit (stack pumps).
  unsigned long t1 = millis();
  while (millis() - t1 < 30) { Ethernet.maintain(); delay(1); }
}

static void httpRespond(EthernetClient& c, const char* status,
                        const char* ctype, const char* body, size_t bodyLen,
                        const char* extraHdr = nullptr) {
  char hdr[224];
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
                   "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                   status, ctype, (unsigned)bodyLen,
                   extraHdr ? extraHdr : "");
  httpWriteAll(c, hdr, (size_t)n);
  httpWriteAll(c, body, bodyLen);
}

// One request per call, bounded. CRITICAL: drain the ENTIRE request (through
// the blank line ending the headers) before responding — the ClearCore TCP
// stack keeps a client slot alive while unread bytes remain, and available()
// returns any slot with bytes, so leftover headers from request N get served
// as "request" N+1 and every response shifts by one (bench-bitten 2026-07-04).
// Do not gate reads on connection state: curl half-closes after sending, and
// native Connected() is false in CLOSE_WAIT while the buffer still holds the
// request.
static void http_process() {
  if (!kwHttpUp) return;
  EthernetClient c = kwHttpServer.available();
  if (!c) return;
  kwHttpClients++;

  // Drain everything the client sent: store the first request line, discard
  // the rest, stop at the header-terminating blank line (or 30 ms idle /
  // 400 ms total — covers multi-segment browser requests; available()
  // internally refreshes the stack so new segments surface mid-loop).
  char req[160];
  size_t n = 0;
  char hline[100];   // current header line — scanned for Authorization
  size_t hn = 0;
  char auth[80] = "";   // base64 token from "Authorization: Basic <token>"
  bool firstLineDone = false, endOfHeaders = false;
  uint32_t tail = 0;   // rolling last-4-bytes window ("\r\n\r\n" detector)
  unsigned long t0 = millis(), lastByteMs = millis();
  while (!endOfHeaders && millis() - t0 < 400) {
    int avail = c.available();
    if (avail <= 0) {
      if (millis() - lastByteMs > 30) break;   // sender done (or stale slot drained)
      continue;
    }
    while (avail-- > 0 && !endOfHeaders) {
      int ch = c.read();
      if (ch < 0) break;
      lastByteMs = millis();
      tail = (tail << 8) | (uint8_t)ch;
      if (tail == 0x0D0A0D0AUL) endOfHeaders = true;          // \r\n\r\n
      if (!firstLineDone) {
        if (ch == '\n') firstLineDone = true;
        else if (ch != '\r' && n < sizeof(req) - 1) req[n++] = (char)ch;
      }
      if (ch == '\n') {
        hline[hn] = '\0';
        if (strncasecmp(hline, "Authorization:", 14) == 0) {
          const char* b = strstr(hline, "Basic ");
          if (b) snprintf(auth, sizeof(auth), "%s", b + 6);
        }
        hn = 0;
      } else if (ch != '\r' && hn < sizeof(hline) - 1) {
        hline[hn++] = (char)ch;
      }
    }
  }
  req[n] = '\0';

  // "GET /path HTTP/1.1" → isolate /path. Anything else (stale-slot garbage,
  // non-GET methods) is dropped without a response — there is nobody to answer.
  char* path = nullptr;
  if (strncmp(req, "GET ", 4) == 0) {
    path = req + 4;
    char* sp = strchr(path, ' ');
    if (sp) *sp = '\0';
    kwHttpLastReqMs = millis();   // real request — light the footer W dot
  }

  // Optional HTTP Basic auth: active when BOTH webUser and webPass are set
  // (SD keys; empty/absent = open, the pre-auth behavior). Compare against the
  // expected base64("user:pass") — no decode needed. LAN-grade protection:
  // credentials travel base64 over plain HTTP, so this is a shop-floor lock,
  // not internet-facing security.
  if (path && webUser[0] && webPass[0]) {
    char cred[52], expect[80];
    snprintf(cred, sizeof(cred), "%s:%s", webUser, webPass);
    kw_b64enc(cred, expect, sizeof(expect));
    if (strcmp(auth, expect) != 0) {
      httpRespond(c, "401 Unauthorized", "text/plain", "auth required\n", 14,
                  "WWW-Authenticate: Basic realm=\"KegWasher\"\r\n");
      kwHttpClients--;
      return;
    }
  }

  if (!path) {
    // drained + dropped
  } else if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
    httpRespond(c, "200 OK", "text/html", KW_HTTP_PAGE, strlen(KW_HTTP_PAGE));
  } else if (strcmp(path, "/cfg.json") == 0) {
    http_buildCfgJson();
    httpRespond(c, "200 OK", "application/json", kwHttpJson, kwHttpJsonLen);
  } else if (strncmp(path, "/set?", 5) == 0 && path[5]) {
    char ack[96];
    kw_remoteCfgApply(path + 5, ack, sizeof(ack));   // query IS the KEY=VALUE
    httpRespond(c, ack[0] == 'O' ? "200 OK" : "409 Conflict",
                "text/plain", ack, strlen(ack));
  } else {
    httpRespond(c, "404 Not Found", "text/plain", "not found\n", 10);
  }
  // Deliberately NO c.stop(): EthernetClient::stop() frees the TcpData that
  // the server's slot table still references (use-after-free + double-free in
  // Available()'s cleanup — the alternating-hang bug, bench-bitten 2026-07-04).
  // We answered with Connection: close; the peer's FIN drives TcpClose in the
  // stack, and Available()'s scan then frees the drained slot exactly once.
  kwHttpClients--;
}

// How long to wait for the cable link to come up before giving up and
// continuing in offline-degraded mode. Generous because the rest of setup
// runs before the watchdog is armed.
static const unsigned long ETH_LINK_WAIT_MS = 3000;

static void setupEthernet() {
  // Empty MAC array → Ethernet library uses the ClearCore's burned-in MAC,
  // which is the right thing for production (unique per board, no risk of
  // collision on the LAN). The example in the platform shows this idiom.
  uint8_t mac[6] = {0};

  // Wait briefly for the PHY to see link, but don't block forever — a
  // bench/dev build may run with no cable plugged in.
  unsigned long t0 = millis();
  while (Ethernet.linkStatus() == LinkOFF) {
    if (millis() - t0 >= ETH_LINK_WAIT_MS) {
      diagnostics_logEvent("Ethernet: no link — running offline");
      return;
    }
    delay(100);
  }
  // Static IP when configured (netMode=static + a valid netIp): no DHCP wait,
  // address survives router reboots/lease churn, and the editor URL is stable.
  // Mask defaults to /24, gateway to .1 on the IP's subnet. DNS is unused by
  // this firmware (broker is an IP) — the gateway is passed to satisfy lwIP.
  bool up;
  uint8_t ipd[4];
  if (netStaticMode && config_parseIp(netIp, ipd)) {
    uint8_t d[4];
    IPAddress ip(ipd[0], ipd[1], ipd[2], ipd[3]);
    IPAddress mask = config_parseIp(netMask, d) ? IPAddress(d[0], d[1], d[2], d[3])
                                                : IPAddress(255, 255, 255, 0);
    IPAddress gw   = config_parseIp(netGw, d)   ? IPAddress(d[0], d[1], d[2], d[3])
                                                : IPAddress(ipd[0], ipd[1], ipd[2], 1);
    Ethernet.begin(mac, ip, gw, gw, mask);   // (mac, ip, dns, gateway, subnet)
    diagnostics_logEvent("Ethernet: static IP");
    up = true;
  } else {
    if (netStaticMode) {
      diagnostics_logEvent("netMode=static but netIp invalid - DHCP fallback");
    }
    diagnostics_logEvent("Ethernet: link up; requesting DHCP...");
    // Ethernet.begin() with a single mac arg drives DHCP. Returns truthy on
    // success. lwIP's default DHCP timeout is in the tens of seconds, hence
    // doing this BEFORE Watchdog.enable() — a slow DHCP shouldn't trip an
    // 8 s watchdog.
    up = Ethernet.begin(mac) != 0;
  }

  if (up) {
    IPAddress ip = Ethernet.localIP();
    kwLocalIP[0] = ip[0]; kwLocalIP[1] = ip[1];
    kwLocalIP[2] = ip[2]; kwLocalIP[3] = ip[3];
    kwEthernetReady = true;
    char buf[64];
    snprintf(buf, sizeof(buf), "Ethernet: IP=%u.%u.%u.%u",
             kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
    diagnostics_logEvent(buf);

    // Web config editor at http://<ip>/ (see the HTTP section above).
    kwHttpServer.begin();
    kwHttpUp = true;
    diagnostics_logEvent("Web config editor up (port 80)");

    // Bring up the MQTT log mirror. From here on, every
    // diagnostics_logEvent call also publishes to the broker.
    snprintf(kwTopicLog,    sizeof(kwTopicLog),    "%s/log",    mqttTopicRoot);
    snprintf(kwTopicOnline, sizeof(kwTopicOnline), "%s/online", mqttTopicRoot);
    snprintf(kwTopicIp,     sizeof(kwTopicIp),     "%s/ip",     mqttTopicRoot);

    // Broker IP comes from the runtime config as a dotted-quad string.
    int bo0 = 0, bo1 = 0, bo2 = 0, bo3 = 0;
    sscanf(mqttBrokerIp, "%d.%d.%d.%d", &bo0, &bo1, &bo2, &bo3);
    IPAddress brokerIP(bo0, bo1, bo2, bo3);
    kwMqtt.setServer(brokerIP, mqttBrokerPort);
    kwMqtt.setCallback(mqtt_callback);
    // setBufferSize default is 256 bytes — fine for short log lines
    // and for the empty/single-byte command payloads we expect.

    if (mqtt_try_connect()) {
      snprintf(buf, sizeof(buf), "MQTT: %s:%u as %s",
               mqttBrokerIp, (unsigned)mqttBrokerPort, mqttUser);
      diagnostics_logEvent(buf);
    } else {
      diagnostics_logEvent("MQTT: initial connect failed — will retry in loop");
    }
  } else {
    diagnostics_logEvent("Ethernet: DHCP failed - running offline");
  }
}

// ----- Display (ViSi-Genie) -----
// State handlers call display_show*() on form transitions.
// The operating state draws once on entry; partial updates follow.
static byte   lastDisplayedPhase = 0xFF;  // sentinel — forces redraw on phase change / first entry
static bool   lastHeldShown      = false; // tracks the HELD (paused) overlay state
static unsigned long lastTimerUpdateMs  = 0;
static unsigned long lastStatusUpdateMs = 0;

void setup() {
  // Diagnostics first so any later init failure can be logged.
  Serial.begin(DIAG_SERIAL_BAUD);



  // Display before config, hardware, etc. — failures in those modules
  // call display_showError() and need somewhere to render. Goldelox boot
  // sequence is the longest single step in setup (~4 s).
  display_init();          // panel up + the single "KEGWASHER" boot screen
  {
    int fcell, ftop, fh;
    KDS::fontMetrics(&fcell, &ftop, &fh);
    char fb[48];
    snprintf(fb, sizeof(fb), "Font metrics: cell=%d capTop=%d capH=%d", fcell, ftop, fh);
    diagnostics_logEvent(fb);
  }

  config_init();
  if (kwBenchMode) {
    // Runtime bench mode (no SD card found): loud on the wire so a cardless
    // boot can never silently run a real cleaner.
    Serial.println(F("=============================================="));
    Serial.println(F("**  BENCH MODE (no SD card) — gates bypassed **"));
    Serial.println(F("=============================================="));
  }
  display_bootStatus(0, "SD CONFIG", cfgLoadedFromSD ? "READ OK" : "NONE (BENCH)",
                     cfgLoadedFromSD ? GREEN : AMBER);
  hardware_init();
  display_bootStatus(1, "HARDWARE", "OK", GREEN);
  timers_init();
  diagnostics_init();
  stateMachine_init();

  // Boot readiness is handled gracefully by MACH_IDLE → IDLE_NOT_READY (it shows
  // which systems are offline and waits, no boot alarm). The one-time pre-check
  // latch (washerInitialized) is only set once all systems are go. No hard abort
  // at boot — that was a pre-precheck-redesign artifact.

  // Bring up Ethernet before the watchdog is armed — DHCP can legitimately
  // take many seconds, well over an 8 s WDT timeout. Degrades gracefully
  // to offline mode if no link or no DHCP server.
  setupEthernet();
  display_bootStatus(2, "NETWORK", kwEthernetReady ? "OK" : "OFFLINE",
                     kwEthernetReady ? GREEN : AMBER);   // IP shown in the footer
  display_bootStatus(3, "MQTT", kwMqttReady ? "OK" : "--",
                     kwMqttReady ? GREEN : AMBER);
  display_bootStatus(4, "SENSORS", hardware_allSystemsGo() ? "OK" : "CHECK",
                     hardware_allSystemsGo() ? GREEN : AMBER);
  display_bootDone(hardware_allSystemsGo());
  delay(2000);   // hold the completed boot screen, then the loop's IDLE screen takes over

  diagnostics_logEvent("Boot complete");

  // Log the cause of the most recent reset for postmortem. 0x01=POR
  // (cold boot), 0x10=EXT (button or bossac SYSRESETREQ-equiv after
  // upload), 0x20=WDT (we hung — watchdog recovered us), 0x40=SYST
  // (software reset). Multiple bits can be set if causes pile up.
  {
    char buf[40];
    snprintf(buf, sizeof(buf), "Reset cause=0x%02X", Watchdog.lastResetCause());
    diagnostics_logEvent(buf);
  }

  // Arm the hardware watchdog last — display_init's 4 s of RTS/boot
  // delays and the SD-fail message delays are all over by now. From
  // here on, every loop iteration must kick within 8 seconds or the
  // chip resets. diagnostics_runTest() disables the WDT around its
  // ~10 s of output exercises and re-enables it on exit.
  Watchdog.enable();
  diagnostics_logEvent("Watchdog armed");
}

void loop() {
  // Stamp the start of work so the heartbeat can report the longest
  // iteration in the last 5 s window. Measured before the trailing
  // pacing delay so the constant 10 ms doesn't swamp the signal.
  unsigned long loopStartUs = micros();

  timers_update();
  display_doEvents();  // pump touch every loop (serial display; replaces genie.DoEvents)
  hardware_readInputs();

  // Inject any pending MQTT command flags into the button-pressed
  // flags. Must run AFTER hardware_readInputs (which overwrites them)
  // and BEFORE stateMachine_process / the ESTOP block (which read them).
  // mqtt_loop() runs at the bottom of this same loop iteration, so a
  // command published from the dashboard takes effect ~one loop tick
  // later (~10-20 ms).
  mqtt_applyCmdFlags();

  // ESTOP: the ISR has already killed safety-critical outputs. Here we
  // do the non-ISR-safe follow-up: log it, set the error code, and move
  // the state machine to ERROR. Doing this in the ISR would risk dead-
  // locking on Serial / CCIO transactions.
  // E-STOP / safety-chain trip. The ISR (FALLING on DI8) kills outputs fast and
  // sets a flag; but we react to the POLLED isEstopActive too, so a missed edge
  // can't leave a tripped chain unhandled. Either way → Abort, and outputs are
  // re-held off every loop while the chain stays open (can't recover until the
  // NC chain is restored — operator releases E-stop AND drives report OK).
  hardware_consumeEstopFlag();          // clear the ISR flag (outputs already killed)
  if (isEstopActive) {
    hardware_allStop();
    if (machineState != MACH_ABORTED) {
      errorCode = ERR_ESTOP;
      diagnostics_logEvent("E-STOP triggered");
      stateMachine_abort();             // PackML Abort → MACH_ABORTED
    }
  }

  // Diag mode owns the machine while active: freeze the state machine so the
  // exit START press can't double as a cycle start, and so READY's redraw
  // doesn't paint over the diag messages. (Diag is gated to IDLE/COMPLETE and
  // the ESTOP path above still runs.)
  if (!diagnosticMode) stateMachine_process();

  // Indicators (IO4 red / IO5 green):
  //  - HELD (paused): both lamps breathe via PWM — visible-from-across-the-room hold.
  //  - COMPLETE (swap kegs): GREEN alone breathes — "press START for the next pair".
  //  - otherwise GREEN solid = "START will act" states; RED is owned by the fault
  //    paths (digitalWrite there overrides any leftover PWM). On resume specifically,
  //    force RED back off — no fault path runs on HELD -> EXECUTE.
  static bool kwWasHeld = false;
  if (machineState == MACH_HELD) {
    hardware_pulseLamps();
    kwWasHeld = true;
  } else if (machineState == MACH_COMPLETE) {
    kwWasHeld = false;
    hardware_pulseReadyLamp();
  } else {
    if (kwWasHeld && machineState == MACH_EXECUTE) hardware_setAlarm(false);
    kwWasHeld = false;
    hardware_setReadyLamp(machineState == MACH_IDLE && idleSub == IDLE_READY);
  }

  // Operating display: the operating screen is shown while EXECUTE (running) or
  // HELD (paused). Full redraw on recipe-phase change (the title) or first entry;
  // then overwrite only dynamic fields. IDLE/COMPLETE/STOPPED/ABORTED manage
  // their own screens from within their state handlers.
  {
    bool isOperating = (machineState == MACH_EXECUTE || machineState == MACH_HELD);
    bool held        = (machineState == MACH_HELD);

    if (isOperating) {
      // recipePhase is preserved across a HELD pause, so this fires on each
      // phase advance and on first entry — exactly when the title must redraw.
      if (recipePhase != lastDisplayedPhase) {
        lastDisplayedPhase = recipePhase;
        display_showOperatingScreen();   // title = phaseNames[recipePhase] + controls
        lastHeldShown      = held;
        lastTimerUpdateMs  = millis();
        lastStatusUpdateMs = millis();
      }
      // Reflect a hold-state change (banner + buttons); skip the countdown while
      // held so the displayed time freezes.
      if (held != lastHeldShown) {
        lastHeldShown = held;
        display_setPaused(held);
      }
      // IO dots track the hardware every tick (change-detected in KDS, so
      // this is serial-silent unless a bit flips). Unthrottled because brief
      // output changes (mid-phase valve moves like PRESSURE's CO2 shutoff)
      // can fit entirely between two 5 s status refreshes and never show.
      // Runs while HELD too: outputs are safe-off during a pause and the
      // dots should say so.
      display_updateIO();
      if (!held) {
        // LOW TEMP warn banner (warn-only; low temp never faults). While HELD
        // the banner zone belongs to the PAUSED banner; the resume/restart
        // full repaint resets KDS's shown-state so this re-asserts after.
        display_updateLowTempWarnOp(kwLowTempWarn);
        if (millis() - lastTimerUpdateMs >= 1000UL) {
          lastTimerUpdateMs = millis();
          display_updateTimer(timers_getStateElapsed());
        }
        if (millis() - lastStatusUpdateMs >= 5000UL) {
          lastStatusUpdateMs = millis();
          display_updateStatus();
        }
      }
    } else {
      lastDisplayedPhase = 0xFF;  // force fresh redraw on next operating entry
    }
  }

  diagnostics_process();

  // Renew DHCP lease as needed; no-op if no lease or no link. Cheap call
  // (just checks an internal timer in lwIP), safe to invoke every loop.
  if (kwEthernetReady) {
    Ethernet.maintain();
  }

  // Web config editor: at most one bounded request per tick (no-op when
  // nobody is connected).
  http_process();

  // Drive MQTT keepalive + retry. Non-blocking when broker is up;
  // throttled to MQTT_RETRY_INTERVAL_MS when down.
  mqtt_loop();

  // Mirror state, sensors, temps, timers to retained MQTT topics.
  // Internally throttled to MQTT_STATUS_INTERVAL_MS and only publishes
  // values that have actually changed since last call — cheap enough
  // to invoke every loop.
  mqtt_publishStatus();

  // Loop-health heartbeat (uptime, free RAM, longest loop). Internally
  // throttled to MQTT_HEARTBEAT_INTERVAL_MS (5 s).
  mqtt_publishHeartbeat();

  // Refresh the footer indicator dots (~4 Hz). P blinks on each heartbeat
  // publish, S blinks when a command is received over MQTT, W holds green
  // while the web editor is in use (its page polls every 5 s, so a 12 s
  // window ≈ "a browser has the page open").
  if (millis() >= kwMqttIndNextMs) {
    kwMqttIndNextMs = millis() + 250;
    display_setMqttIndicators(kwMqttReady,
                              (millis() - kwMqttLastPubMs) < 400,
                              (millis() - kwMqttLastRxMs) < 400,
                              (millis() - kwHttpLastReqMs) < 12000 &&
                                  kwHttpLastReqMs != 0);
  }

  // Update the loop-max accumulator with this iteration's work time.
  // Done BEFORE the pacing delay so the constant 10 ms tail isn't
  // counted — what we want to see is real work spikes.
  unsigned long loopUs = micros() - loopStartUs;
  if (loopUs > kwLoopMaxUs) kwLoopMaxUs = loopUs;

  // Kick the watchdog after all per-loop work is done. If anything
  // above hangs, the WDT will fire within 8 s and the bootloader
  // brings us back through setup() cleanly.
  Watchdog.kick();

  // 10 ms loop pacing keeps the debounce window stable and bounds CPU.
  delay(10);
}
