#!/usr/bin/env python3
"""
reset_test.py — find a host-driven RESET sequence for the gen4-uLCD-43DT.

Premise: the adapter's CP2104 DTR->Q1->RESET circuit DOES work (it resets the
display under Windows + the Silabs VCP driver). So a host-commandable reset is
possible here too; we just have to find the line/polarity/timing that the Linux
cp210x + pyserial stack must use to match what Windows does.

This walks through DTR / RTS / combined pulses in BOTH polarities and pauses
after each so you can watch the display and report which (if any) causes a
RESET (screen blanks + re-shows the splash / boots).

The manual RESET->GND short pulls the pin LOW and resets, so we're looking for
whichever host action drives the RESET net LOW via Q1.

Usage:  python3 -u reset_test.py [port]   (default /dev/ttyUSB0)
Nothing here flashes anything — it only toggles DTR/RTS modem lines.
"""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"

def pause(msg):
    try:
        input(f"\n  >>> {msg}\n      (watch the display, then press Enter) ")
    except EOFError:
        time.sleep(3)

def main():
    print(f"RESET line characterization on {PORT}")
    print("=" * 64)
    print("For each test: I set an idle state, hold ~1.5s, then PULSE the line")
    print("for the stated duration, then hold the idle state again. Watch the")
    print("display during the PULSE. Tell me the test number(s) that RESET it.")
    print("=" * 64)

    # timeout=0 non-blocking; we only manipulate modem control lines.
    ser = serial.Serial(PORT, baudrate=115200, timeout=0,
                         rtscts=False, dsrdtr=False, xonxoff=False)
    try:
        tests = [
            # (label, line, idle_value, pulse_value, pulse_seconds)
            ("DTR pulse to ASSERTED (True), 100ms",  "dtr", False, True,  0.10),
            ("DTR pulse to DEASSERTED (False), 100ms","dtr", True,  False, 0.10),
            ("DTR pulse to ASSERTED (True), 500ms",  "dtr", False, True,  0.50),
            ("DTR pulse to DEASSERTED (False), 500ms","dtr", True,  False, 0.50),
            ("RTS pulse to ASSERTED (True), 100ms",  "rts", False, True,  0.10),
            ("RTS pulse to DEASSERTED (False), 100ms","rts", True,  False, 0.10),
            ("RTS pulse to ASSERTED (True), 500ms",  "rts", False, True,  0.50),
            ("RTS pulse to DEASSERTED (False), 500ms","rts", True,  False, 0.50),
        ]
        for i, (label, line, idle, pulse, secs) in enumerate(tests, 1):
            pause(f"TEST {i}: {label} — ready?")
            # establish idle
            setattr(ser, line, idle)
            # park the other line deasserted so it isn't a confound
            other = "rts" if line == "dtr" else "dtr"
            setattr(ser, other, False)
            time.sleep(1.5)
            # pulse
            setattr(ser, line, pulse)
            time.sleep(secs)
            setattr(ser, line, idle)
            print(f"      TEST {i} pulse done.")

        # combined DTR+RTS, both polarities
        pause("TEST 9: DTR+RTS BOTH pulse to True, 200ms — ready?")
        ser.dtr = False; ser.rts = False; time.sleep(1.5)
        ser.dtr = True;  ser.rts = True;  time.sleep(0.2)
        ser.dtr = False; ser.rts = False
        print("      TEST 9 pulse done.")

        pause("TEST 10: DTR+RTS BOTH pulse to False (from True), 200ms — ready?")
        ser.dtr = True;  ser.rts = True;  time.sleep(1.5)
        ser.dtr = False; ser.rts = False; time.sleep(0.2)
        ser.dtr = True;  ser.rts = True
        print("      TEST 10 pulse done.")

        # leave lines deasserted on exit
        ser.dtr = False; ser.rts = False
        print("\nDone. Report which TEST number(s) reset the display.")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
