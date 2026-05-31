#!/usr/bin/env python3
"""
dtr_hold.py — A1 measurement aid (Track A, the Wine-fix track).

Holds the CP2104 DTR (and optionally RTS) line in one state for a long dwell,
then the other, repeating — slow enough that a multimeter (not just a scope)
can read the level each way. The point is to MEASURE, with certainty, whether
Linux actually toggles the physical DTR pin, since the reset circuit is
  CP2104 DTR pin -> C8 0.1uF -> Q1 (MMBT3904) base -> RESET (active low).

WHERE TO PROBE (DC voltage, black lead on GND):
  - Best: the CP2104 DTR pin (pin 23) or the DTR-side pad of C8 (the cap's
    input, before the AC coupling). That node carries the DTR logic level.
  - Do NOT probe Q1's base or the RESET net for this: the cap blocks DC, so
    those sit at a steady level regardless of DTR. (RESET only dips on the
    EDGE; catching that needs a scope, not a DMM.)

INTERPRETATION:
  - DTR pad voltage CHANGES between the two phases  -> Linux IS driving the
    pin. The reset failure is downstream (polarity / edge / AC-timing). Fixable.
  - DTR pad voltage does NOT change                 -> cp210x is NOT moving the
    pin (driver issue on this kernel, or the chip's OTP/RS485 config). Different
    fix path (driver / chip config).

Usage:  python3 -u dtr_hold.py [port] [dwell_seconds] [--rts]
        defaults: /dev/ttyUSB0, 4 s dwell, DTR only.
Nothing is transmitted — only the modem control line is toggled.
"""
import sys, time, serial

def main():
    port = "/dev/ttyUSB0"
    dwell = 4.0
    do_rts = "--rts" in sys.argv
    args = [a for a in sys.argv[1:] if a != "--rts"]
    if len(args) >= 1:
        port = args[0]
    if len(args) >= 2:
        dwell = float(args[1])

    line = "RTS" if do_rts else "DTR"
    print(f"dtr_hold on {port}: toggling {line} every {dwell:.0f}s. Ctrl-C to stop.")
    print(f"Probe the {line} pad (DC volts vs GND) and note the level in each phase.")
    print("=" * 64)

    ser = serial.Serial(port, baudrate=115200, timeout=0,
                        rtscts=False, dsrdtr=False, xonxoff=False)
    # park both low to start
    ser.dtr = False
    ser.rts = False
    try:
        phase = True
        while True:
            if do_rts:
                ser.rts = phase
            else:
                ser.dtr = phase
            # pyserial: True = "asserted". On CP2104 the asserted state usually
            # drives the physical pin LOW, deasserted HIGH — but THAT is exactly
            # what we are measuring, so just report the logical state.
            print(f"  {line} = {'ASSERTED (pyserial True)' if phase else 'DEASSERTED (pyserial False)'}"
                  f"  -- measure now ({dwell:.0f}s)")
            time.sleep(dwell)
            phase = not phase
    except KeyboardInterrupt:
        ser.dtr = False
        ser.rts = False
        print("\nstopped.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
