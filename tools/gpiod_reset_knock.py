#!/usr/bin/env python3
"""
gpiod_reset_knock.py — drive the cp210x GPIO lines PROPERLY (libgpiod) + knock.

Background: the legacy /sys/class/gpio interface silently failed on this kernel
(export of kernel-claimed lines errored; my earlier GPIO sweeps drove nothing).
gpioinfo shows cp210x gpiochip1: lines 0 & 2 are kernel-claimed inputs (TX/RX
LEDs), lines 1 & 3 are free outputs. So only 1 and 3 are user-drivable.

This uses `gpioset -t` to generate a continuous square wave (real USB WRITE_LATCH
edges through C8 -> Q1 -> RESET) on a line, while we knock 4DGL @115200 the whole
time watching for db16 — the proven-reliable continuous-knock detector.

  db16 while toggling line N  -> THAT gpio line is the reset; Linux software reset
                                 found (no DTR, no ClearCore, no Wine).
  no db16 on 1 or 3           -> the reset pin is NOT a user-drivable cp210x GPIO
                                 (likely a kernel-claimed line 0/2, or needs raw
                                 vendor access) -> next: pyusb / driver route.

Usage:  python3 -u gpiod_reset_knock.py [port]
"""
import sys, time, subprocess, serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
CHIP  = "gpiochip1"
LINES = [1, 3]            # the only user-drivable cp210x outputs
KNOCK = b"4DGL"
REPLY = b"db16"
SECS  = 8.0              # continuous knock per line while it toggles

def knock_watch(ser, secs):
    buf = bytearray(); t0 = time.monotonic(); nextw = 0.0
    while time.monotonic() - t0 < secs:
        now = time.monotonic()
        if now >= nextw:
            ser.write(KNOCK); nextw = now + 0.010
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            if REPLY in buf:
                return True
            buf = buf[-8:]
        time.sleep(0.001)
    return False

def main():
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    print(f"gpiod_reset_knock on {PORT}: toggling {CHIP} lines {LINES} (push-pull) + knock")
    print("=" * 64)
    try:
        for line in LINES:
            # continuous square wave on this line: toggle every 300ms, repeating
            p = subprocess.Popen(
                ["sudo", "gpioset", "-c", CHIP, "-t", "300ms", "--drive", "push-pull", f"{line}=1"],
                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            time.sleep(0.2)
            if p.poll() is not None:
                err = p.stderr.read().decode().strip()
                print(f"  line {line}: gpioset FAILED to drive ({err or 'exited early'})")
                continue
            ser.reset_input_buffer()
            ok = knock_watch(ser, SECS)
            p.terminate()
            try: p.wait(timeout=2)
            except Exception: p.kill()
            print(f"  line {line}: {'*** db16 — RESET via this GPIO! ***' if ok else 'no db16 (toggled cleanly, no reset)'}")
            if ok:
                print(f"\nSUCCESS: cp210x GPIO line {line} resets the Diablo16.")
                print("Fix = pulse this gpiochip line (gpioset/libgpiod), not DTR. No Wine, no ClearCore.")
                return
        print("\nNo db16 on the drivable GPIO lines (1, 3). The reset pin is not a")
        print("user-drivable cp210x GPIO — likely a kernel-claimed line (0/2) or needs")
        print("raw vendor access. Next: pyusb to drive the claimed line / read chip config.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
