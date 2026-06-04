#!/usr/bin/env python3
"""
rts_reset_knock.py — test the CP2104's *RTS* line as a reset, PC-side.

Context: db16 detector is now VALIDATED (manual paperclip -> db16 on the native
pyserial knock). DTR (SET_MHS) is proven dead (pin doesn't move). RTS is the only
other software-driveable CP2104 modem output we haven't tried. SW1=RESET-EN ties
the RJ45 RTS (ClearCore side) to RESET; whether the CP2104's own RTS reaches that
net is unknown from the schematic — so test it empirically.

If any RTS edge yields db16: automated reset is achievable from the PC alone with
a one-line `ser.rts` pulse — no ClearCore programmer, no Wine. If not: certain
elimination, and the reset must come from the ClearCore RTS via SW1.

Tries both edge polarities at several hold widths, knocking 4DGL after each.
Requires SW1 = RESET-EN. Usage: python3 -u rts_reset_knock.py [port]
"""
import sys, time, serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
KNOCK = b"4DGL"
REPLY = b"db16"
WIDTHS = [0.002, 0.010, 0.050, 0.200]   # cover short edge .. long hold

def knock_for_db16(ser, secs=0.4):
    buf = bytearray()
    t0 = time.monotonic(); nextw = 0.0
    while time.monotonic() - t0 < secs:
        now = time.monotonic()
        if now >= nextw:
            ser.write(KNOCK); nextw = now + 0.010
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            if REPLY in buf:
                return True, bytes(buf)
        time.sleep(0.001)
    return False, bytes(buf)

def main():
    print(f"rts_reset_knock on {PORT} @115200 (SW1 must be RESET-EN)")
    print("=" * 64)
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        for rising in (True, False):
            for w in WIDTHS:
                edge = "deassert->assert" if rising else "assert->deassert"
                ser.rts = (not rising)        # idle level (opposite of the edge target)
                time.sleep(0.05)
                ser.reset_input_buffer()
                ser.rts = rising              # THE EDGE
                time.sleep(w)
                ok, buf = knock_for_db16(ser)
                ser.rts = False
                nz = bytes(b for b in buf if b != 0)
                tag = " ".join(f"{b:02x}" for b in nz[:12]) if nz else "(silence/0x00)"
                star = "   *** db16 — RESET via CP2104 RTS! ***" if ok else ""
                print(f"  RTS {edge:18s} hold {w*1000:5.0f}ms: rx {len(buf):3d}B {tag}{star}")
                if ok:
                    print("\nSUCCESS: CP2104 RTS resets the display. A pyserial RTS pulse is")
                    print("the automated reset — no ClearCore programmer or Wine needed.")
                    return
        print("\nNo db16 on any RTS edge — CP2104 RTS does not reach RESET (matches the")
        print("schematic: SW1 ties the *ClearCore* RJ45-RTS to RESET, not the CP2104's).")
        print("Next: drive the reset from the ClearCore (cc_reset_knock.ino).")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
