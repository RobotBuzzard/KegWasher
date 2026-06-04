#!/usr/bin/env python3
"""
knock_watch.py — CONTROL experiment for the db16 detector.

Continuously knock "4DGL" @115200 and report, with timestamps, any non-0x00
bytes received and (loudly) any "db16" bootloader reply. The operator triggers
a KNOWN-GOOD reset by hand (short RESET->GND on the 5-pin header / paperclip)
a few times while this runs.

PURPOSE: every automated "no db16" result so far (DTR toggle, open-time edge,
GPIO pulse) is only meaningful if a native-Linux pyserial knock can detect db16
AT ALL. We have only ever seen db16 in the Wine/PMMCLOADER strace. This proves
(or disproves) the detector against the one reset we KNOW works.

INTERPRETATION:
  - Manual paperclip -> "db16" prints here  => detector VALID; DTR/GPIO really
    don't reset, and the automated-reset hunt continues on other mechanisms.
  - Manual paperclip -> still no db16 (only 0x00 / silence) => the native
    pyserial knock path itself is the broken variable (baud/line/timing); every
    prior "no db16" is a false negative and must be re-tested.

The steady 0x00 is expected: the display's 9600-baud runtime traffic mis-framed
at 115200. A real bootloader (115200) reply will stand out as ascii "db16".

Usage:  python3 -u knock_watch.py [port] [seconds]   (default /dev/ttyUSB0, 30s)
"""
import sys, time, serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
SECS  = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
REPLY = b"db16"

def main():
    print(f"knock_watch on {PORT} @115200 for {SECS:.0f}s — knocking 4DGL continuously.")
    print(">>> SHORT RESET->GND WITH THE PAPERCLIP a few times while this runs. <<<")
    print("=" * 64)
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    seen_db16 = False
    buf = bytearray()
    t0 = time.monotonic()
    nextw = 0.0
    last_report = 0.0
    nz_window = bytearray()
    try:
        while time.monotonic() - t0 < SECS:
            now = time.monotonic()
            if now >= nextw:
                ser.write(b"4DGL"); nextw = now + 0.010
            n = ser.in_waiting
            if n:
                chunk = ser.read(n)
                buf.extend(chunk)
                nz_window.extend(b for b in chunk if b != 0)
                if REPLY in buf and not seen_db16:
                    seen_db16 = True
                    print(f"  [{now-t0:6.2f}s] *** db16 — BOOTLOADER REACHED (detector WORKS) ***")
                buf = buf[-8:]  # keep tail for cross-chunk match
            # report any nonzero (ascii) activity ~4x/sec
            if now - last_report >= 0.25:
                if nz_window:
                    txt = "".join(chr(b) if 32 <= b < 127 else "." for b in nz_window[:24])
                    hx  = " ".join(f"{b:02x}" for b in nz_window[:12])
                    print(f"  [{now-t0:6.2f}s] rx nonzero ({len(nz_window)}B): '{txt}'  {hx}")
                    nz_window = bytearray()
                last_report = now
            time.sleep(0.001)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    print("=" * 64)
    print("RESULT:", "db16 SEEN — detector valid." if seen_db16
          else "NO db16 — if you DID short RESET, the native knock path is the broken variable.")

if __name__ == "__main__":
    main()
