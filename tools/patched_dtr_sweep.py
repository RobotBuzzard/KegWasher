#!/usr/bin/env python3
"""
patched_dtr_sweep.py — find the reset pin using the patched cp210x driver.

The patched driver mirrors DTR onto a chosen CP2104 GPIO (module param
reset_gpio, 0-3) via WRITE_LATCH — bypassing the gpiochip valid_mask, so it can
drive the reserved/push-pull lines too. For each (gpio, polarity), we set the
param, toggle DTR (which now pulses that GPIO -> through C8 -> Q1 -> RESET), and
knock 4DGL @115200 continuously watching for db16.

A hit means: that GPIO is the reset pin, and DTR now physically resets the chip
on Linux -> Workshop4 under Wine (same driver) auto-resets -> Bank0 flash works.

Run as a normal user (uses sudo to poke /sys/module params). pyserial owns the tty.
"""
import sys, time, subprocess, serial

PORT  = "/dev/ttyUSB0"
KNOCK = b"4DGL"
REPLY = b"db16"
GPIOS = [2, 1, 3, 0]     # 2 = push-pull (best edge) first
INVERTS = [0, 1]
KNOCK_S = 6.0

def set_param(name, val):
    subprocess.run(["sudo", "bash", "-c",
                    f"echo {val} > /sys/module/cp210x/parameters/{name}"], check=True)

def knock(ser, secs):
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
    print("patched_dtr_sweep: reset_gpio x polarity, DTR toggle + continuous knock")
    print("=" * 66)
    for g in GPIOS:
        for inv in INVERTS:
            set_param("reset_gpio", g)
            set_param("reset_invert", inv)
            # fresh open => DTR asserted on open (one edge via patch)
            ser = serial.Serial(PORT, 115200, timeout=0, rtscts=False,
                                dsrdtr=False, xonxoff=False)
            try:
                # explicit DTR edges too, so we don't rely only on open-time
                ser.dtr = False; time.sleep(0.05)
                ser.dtr = True;  time.sleep(0.05)   # assert edge
                ser.dtr = False; time.sleep(0.05)   # deassert edge
                ser.dtr = True                      # assert again
                ser.reset_input_buffer()
                hit = knock(ser, KNOCK_S)
            finally:
                ser.dtr = False
                ser.close()
            tag = "*** db16 — RESET! ***" if hit else "no db16"
            print(f"  reset_gpio={g} invert={inv}: {tag}")
            if hit:
                print(f"\nSUCCESS: DTR now resets via cp210x GPIO {g} (invert={inv}).")
                print(f"Persist with: options cp210x reset_gpio={g} reset_invert={inv}")
                set_param("reset_gpio", g); set_param("reset_invert", inv)
                return
    print("\nNo db16 on any GPIO/polarity via the patch. The mirror-DTR-onto-GPIO")
    print("approach didn't reach the reset (open-drain pins can't drive the edge, or")
    print("the reset isn't a latch-drivable GPIO). Next: reconfigure pin to push-pull.")
    set_param("reset_gpio", -1)

if __name__ == "__main__":
    main()
