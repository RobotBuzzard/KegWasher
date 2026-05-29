#!/usr/bin/env python3
"""SPE smoke test — sends visible draw commands to a Diablo16-based 4D
display via the SPE runtime command set.

Use this after flashing a fresh PmmC + Driver to confirm the panel
works. The screen is blank because no 4XE program is loaded yet — this
script directly issues SPE drawing commands to verify the chip + panel
are alive end-to-end.

Diablo16 SPE commands (verified via Serial Commander 2026-05-28):
  gfx_Cls()         : 0xFF82                       (was 0xFFCD in v1 — wrong)
  gfx_CircleFilled  : 0xFF77 <x-BE16> <y-BE16> <r-BE16> <colour-BE16>
  gfx_BGcolour(c)   : 0xFF6E <colour-BE16>
  txt_FGcolour(c)   : 0xFF7F <colour-BE16>
  putstr(str)       : 0x0018 <ASCII bytes> 0x00    (null-terminated)

All command bytes are big-endian. ACK is 0x06, NAK is 0x15.

CAT5 to ClearCore MUST be disconnected — only this script talks to the
chip during the test.
"""
import sys
import time
import struct
import serial

PORT = "/dev/ttyUSB0"

# RGB565 colour packing helper.
def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

RED   = rgb565(255, 0, 0)
GREEN = rgb565(0, 255, 0)
BLUE  = rgb565(0, 0, 255)
WHITE = rgb565(255, 255, 255)
BLACK = 0

def send_cmd(ser, *parts, expect_ack=True, label="cmd"):
    """Send one SPE command (concatenated big-endian bytes) and read ACK."""
    payload = b"".join(
        p if isinstance(p, (bytes, bytearray)) else struct.pack(">H", p)
        for p in parts
    )
    ser.write(payload)
    ser.flush()
    if not expect_ack:
        return None
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        b = ser.read(1)
        if b:
            code = b[0]
            tag = {0x06: "ACK", 0x15: "NAK"}.get(code, f"0x{code:02x}")
            print(f"  {label}: {tag}")
            return code
    print(f"  {label}: NO RESPONSE (timeout)")
    return None

def main():
    baud = int(sys.argv[1]) if len(sys.argv) > 1 else 9600
    print(f"SPE smoke test on {PORT} @ {baud} baud")
    print("=" * 60)
    ser = serial.Serial(PORT, baudrate=baud, bytesize=8, parity="N", stopbits=1,
                        timeout=0.2, rtscts=False, dsrdtr=False, xonxoff=False)
    ser.dtr = False
    ser.rts = False
    ser.reset_input_buffer()
    ser.reset_output_buffer()

    # SPE expects no preamble — it's stateful, starts after PmmC boots
    # and the first byte received is the first command opcode.

    # 1. Clear to BLACK (default), should ACK.
    print("\n[1] gfx_Cls (clear to current bg)")
    send_cmd(ser, 0xFF82, label="gfx_Cls")
    time.sleep(0.5)

    # 2. Set bg to RED and clear — screen should turn solid red.
    print("\n[2] gfx_BGcolour(RED) then gfx_Cls — screen should go RED")
    send_cmd(ser, 0xFF6E, RED, label="BGcolour RED")
    send_cmd(ser, 0xFF82, label="gfx_Cls")
    time.sleep(2.0)

    # 3. Filled green circle in the middle — most visible test.
    print("\n[3] gfx_CircleFilled green at (200, 130) r=60 — should see green disc")
    send_cmd(ser, 0xFF77, 200, 130, 60, GREEN, label="CircleFilled GREEN")
    time.sleep(2.0)

    # 4. Reset bg to BLACK, white text "HELLO" via null-terminated putstr.
    print("\n[4] BLACK bg + white 'HELLO' text")
    send_cmd(ser, 0xFF6E, BLACK, label="BGcolour BLACK")
    send_cmd(ser, 0xFF82, label="gfx_Cls")
    send_cmd(ser, 0xFF7F, WHITE, label="txt_FGcolour WHITE")
    # putstr: opcode 0x0018, then null-terminated ASCII (no length prefix)
    send_cmd(ser, 0x0018, b"HELLO\x00", label="putstr HELLO")
    time.sleep(2.0)

    ser.close()
    print()
    print("Done. Verdict:")
    print("  - All ACK + you saw red/blue/black + HELLO → panel + chip + driver OK")
    print("  - All ACK but no visible change → panel/backlight problem (driver or hardware)")
    print("  - NAK/no-response → wrong baud OR chip not in SPE mode")

if __name__ == "__main__":
    main()
