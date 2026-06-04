#!/usr/bin/env python3
"""
dtr_reset_knock.py — FAIR db16 test of the DTR auto-reset (long knock window).

Schematic (CC Adaptor R1.1, p.11) confirms the reset is the standard 4D/Arduino
DTR auto-reset: CP2104 DTR -> C8 (0.1uF) -> Q1 (MMBT3904) -> display RESET. So
an edge on DTR should pulse RESET. Every prior DTR test knocked only 300-400ms
after the edge — too short for this slow chip (the paperclip only showed db16
under a CONTINUOUS knock). knock_watch already covered the open-time DTR edge for
30s (negative); this covers explicit DTR TOGGLES with the same long window.

Method: knock 4DGL @115200 continuously the whole time; every 6s apply one DTR
action (a fresh edge); attribute any db16 to the most recent action. No meter.

  db16 appears after a DTR edge  -> DTR DOES reset; earlier negatives were the
                                    short-window bug; the fix is timing-level.
  no db16 across all edges (long window) -> DTR genuinely does not actuate the
                                    pin on Linux -> cp210x driver/chip-config gap.

Screen will blank if it works (that's success; USB-unplug recovers).
Usage:  python3 -u dtr_reset_knock.py [port]
"""
import sys, time, serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
KNOCK = b"4DGL"
REPLY = b"db16"
SLOT  = 6.0   # seconds of continuous knock per DTR action

def main():
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    # DTR actions: each lambda leaves DTR at a new level (an edge) or pulses it.
    actions = [
        ("deassert edge (True->False)", lambda: (setattr(ser, 'dtr', True),  time.sleep(0.05), setattr(ser, 'dtr', False))),
        ("assert edge (False->True)",   lambda: (setattr(ser, 'dtr', False), time.sleep(0.05), setattr(ser, 'dtr', True))),
        ("pulse low 5ms (F->T)",        lambda: (setattr(ser, 'dtr', True),  setattr(ser, 'dtr', False), time.sleep(0.005), setattr(ser, 'dtr', True))),
        ("pulse high 5ms (T->F)",       lambda: (setattr(ser, 'dtr', False), setattr(ser, 'dtr', True),  time.sleep(0.005), setattr(ser, 'dtr', False))),
    ]
    print(f"dtr_reset_knock on {PORT} @115200 — continuous knock, DTR edge every {SLOT:.0f}s")
    print("=" * 64)
    buf = bytearray()
    nextw = 0.0
    cur = None
    seen = False
    t0 = time.monotonic()
    next_action = 0.0
    ai = 0
    try:
        while time.monotonic() - t0 < SLOT * len(actions) + 1:
            now = time.monotonic()
            if now >= next_action and ai < len(actions):
                cur = actions[ai][0]
                print(f"  [{now-t0:6.2f}s] DTR action: {cur}")
                actions[ai][1]()
                ai += 1
                next_action = now + SLOT
            if now >= nextw:
                ser.write(KNOCK); nextw = now + 0.010
            n = ser.in_waiting
            if n:
                buf.extend(ser.read(n))
                if REPLY in buf and not seen:
                    seen = True
                    print(f"  [{now-t0:6.2f}s] *** db16 — RESET via DTR ({cur}) ***")
                    break
                buf = buf[-8:]
            time.sleep(0.001)
    finally:
        ser.dtr = False
        ser.close()
    print("=" * 64)
    if seen:
        print("RESULT: db16 SEEN — DTR DOES reset. Earlier negatives were the short")
        print("knock window. The Linux/Wine fix is timing-level, not a dead pin.")
    else:
        print("RESULT: NO db16 across all DTR edges with a long knock window.")
        print("DTR genuinely does not actuate the pin on Linux -> cp210x driver/config gap.")

if __name__ == "__main__":
    main()
