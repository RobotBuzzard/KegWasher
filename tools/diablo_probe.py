#!/usr/bin/env python3
# Diablo16 bootloader probe — manual-reset edition v3.
#
# Detects silence gaps (>200ms no bytes) — those mark the moment the chip is
# in reset / bootloader window. Reports any non-NAK byte instantly.
#
# Usage:
#   python3 -u diablo_probe.py [baud]   # 9600 default
import sys
import time
import serial

PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 9600
HANDSHAKE = b"4dglU"
SEND_INTERVAL_S = 0.050
SILENCE_THRESHOLD_S = 0.200   # gap > this = chip in reset / bootloader

def main():
    baud = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_BAUD
    print(f"Diablo16 manual-reset probe on {PORT} at {baud} baud")
    print(f"Sending {HANDSHAKE!r} every {SEND_INTERVAL_S*1000:.0f} ms")
    print(f"Silence threshold: {SILENCE_THRESHOLD_S*1000:.0f} ms")
    print("=" * 70)
    print(">> READY. Short RESET to GND on the 5-way header now. <<")
    print("(Ctrl-C to stop.)")
    print()

    ser = serial.Serial(
        PORT, baudrate=baud, bytesize=8, parity='N', stopbits=1,
        timeout=0, rtscts=False, dsrdtr=False, xonxoff=False,
    )
    ser.dtr = False
    ser.rts = False
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    next_send = 0.0
    last_byte_t = time.monotonic()
    in_silence = False
    nak_total = 0
    start = time.monotonic()

    try:
        while True:
            now = time.monotonic()
            elapsed = now - start
            if now >= next_send:
                ser.write(HANDSHAKE)
                next_send = now + SEND_INTERVAL_S
            n = ser.in_waiting
            if n:
                # Bytes coming in = chip is talking
                if in_silence:
                    gap_ms = (now - last_byte_t) * 1000
                    print(f"!!! [t={elapsed:6.3f}s] CHIP CAME BACK after {gap_ms:.0f}ms silence !!!")
                    in_silence = False
                data = ser.read(n)
                last_byte_t = now
                for b in data:
                    if b == 0x15:
                        nak_total += 1
                    elif b == 0x06:
                        print(f"*** [t={elapsed:6.3f}s] ACK 0x06 — BOOTLOADER REACHED ***")
                    else:
                        print(f"  [t={elapsed:6.3f}s] non-NAK byte: 0x{b:02x} ({chr(b) if 32<=b<127 else '.'!r})")
            else:
                # No bytes — check if we've crossed silence threshold
                if not in_silence and (now - last_byte_t) > SILENCE_THRESHOLD_S:
                    print(f">>> [t={elapsed:6.3f}s] SILENCE began (NAK total so far: {nak_total})")
                    in_silence = True
            time.sleep(0.002)
    except KeyboardInterrupt:
        print()
        print("=" * 70)
        print(f"Stopped after {time.monotonic()-start:.1f}s, total NAKs: {nak_total}")
    finally:
        ser.close()

if __name__ == "__main__":
    main()
