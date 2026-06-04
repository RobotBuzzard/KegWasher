#!/usr/bin/env python3
"""
reset_open_knock.py — exploit Linux's open-time DTR edge to reset the Diablo16.

Research finding (qsantos.fr + kernel tty_port): Linux ALWAYS asserts DTR/RTS on
open() with no way to suppress it. For the 4D AC/cap-coupled reset circuit
(CP2104 DTR -> 0.1uF -> Q1 -> RESET), that open-time edge IS a reset pulse. Our
earlier reset_knock.py toggled DTR *after* open + slept, so it likely missed the
bootloader window opened by open() itself.

This loops: open /dev/ttyUSB0 at 115200 (-> DTR edge -> reset), then IMMEDIATELY
blast "4DGL" for ~300 ms watching for "db16", then close. Each open is a fresh
reset attempt. Also clears HUPCL/CRTSCTS so the toggle isn't dropped (the
`failed set request 0x7 status: -32` flow-control trap).

No paperclip. Safe: only opens/closes + sends the bootloader knock (no flash).
Usage:  python3 -u reset_open_knock.py [iterations]   (default 20)
"""
import sys, time, termios, serial

PORT = "/dev/ttyUSB0"
KNOCK = b"4DGL"
REPLY = b"db16"

def clear_hupcl(fd):
    # belt-and-suspenders: drop HUPCL so the line isn't held in a reset loop
    attrs = termios.tcgetattr(fd)
    attrs[2] &= ~termios.HUPCL          # cflag
    termios.tcsetattr(fd, termios.TCSANOW, attrs)

def attempt(idx):
    # open with rtscts=False (CRTSCTS off -> SET_MHS path, toggle not dropped)
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        clear_hupcl(ser.fileno())
        # the open() above already raised DTR (the reset edge). Knock NOW.
        buf = bytearray()
        t0 = time.monotonic()
        nextw = 0.0
        while time.monotonic() - t0 < 0.3:
            now = time.monotonic()
            if now >= nextw:
                ser.write(KNOCK)
                nextw = now + 0.010
            n = ser.in_waiting
            if n:
                buf.extend(ser.read(n))
                if REPLY in buf:
                    return True, bytes(buf)
            time.sleep(0.001)
        return False, bytes(buf)
    finally:
        ser.close()

def main():
    iters = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    print(f"reset_open_knock on {PORT}: {iters} open->knock cycles, watch for db16")
    print("=" * 64)
    for i in range(1, iters + 1):
        ok, buf = attempt(i)
        nonzero = bytes(b for b in buf if b != 0)
        tag = " ".join(f"{b:02x}" for b in nonzero[:16]) if nonzero else "(only 0x00 / silence)"
        print(f"  cycle {i:2d}: rx {len(buf):3d}B  {tag}"
              + ("   *** db16 — BOOTLOADER REACHED ***" if ok else ""))
        if ok:
            print("\nSUCCESS: the open-time DTR edge resets the chip; knock at open works.")
            return
        time.sleep(0.3)
    print("\nNo db16 across all cycles — the open-time DTR edge is not resetting the chip.")
    print("Next: check `dmesg` for cp210x 'failed set request 0x7 status: -32' (crtscts trap),")
    print("and/or measure the DTR pin (tools/dtr_hold.py).")

if __name__ == "__main__":
    main()
