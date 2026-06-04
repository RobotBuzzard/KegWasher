#!/usr/bin/env python3
"""
rts_long_knock.py — FAIR db16 test of RTS as the reset (long continuous knock).

The earlier CP2104-RTS test used 300-400ms knock windows (the same too-short bug
that gave false negatives on GPIO/DTR). The CP2104 has a real dedicated RTS pin
(push-pull, driven by SET_MHS CONTROL_RTS). If the reset is wired to RTS, a
proper long continuous knock after each RTS edge would catch db16. Re-test it
right. (Patch should be disabled: reset_gpio=-1, so DTR-mirror doesn't interfere.)

Knock continuously; toggle RTS every 6s through 4 edge types; attribute db16 to
the most recent RTS action.
"""
import sys, time, serial
PORT="/dev/ttyUSB0"; KNOCK=b"4DGL"; REPLY=b"db16"; SLOT=6.0
def main():
    ser=serial.Serial(PORT,115200,timeout=0,rtscts=False,dsrdtr=False,xonxoff=False)
    actions=[
        ("deassert edge (T->F)", lambda:(setattr(ser,'rts',True), time.sleep(0.05), setattr(ser,'rts',False))),
        ("assert edge (F->T)",   lambda:(setattr(ser,'rts',False),time.sleep(0.05), setattr(ser,'rts',True))),
        ("pulse low 5ms",        lambda:(setattr(ser,'rts',True), setattr(ser,'rts',False),time.sleep(0.005),setattr(ser,'rts',True))),
        ("pulse high 5ms",       lambda:(setattr(ser,'rts',False),setattr(ser,'rts',True), time.sleep(0.005),setattr(ser,'rts',False))),
    ]
    print(f"rts_long_knock on {PORT} @115200 — continuous knock, RTS edge every {SLOT:.0f}s")
    print("="*64)
    buf=bytearray(); nextw=0.0; cur=None; seen=False
    t0=time.monotonic(); next_action=0.0; ai=0
    try:
        while time.monotonic()-t0 < SLOT*len(actions)+1:
            now=time.monotonic()
            if now>=next_action and ai<len(actions):
                cur=actions[ai][0]; print(f"  [{now-t0:6.2f}s] RTS action: {cur}")
                actions[ai][1](); ai+=1; next_action=now+SLOT
            if now>=nextw:
                ser.write(KNOCK); nextw=now+0.010
            n=ser.in_waiting
            if n:
                buf.extend(ser.read(n))
                if REPLY in buf and not seen:
                    seen=True; print(f"  [{now-t0:6.2f}s] *** db16 — RESET via RTS ({cur}) ***"); break
                buf=buf[-8:]
            time.sleep(0.001)
    finally:
        ser.rts=False; ser.close()
    print("="*64)
    print("RESULT:", "db16 SEEN — RTS resets!" if seen else "NO db16 — RTS is not the reset either.")
if __name__=="__main__": main()
