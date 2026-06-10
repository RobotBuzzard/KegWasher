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

  // Firmware build identifier — retained so we can verify which version
  // is actually running on the ClearCore even when offline.
  snprintf(buf, sizeof(buf), "%s %s", __DATE__, __TIME__);
  kwMqtt.publish(kwTopic("firmware"), buf, true);

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
//
// Hardening:
//   - Any command is refused if ESTOP is currently active. The intent
//     is "an e-stop button must dominate any remote start"; trying to
//     remote-clear an e-stop with the button still held is a footgun.
//   - Every received command is logged via diagnostics_logEvent which
//     mirrors to kegwasher/log, giving a free audit trail.
//   - Payload is ignored — presence of a publish on the topic is the
//     command. (Some MQTT button-source UIs send "1" or "true" on
//     press, others send empty; treat any payload as a trigger.)
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
  (void)payload; (void)length;  // payload ignored — see header comment
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
  }
  // Unknown leaves are silently dropped. We can't safely log from here
  // for the same reason — see header comment. A dashboard publishing to
  // bogus topics is a dashboard bug, surfaced via mosquitto_sub on the
  // operator side, not via firmware logs.
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
    isManualDrainPressed = true;
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
  int           causticLevel  = -999;
  unsigned long elapsedSec    = 0xFFFFFFFFUL;
  unsigned long remainingSec  = 0xFFFFFFFFUL;
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

  // ----- Temps & level -----
  int causticT = hardware_getCausticTemp();
  if (causticT != kwMqttCache.causticTemp) {
    kwMqttCache.causticTemp = causticT;
    snprintf(buf, sizeof(buf), "%d", causticT);
    kwMqtt.publish(kwTopic("temp/caustic"), buf, true);
  }
  int level = hardware_getCausticLevel();
  if (level != kwMqttCache.causticLevel) {
    kwMqttCache.causticLevel = level;
    snprintf(buf, sizeof(buf), "%d", level);
    kwMqtt.publish(kwTopic("level/caustic"), buf, true);
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
  diagnostics_logEvent("Ethernet: link up; requesting DHCP...");

  // Ethernet.begin() with a single mac arg drives DHCP. Returns truthy on
  // success. lwIP's default DHCP timeout is in the tens of seconds, hence
  // doing this BEFORE Watchdog.enable() — a slow DHCP shouldn't trip an
  // 8 s watchdog.
  if (Ethernet.begin(mac)) {
    IPAddress ip = Ethernet.localIP();
    kwLocalIP[0] = ip[0]; kwLocalIP[1] = ip[1];
    kwLocalIP[2] = ip[2]; kwLocalIP[3] = ip[3];
    kwEthernetReady = true;
    char buf[64];
    snprintf(buf, sizeof(buf), "Ethernet: IP=%u.%u.%u.%u",
             kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
    diagnostics_logEvent(buf);

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

#ifdef BENCH_MODE
  // Loud, unmissable warning on the wire so a bench-mode build can
  // never silently end up running a real cleaner. See KegConfig.h.
  Serial.println();
  Serial.println(F("=============================================="));
  Serial.println(F("**  BENCH_MODE ACTIVE — DO NOT SHIP THIS  **"));
  Serial.println(F("=============================================="));
#endif

  // Display before config, hardware, etc. — failures in those modules
  // call display_showError() and need somewhere to render. Goldelox boot
  // sequence is the longest single step in setup (~4 s).
  display_init();          // panel up + the single "KEGWASHER" boot screen

  config_init();
  display_bootStatus(0, "SD CONFIG", cfgLoadedFromSD ? "READ OK" : "DEFAULT",
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

  stateMachine_process();

  // GREEN cycle-start indicator (IO5, dedicated output): lit whenever a START press
  // will begin/advance a cycle — READY, COMPLETE (next keg), STOPPED (recover).
  hardware_setReadyLamp(machineState == MACH_COMPLETE || machineState == MACH_STOPPED ||
                        (machineState == MACH_IDLE && idleSub == IDLE_READY));

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
      if (!held) {
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

  // Refresh the footer MQTT pub/sub indicator dots (~4 Hz). P blinks on each
  // heartbeat publish, S blinks when a command is received over MQTT.
  if (millis() >= kwMqttIndNextMs) {
    kwMqttIndNextMs = millis() + 250;
    display_setMqttIndicators(kwMqttReady,
                              (millis() - kwMqttLastPubMs) < 400,
                              (millis() - kwMqttLastRxMs) < 400);
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
