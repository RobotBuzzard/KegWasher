#!/usr/bin/env python3
"""
bin2uf2.py — convert a ClearCore app .bin to a .uf2 for the UF2 bootloader.

App flashes at 0x4000 (bootloader owns 0x0-0x4000). UF2 family ID is read from
the bootloader's own CURRENT.UF2 (authoritative); falls back to the SAMD51/
SAME5x standard 0x55114460 if not available.

Usage: bin2uf2.py <app.bin> <out.uf2> [current_uf2_for_familyid]
"""
import struct, sys, os

APP_BASE = 0x4000
M0, M1, MEND = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
DEFAULT_FAMILY = 0x55114460

def family_from_current(path):
    try:
        with open(path, "rb") as f:
            hdr = f.read(32)
        if len(hdr) == 32:
            m0, m1, flags, target, paylen, blkno, numblk, last = struct.unpack("<8I", hdr)
            if m0 == M0 and m1 == M1 and (flags & FLAG_FAMILY):
                print(f"  family ID from CURRENT.UF2: 0x{last:08X} (block base 0x{target:08X})")
                return last
            print("  CURRENT.UF2 has no family flag; using default")
    except Exception as e:
        print(f"  no usable CURRENT.UF2 ({e}); using default family")
    return DEFAULT_FAMILY

def main():
    binp, outp = sys.argv[1], sys.argv[2]
    cur = sys.argv[3] if len(sys.argv) > 3 else None
    family = family_from_current(cur) if cur else DEFAULT_FAMILY
    data = open(binp, "rb").read()
    payload, out = 256, bytearray()
    total = (len(data) + payload - 1) // payload
    for i in range(total):
        chunk = data[i*payload:(i+1)*payload]
        blk = bytearray(512)
        struct.pack_into("<I", blk, 0, M0)
        struct.pack_into("<I", blk, 4, M1)
        struct.pack_into("<I", blk, 8, FLAG_FAMILY)
        struct.pack_into("<I", blk, 12, APP_BASE + i*payload)
        struct.pack_into("<I", blk, 16, len(chunk))
        struct.pack_into("<I", blk, 20, i)
        struct.pack_into("<I", blk, 24, total)
        struct.pack_into("<I", blk, 28, family)
        blk[32:32+len(chunk)] = chunk
        struct.pack_into("<I", blk, 508, MEND)
        out += blk
    open(outp, "wb").write(out)
    print(f"  wrote {outp}: {len(out)} bytes, {total} blocks, app base 0x{APP_BASE:08X}, family 0x{family:08X}")

if __name__ == "__main__":
    main()
