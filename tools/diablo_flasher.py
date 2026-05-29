#!/usr/bin/env python3
"""
diablo_flasher.py — Native-Linux PmmC + Driver flasher for 4D Systems
Diablo16 chips, via the 4D ClearCore Adaptor (whose DTR→RESET circuit
doesn't drive the chip's RESET pin and so prevents Workshop4's PmmC
Loader from completing a flash).

WORKFLOW (manual-reset variant):
  1. Run this tool.
  2. When prompted, briefly short RESET to GND on the adapter's 5-way
     header. The chip reboots into bootloader at 115200.
  3. Tool detects bootloader (chip replies 'D' = 0x44 to the standard
     "4dgl"+"U" handshake) and reads the current chip identity.
  4. Tool confirms identity matches the supplied PmmC/Driver files,
     prompts the operator to proceed, then flashes.
  5. Verify pass before declaring success. Power-cycle the display to
     run the new firmware.

PREREQUISITES:
  - /dev/ttyUSB0 is the CP2104 on the ClearCore Adaptor
  - CAT5 to the ClearCore is DISCONNECTED (else the ClearCore drives
    the same UART and corrupts the programming bytes; see CC Adaptor
    datasheet §3.4)
  - PmmC file (e.g. Diablo16-R30.PmmC) and Driver file
    (e.g. Gen4-uLCD-43DT-B-D210902.4Drv) available on disk

STATUS: SKELETON. The post-handshake protocol (chip ID readback, erase,
write, verify) is not yet implemented — that's blocked on either (a) an
open-source 4D PmmC loader implementation we can translate, or (b) a
fresh strace of Workshop4's PmmC Loader in a partial-success state.
"""
from __future__ import annotations

import argparse
import dataclasses
import struct
import sys
import time
from pathlib import Path
from typing import Optional

import serial

DEFAULT_PORT = "/dev/ttyUSB0"
DEFAULT_BAUD = 115200

# Standard 4D bootloader handshake. The chip replies 0x44 ('D') for
# the Diablo16 family on a successful bootloader entry.
HANDSHAKE = b"4dglU"
ACK_DIABLO16 = 0x44  # 'D' — chip-family identifier
ACK = 0x06           # generic ACK
NAK = 0x15           # generic NAK (runtime SPE2 PmmC sends this for unknown bytes)


# ---------------------------------------------------------------------------
# .PmmC / .4Drv file parsing.
#
# Empirically derived from xxd of Diablo16-R30.PmmC and
# Gen4-uLCD-43DT-B-D210902.4Drv. The headers are sequences of NUL-
# terminated ASCII strings followed by trailing bytes, with the header
# padded to a multiple of some boundary (8 or 16 bytes).
#
# .PmmC layout (245,777 bytes total for Diablo16-R30.PmmC):
#   field 1   : NUL-terminated chip-family (e.g. "Diablo16\0", 9 bytes)
#   field 2   : NUL-terminated version    (e.g. "3.0\0",      4 bytes)
#   padding   : zeros to byte 16
#   payload   : binary firmware blob from byte 16 onward (245,761 bytes)
#
# .4Drv layout (16,414 bytes total for Gen4-uLCD-43DT-B-D210902.4Drv):
#   field 1   : NUL-terminated display-model ("Gen4-uLCD-43DT-B\0", 17 bytes)
#   field 2   : NUL-terminated date         ("21/09/02\0",          9 bytes)
#   metadata  : 4-byte big-endian u32 (0x0003c000 for this driver) — appears
#               to encode the size of the companion PmmC payload field
#   payload   : binary blob from byte 30 onward (16,384 bytes — exactly 16 KiB)
# ---------------------------------------------------------------------------

# Empirical header sizes — the boundary between header and binary payload.
PMMC_HEADER_LEN = 16
DRIVER_HEADER_LEN = 30


@dataclasses.dataclass
class PmmCFile:
    family: str            # e.g. "Diablo16"
    version: str           # e.g. "3.0"
    payload: bytes
    raw: bytes             # full file contents, in case the protocol wants headers too


@dataclasses.dataclass
class DriverFile:
    model: str             # e.g. "Gen4-uLCD-43DT-B"
    date: str              # e.g. "21/09/02"
    metadata_u32: int      # 4-byte BE int — looks like companion-PmmC payload size
    payload: bytes
    raw: bytes


def _read_cstring(blob: bytes, offset: int) -> tuple[str, int]:
    """Read a NUL-terminated ASCII string starting at offset. Returns
    (text, length_including_nul)."""
    end = blob.find(b"\x00", offset)
    if end < 0:
        raise ValueError(f"unterminated string starting at offset {offset}")
    return blob[offset:end].decode("ascii"), (end - offset) + 1


def load_pmmc(path: Path) -> PmmCFile:
    raw = path.read_bytes()
    if len(raw) < PMMC_HEADER_LEN:
        raise ValueError(f"PmmC file too short: {len(raw)} bytes")
    family, n1 = _read_cstring(raw, 0)
    version, _ = _read_cstring(raw, n1)
    payload = raw[PMMC_HEADER_LEN:]
    return PmmCFile(family=family, version=version, payload=payload, raw=raw)


def load_driver(path: Path) -> DriverFile:
    raw = path.read_bytes()
    if len(raw) < DRIVER_HEADER_LEN:
        raise ValueError(f"Driver file too short: {len(raw)} bytes")
    model, n1 = _read_cstring(raw, 0)
    date, n2 = _read_cstring(raw, n1)
    # The 4-byte BE u32 immediately follows the two strings.
    u32_off = n1 + n2
    (metadata_u32,) = struct.unpack(">I", raw[u32_off:u32_off + 4])
    payload = raw[DRIVER_HEADER_LEN:]
    return DriverFile(model=model, date=date, metadata_u32=metadata_u32,
                      payload=payload, raw=raw)


# ---------------------------------------------------------------------------
# Serial / bootloader entry.
#
# The chip enters bootloader for ~1.5 seconds after RESET is released
# (per measurement). The bootloader's auto-baud locks on the 0x55 ('U')
# byte we send. As long as we send bytes regularly, the bootloader
# extends its window.
# ---------------------------------------------------------------------------

class BootloaderError(RuntimeError):
    pass


def open_port(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial(
        port,
        baudrate=baud,
        bytesize=8, parity="N", stopbits=1,
        timeout=0,
        rtscts=False, dsrdtr=False, xonxoff=False,
    )
    # Leave DTR/RTS deasserted — neither does anything useful on this
    # adapter for RESET; we rely on the operator's manual short instead.
    ser.dtr = False
    ser.rts = False
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def wait_for_bootloader(ser: serial.Serial, *, timeout_s: float = 30.0,
                        send_interval_s: float = 0.050) -> None:
    """Continuously emit HANDSHAKE until the chip replies ACK_DIABLO16.
    Operator is expected to short RESET→GND on the 5-way header during
    this window. Raises BootloaderError on timeout.
    """
    deadline = time.monotonic() + timeout_s
    next_send = 0.0
    buf = bytearray()
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now >= next_send:
            ser.write(HANDSHAKE)
            next_send = now + send_interval_s
        n = ser.in_waiting
        if n:
            buf.extend(ser.read(n))
            for b in buf:
                if b == ACK_DIABLO16:
                    return
        time.sleep(0.002)
    raise BootloaderError(
        f"No bootloader 'D' reply within {timeout_s}s. Did RESET-to-GND short happen?"
    )


# ---------------------------------------------------------------------------
# TODO: post-'D' protocol
# ---------------------------------------------------------------------------

def read_chip_identity(ser: serial.Serial) -> dict:
    """Query the chip for its current PmmC version + driver name/date.

    Workshop4's strace shows this is the next sequence after 'D', but we
    haven't extracted the exact bytes yet. Blocked on:
      (a) open-source PmmC Loader source (agent searching), or
      (b) fresh strace of Workshop4 with the manual-reset trick.
    """
    raise NotImplementedError("post-handshake protocol not yet captured")


def flash_pmmc(ser: serial.Serial, pmmc: PmmCFile) -> None:
    raise NotImplementedError("flash protocol not yet captured")


def flash_driver(ser: serial.Serial, drv: DriverFile) -> None:
    raise NotImplementedError("flash protocol not yet captured")


def verify(ser: serial.Serial, pmmc: PmmCFile, drv: DriverFile) -> bool:
    raise NotImplementedError("verify protocol not yet captured")


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    parser.add_argument("--port", default=DEFAULT_PORT)
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    parser.add_argument("--pmmc", type=Path, required=True,
                        help="path to .PmmC file (e.g. Diablo16-R30.PmmC)")
    parser.add_argument("--driver", type=Path, required=True,
                        help="path to .4Drv file (e.g. Gen4-uLCD-43DT-B-D210902.4Drv)")
    parser.add_argument("--dry-run", action="store_true",
                        help="parse files and enter bootloader, but skip writing")
    args = parser.parse_args(argv)

    pmmc = load_pmmc(args.pmmc)
    drv = load_driver(args.driver)

    print(f"PmmC:   {pmmc.family} v{pmmc.version} ({len(pmmc.payload)} byte payload)")
    print(f"Driver: {drv.model} dated {drv.date}, metadata_u32=0x{drv.metadata_u32:08x}, "
          f"{len(drv.payload)} byte payload")
    print()

    print(f"Opening {args.port} @ {args.baud}...")
    ser = open_port(args.port, args.baud)
    try:
        print()
        print(">>> SHORT RESET→GND ON THE 5-WAY HEADER NOW <<<")
        print("    (Brief touch is enough. Bootloader window is ~1.5s.)")
        wait_for_bootloader(ser)
        print(">>> Bootloader reached. <<<")
        print()

        if args.dry_run:
            print("--dry-run: stopping before flash.")
            return 0

        print("ERROR: post-handshake flash protocol not yet implemented.")
        print("Next steps: extract Workshop4's byte sequence via strace, OR translate")
        print("the open-source PmmC Loader Qt project. See the plan file.")
        return 2
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
