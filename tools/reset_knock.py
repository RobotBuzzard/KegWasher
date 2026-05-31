#!/usr/bin/env python3
"""
reset_knock.py — prove a host-driven DTR reset reaches the Diablo16 bootloader.

Schematic-confirmed reset path (4D ClearCore Adaptor R1.3 p.11 + uUSB-PA5 R2.3):
  CP2104 DTR pin -> C8 0.1uF (AC couple) -> Q1 MMBT3904 base -> RESET (active low).
Edge-triggered: a RISING edge at the DTR pin fires a ~few-hundred-us LOW reset
pulse. CP210x "DTR asserted" = pin LOW, so the rising edge is assert->deassert.

This drives the DTR edge, then immediately blasts the verified bootloader knock
"4DGL" at 115200 and watches for the reply "db16" (chip is in the bootloader
window => the reset WORKED, unambiguously, no eyeballing needed). It sends ONLY
the knock — no loader/flash bytes — so nothing is programmed.

A camera frame (bench-vision on the Jetson) is grabbed after each attempt as a
visual backup.

Usage:  python3 -u reset_knock.py [port]      (default /dev/ttyUSB0)
"""
import sys, time, subprocess, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
KNOCK = b"4DGL"
REPLY = b"db16"
CAM = "http://192.168.1.96:8080/snapshot.jpg"

def grab(tag):
    path = f"/tmp/knock_{tag}.jpg"
    try:
        subprocess.run(["curl", "-sS", "--max-time", "5", "-o", path, CAM],
                       check=True, capture_output=True)
        return path
    except Exception as e:
        return f"(cam grab failed: {e})"

def knock_window(ser, duration=0.5, interval=0.015):
    """Blast 4DGL repeatedly for `duration`, return all bytes read."""
    deadline = time.monotonic() + duration
    buf = bytearray()
    nextw = 0.0
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= nextw:
            ser.write(KNOCK)
            nextw = now + interval
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            if REPLY in buf:
                break
        time.sleep(0.002)
    return bytes(buf)

def attempt(ser, label, pre_dtr, pre_rts, edge_line, edge_to, dwell=0.05):
    print(f"\n>>> {label}")
    ser.dtr = pre_dtr
    ser.rts = pre_rts
    time.sleep(dwell)
    ser.reset_input_buffer()
    # the deliberate edge:
    setattr(ser, edge_line, edge_to)
    # strace showed PMMCLOADER waits ~21ms after the DTR edge before 4DGL:
    time.sleep(0.020)
    resp = knock_window(ser)
    hexd = " ".join(f"{b:02x}" for b in resp[:32])
    got = REPLY in resp
    print(f"    sent 4DGL burst; rx {len(resp)}B: [{hexd}]")
    print(f"    {'*** db16 — BOOTLOADER REACHED (reset worked!) ***' if got else 'no db16'}")
    cam = grab(label.replace(' ', '_').replace('/', '_')[:40])
    print(f"    camera frame: {cam}")
    return got

def main():
    print(f"reset_knock on {PORT} @115200 — DTR/RTS edge + 4DGL knock, watch for db16")
    print("=" * 70)
    grab("baseline")
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        combos = [
            # label, pre_dtr, pre_rts, edge_line, edge_to
            ("DTR rising edge (assert->deassert) [schematic-predicted]", True,  False, "dtr", False),
            ("DTR falling edge (deassert->assert)",                       False, False, "dtr", True),
            ("RTS rising edge (assert->deassert) [fallback]",             False, True,  "rts", False),
            ("RTS falling edge (deassert->assert) [fallback]",            False, False, "rts", True),
        ]
        hits = []
        for c in combos:
            if attempt(ser, *c):
                hits.append(c[0])
            time.sleep(0.5)
        print("\n" + "=" * 70)
        if hits:
            print("RESET CONFIRMED via:", "; ".join(hits))
        else:
            print("No db16 from any edge. Either the edge isn't reaching the DTR")
            print("pin (cp210x not toggling it), or timing/polarity is off. Check")
            print("the camera frames /tmp/knock_*.jpg for a visual blank/re-splash.")
        ser.dtr = False; ser.rts = False
    finally:
        ser.close()

if __name__ == "__main__":
    main()
