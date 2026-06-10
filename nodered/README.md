# Node-RED operator dashboard

Installed on CheapBourbon (192.168.1.111). Access at **http://192.168.1.111:1880/ui**  
Editor: **http://192.168.1.111:1880**

## Service

```bash
sudo systemctl status nodered
sudo systemctl restart nodered
sudo journalctl -u nodered -f --no-pager
```

## MQTT client

Node-RED connects to Mosquitto on localhost:1883 as **`rr21`** (client-id `nodered-dashboard`).  
Credentials are in Node-RED's encrypted credential store (`~/.node-red/flows_cred.json`).

## Palettes installed

- `node-red-dashboard` v3 — `/ui` endpoint, dark theme
- `node-red-contrib-ui-led` — LED indicator nodes

## Dashboard layout

**Monitor + alert only** (decided 2026-06-10): the dashboard publishes nothing to
`kegwasher/cmd/*`; all control happens at the machine. The old Commands group and the
old text-line "Screen mirror" (which simulated the pre-PackML Goldelox display) were
removed. A graphical replica of the panel, driven by the retained `kegwasher/screen`
topic, is planned **after** the panel screen redesign settles.

**KegWasher tab**

| Group | Widgets |
|---|---|
| Status | State badge (PackML `machineState`, with IDLE sub-state, e.g. `IDLE · READY`), Recipe phase, Panel screen id, Connection, Remaining timer, Elapsed timer, Error message |
| Sensors | Water / Air / CO2 / E-Stop OK LEDs |
| Temperatures | Caustic °C (enclosure temp removed from the controller 2026-06-08) |
| Heartbeat | Uptime, Free RAM (KB), Loop max (µs), ALERT row |

Alerting: if `kegwasher/state` sits in `ABORTED` for >5 min, the ALERT row fills in and
a red toast pops every minute until recovery (a 60 s inject re-checks the latch, since
the firmware's MQTT publishes are change-detected and won't re-fire on their own).

**Vision tab**

| Group | Widgets |
|---|---|
| Camera | MJPEG stream from `http://192.168.1.96:8080/stream.mjpg` (snapshot at `/snapshot.jpg`) |
| Vision Stats | Camera OK LED, FPS, Frame age, Uptime |

## Topic subscriptions

| Topic | Source |
|---|---|
| `kegwasher/{state,state/sub,phase,screen,online,timer/±,temp/caustic,sensors/±,error/±,heartbeat/±}` | ClearCore firmware (rr1) |
| `vision/#` | Jetson bench-vision (rr20) |

## Importing the flow

The flow JSON is at `nodered/kegwasher-dashboard.json` (credentials stripped — add them
via the Node-RED editor after import).

```bash
# From the Node-RED editor:
# ☰ → Import → select kegwasher-dashboard.json
# Then open the broker node and set user=rr21 + password from mosquitto-credentials.txt
```
