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

// Attempt one connection to the broker. Returns true on success.
// Publishes online=true (retained) and ip (retained) on connect, and
// registers a Last-Will-Testament so the broker auto-publishes
// online=false when this connection dies for any reason.
static bool mqtt_try_connect() {
  if (!kwEthernetReady) return false;
  if (kwMqtt.connected()) return true;

  // LWT: when the broker stops hearing from us, it publishes "false"
  // to the online topic (retained, so any new subscriber sees the
  // last-known liveness).
  bool ok = kwMqtt.connect(
      MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS,
      kwTopicOnline,  // will topic
      0,              // will QoS
      true,           // will retain
      "false");       // will message

  if (!ok) return false;

  // Announce ourselves on connect.
  kwMqtt.publish(kwTopicOnline, "true", true);
  char buf[24];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
           kwLocalIP[0], kwLocalIP[1], kwLocalIP[2], kwLocalIP[3]);
  kwMqtt.publish(kwTopicIp, buf, true);
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
//   temp/enclosure       — int °C
//   level/caustic        — int %
//   timer/elapsed_s      — seconds in current state
//   timer/remaining_s    — seconds remaining (0 outside operating states)
//   error/code           — int (0 == ERR_NONE)
//   error/message        — human-readable from diagnostics_getErrorMessage
//
// Topic strings are built on the fly via kwTopic() into a single shared
// buffer; PubSubClient copies its inputs immediately on publish() so
// reuse is safe.

static char kwTopicBuf[64];
static const char* kwTopic(const char* leaf) {
  snprintf(kwTopicBuf, sizeof(kwTopicBuf), "%s/%s", MQTT_TOPIC_ROOT, leaf);
  return kwTopicBuf;
}

// Sentinel cache. Initial values are deliberately impossible
// (state=255, errorCode=255, temps=-999) so the first call to
// mqtt_publishStatus() flushes everything to the broker.
struct MqttStatusCache {
  byte          state         = 255;
  byte          subState      = 255;
  bool          isLargeKeg    = false;
  bool          kegInit       = false;
  byte          errorCode     = 255;
  bool          waterOk       = false;
  bool          airOk         = false;
  bool          co2Ok         = false;
  bool          estopActive   = false;
  bool          sensorsInit   = false;
  int           causticTemp   = -999;
  int           enclosureTemp = -999;
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

static const char* startupSubName(byte s) {
  switch (s) {
    case STARTUP_INIT:     return "INIT";
    case STARTUP_HEATING:  return "HEATING";
    case STARTUP_IO_CHECK: return "IO_CHECK";
    case STARTUP_READY:    return "READY";
    default:               return "?";
  }
}

static void mqtt_publishStatus() {
  if (!kwMqttReady) return;
  unsigned long now = millis();
  if (now < kwMqttStatusNextMs) return;
  kwMqttStatusNextMs = now + MQTT_STATUS_INTERVAL_MS;

  char buf[24];

  // ----- State -----
  if (currentState != kwMqttCache.state) {
    kwMqttCache.state = currentState;
    if (currentState < NUM_STATES) {
      kwMqtt.publish(kwTopic("state"), stateNames[currentState], true);
    }
    // Force the substate publish below to re-evaluate so a STARTUP→
    // DRAINING transition can clear the lingering substate value.
    kwMqttCache.subState = 254;
  }

  // ----- Sub-state (only meaningful in STARTUP) -----
  if (currentState == STATE_STARTUP) {
    if (startupSubState != kwMqttCache.subState) {
      kwMqttCache.subState = startupSubState;
      kwMqtt.publish(kwTopic("state/sub"),
                     startupSubName(startupSubState), true);
    }
  } else if (kwMqttCache.subState != 255) {
    // Just left STARTUP — clear the topic so the dashboard doesn't show
    // a stale "READY" alongside e.g. "WASHING".
    kwMqttCache.subState = 255;
    kwMqtt.publish(kwTopic("state/sub"), "", true);
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
  int enclosureT = hardware_getEnclosureTemp();
  if (enclosureT != kwMqttCache.enclosureTemp) {
    kwMqttCache.enclosureTemp = enclosureT;
    snprintf(buf, sizeof(buf), "%d", enclosureT);
    kwMqtt.publish(kwTopic("temp/enclosure"), buf, true);
  }
  int level = hardware_getCausticLevel();
  if (level != kwMqttCache.causticLevel) {
    kwMqttCache.causticLevel = level;
    snprintf(buf, sizeof(buf), "%d", level);
    kwMqtt.publish(kwTopic("level/caustic"), buf, true);
  }

  // ----- Timers (operating states only) -----
  unsigned long elapsedMs = timers_getStateElapsed();
  unsigned long durationMs = 0;
  switch (currentState) {
    case STATE_DRAINING:
      durationMs = isLargeKeg ? timers_adjustForKegSize(dirtyDrainTimer) : dirtyDrainTimer; break;
    case STATE_RINSING:
      durationMs = isLargeKeg ? timers_adjustForKegSize(rinseTimer)      : rinseTimer; break;
    case STATE_WASHING:
      durationMs = isLargeKeg ? timers_adjustForKegSize(washTimer)       : washTimer; break;
    case STATE_SANITIZE:
      durationMs = isLargeKeg ? timers_adjustForKegSize(saniTimer)       : saniTimer; break;
    case STATE_PRESSURE:
      durationMs = isLargeKeg ? timers_adjustForKegSize(purgeTimer)      : purgeTimer; break;
    default:
      durationMs = 0;
  }
  unsigned long elapsedSec   = elapsedMs / 1000;
  unsigned long remainingMs  = (durationMs > elapsedMs) ? (durationMs - elapsedMs) : 0;
  unsigned long remainingSec = remainingMs / 1000;

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
    snprintf(kwTopicLog,    sizeof(kwTopicLog),    "%s/log",    MQTT_TOPIC_ROOT);
    snprintf(kwTopicOnline, sizeof(kwTopicOnline), "%s/online", MQTT_TOPIC_ROOT);
    snprintf(kwTopicIp,     sizeof(kwTopicIp),     "%s/ip",     MQTT_TOPIC_ROOT);

    IPAddress brokerIP(MQTT_BROKER_IP_0, MQTT_BROKER_IP_1,
                       MQTT_BROKER_IP_2, MQTT_BROKER_IP_3);
    kwMqtt.setServer(brokerIP, MQTT_BROKER_PORT);
    // setBufferSize default is 256 bytes — fine for short log lines.

    if (mqtt_try_connect()) {
      snprintf(buf, sizeof(buf),
               "MQTT: %u.%u.%u.%u:%u as %s",
               MQTT_BROKER_IP_0, MQTT_BROKER_IP_1, MQTT_BROKER_IP_2,
               MQTT_BROKER_IP_3, (unsigned)MQTT_BROKER_PORT, MQTT_USER);
      diagnostics_logEvent(buf);
    } else {
      diagnostics_logEvent("MQTT: initial connect failed — will retry in loop");
    }
  } else {
    diagnostics_logEvent("Ethernet: DHCP failed - running offline");
  }
}

// ----- Display refresh -----
// State handlers do their own display calls during transient phases
// (heating progress, air-burst messages). The standard status panel
// (state name / timer / temps) is refreshed here.
//
// 1000 ms cadence: a full display_update() at 9600 baud takes ~300-500 ms
// (clear + ~7 lines of text, plus Goldelox processing). At the previous
// 250 ms interval we were queuing redraws faster than they could finish,
// catching the screen mid-write — visible as a partial-frame "slow
// repeating refresh" of the status panel.
static const unsigned long DISPLAY_INTERVAL_MS = 1000;
static unsigned long lastDisplayMs = 0;

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
  display_init();
  display_showStartup();

  config_init();
  hardware_init();
  timers_init();
  diagnostics_init();
  stateMachine_init();

  // First-pass system check. Any failure pushes us straight into the
  // ERROR state with the appropriate errorCode set by hardware_allSystemsGo.
  if (!hardware_allSystemsGo()) {
    stateMachine_changeState(STATE_ERROR);
  }

  // Bring up Ethernet before the watchdog is armed — DHCP can legitimately
  // take many seconds, well over an 8 s WDT timeout. Degrades gracefully
  // to offline mode if no link or no DHCP server.
  setupEthernet();

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
  timers_update();
  hardware_readInputs();

  // ESTOP: the ISR has already killed safety-critical outputs. Here we
  // do the non-ISR-safe follow-up: log it, set the error code, and move
  // the state machine to ERROR. Doing this in the ISR would risk dead-
  // locking on Serial / CCIO transactions.
  if (hardware_consumeEstopFlag()) {
    hardware_allStop();
    errorCode = ERR_ESTOP;
    diagnostics_logEvent("E-STOP triggered");
    if (currentState != STATE_ERROR) {
      stateMachine_changeState(STATE_ERROR);
    }
  }

  stateMachine_process();

  // Only refresh the standard status panel during operating states.
  // STARTUP/FINISHED/ERROR each render their own bespoke screens via
  // display_showMessage / display_showProgress / display_showError, and
  // overlaying display_update on top would cause flicker.
  if (millis() - lastDisplayMs >= DISPLAY_INTERVAL_MS) {
    switch (currentState) {
      case STATE_DRAINING:
      case STATE_RINSING:
      case STATE_WASHING:
      case STATE_SANITIZE:
      case STATE_PRESSURE:
        display_update();
        break;
      default:
        break;
    }
    lastDisplayMs = millis();
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

  // Kick the watchdog after all per-loop work is done. If anything
  // above hangs, the WDT will fire within 8 s and the bootloader
  // brings us back through setup() cleanly.
  Watchdog.kick();

  // 10 ms loop pacing keeps the debounce window stable and bounds CPU.
  delay(10);
}
