#!/usr/bin/env python3
"""
gpio_reset_knock.py — A1 follow-up: reset the Diablo16 via a CP2104 *GPIO* line.

WHY: A1 proved the modem-DTR path is a dead end with certainty —
  - meter: the C8/DTR pad does not move when Linux toggles DTR;
  - usbmon (bus4): the host DOES send SET_MHS (41 07 0101/0100, DTR bit only)
    and the chip ACKs success — yet the pin stays put.
A modem pin must obey SET_MHS; this one doesn't. So the reset net is almost
certainly wired to one of the CP2104's GPIO pins (GPIO.0-3), which Linux drives
through the cp210x *gpiochip* (a vendor LATCH command) — a different USB request
than SET_MHS. The Silabs Windows driver toggles that GPIO via its own API, which
is why the unit resets on Windows but not via pyserial's ser.dtr.

This sweeps each cp210x GPIO line (gpiochip588, lines 588-591), pulses it in both
edge directions through the AC-coupled reset cap (C8 -> Q1 -> RESET), then knocks
"4DGL" @115200 watching for the bootloader reply "db16". Fully automated — no
meter, no paperclip. The db16 reply is the unambiguous "reset worked" signal.

sysfs GPIO writes need root; this shells out via sudo (passwordless on this box).
Usage:  python3 -u gpio_reset_knock.py [port]   (default /dev/ttyUSB0)
"""
import sys, time, subprocess, serial

PORT  = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
BASE  = 588                       # gpiochip588 (cp210x) base
LINES = [589, 590, 591, 588]      # GPIO.1,2,3 first (GPIO.0 already fairly tested -ve), then GPIO.0 last
KNOCK = b"4DGL"
REPLY = b"db16"

def sh(cmd):
    """Run a shell command via sudo; return (rc, output)."""
    p = subprocess.run(["sudo", "bash", "-c", cmd], capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()

def export(line):
    # ignore "already exported" (16/EBUSY)
    sh(f"echo {line} > /sys/class/gpio/export 2>/dev/null; "
       f"echo out > /sys/class/gpio/gpio{line}/direction")

def unexport(line):
    sh(f"echo {line} > /sys/class/gpio/unexport 2>/dev/null")

def setval(line, v):
    sh(f"echo {v} > /sys/class/gpio/gpio{line}/value")

def knock_for_db16(ser, secs=0.4):
    buf = bytearray()
    t0 = time.monotonic()
    nextw = 0.0
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

def attempt(ser, line, rising):
    """Pulse `line` to make one edge, then knock. rising=True -> 0->1 edge."""
    setval(line, 0 if rising else 1)   # idle level
    time.sleep(0.05)
    ser.reset_input_buffer()
    setval(line, 1 if rising else 0)   # THE EDGE -> through C8 -> Q1 -> RESET
    # knock CONTINUOUSLY long enough for the slow chip to reach its bootloader
    # (manual paperclip only showed db16 under a continuous knock; 0.4s was too short)
    ok, buf = knock_for_db16(ser, secs=5.0)
    setval(line, 0)                    # park low
    return ok, buf

def main():
    print(f"gpio_reset_knock on {PORT}: sweeping cp210x GPIO lines {LINES}")
    print("=" * 64)
    ser = serial.Serial(PORT, baudrate=115200, bytesize=8, parity='N', stopbits=1,
                        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        for line in LINES:
            export(line)
            for rising in (True, False):
                edge = "0->1 (rising)" if rising else "1->0 (falling)"
                ok, buf = attempt(ser, line, rising)
                nz = bytes(b for b in buf if b != 0)
                tag = " ".join(f"{b:02x}" for b in nz[:12]) if nz else "(silence/0x00)"
                star = "   *** db16 — BOOTLOADER REACHED ***" if ok else ""
                print(f"  GPIO.{line-BASE} (line {line}) edge {edge}: rx {len(buf):3d}B {tag}{star}")
                if ok:
                    print(f"\nSUCCESS: pulsing cp210x GPIO.{line-BASE} resets the Diablo16.")
                    print("Fix = drive a reset pulse on this GPIO (gpiochip), not DTR/SET_MHS.")
                    unexport(line)
                    return
            unexport(line)
        print("\nNo db16 on any GPIO edge. Either no GPIO reaches RESET, or the knock")
        print("missed the window. Next: meter C8 while holding each GPIO each way to see")
        print("which line (if any) actually moves the reset pad.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
