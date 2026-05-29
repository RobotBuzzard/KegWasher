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

**KegWasher tab**

| Group | Widgets |
|---|---|
| Status | State, Connection, Remaining timer, Elapsed timer, Error message |
| Sensors | Water / Air / CO2 / E-Stop OK LEDs |
| Temperatures | Caustic °C, Enclosure °C |
| Commands | Start (green), Silence (amber), Reset (red) buttons |
| Heartbeat | Uptime, Free RAM (KB), Loop max (µs), 5-min alert if error persists |

**Vision tab**

| Group | Widgets |
|---|---|
| Camera | MJPEG stream from `http://192.168.1.91:8080/stream.mjpg` |
| Vision Stats | Camera OK LED, FPS, Frame age, Uptime |

## Topic subscriptions

| Topic | Source |
|---|---|
| `kegwasher/#` | ClearCore firmware (rr1) |
| `vision/#` | Jetson bench-vision (rr20) |

Commands published to `kegwasher/cmd/{start,silence,reset}` (QoS 1, not retained).

## Importing the flow

The flow JSON is at `nodered/kegwasher-dashboard.json` (credentials stripped — add them
via the Node-RED editor after import).

```bash
# From the Node-RED editor:
# ☰ → Import → select kegwasher-dashboard.json
# Then open the broker node and set user=rr21 + password from mosquitto-credentials.txt
```
