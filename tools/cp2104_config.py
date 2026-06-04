#!/usr/bin/env python3
"""
cp2104_config.py — read the CP2104's pin configuration over raw USB (pyusb).

Pure config READ — issues no pin writes, so it cannot reset/disturb the display.
Replicates the vendor reads the cp210x driver does in cp2104_gpioconf_init(), to
learn each GPIO's alt-function and drive mode, and the live latch. Goal: decide
whether the DTR-reset pin is a GPIO we could drive via raw WRITE_LATCH, or a
dedicated/modem pin the Linux driver can't actuate.

Vendor requests (from drivers/usb/serial/cp210x.c):
  GET_PORTCONFIG : bmRequestType 0xC0, bRequest 0xFF, wValue 0x370C, 13 bytes
  GET_PARTNUM    : 0xC0, 0xFF, wValue 0x370B, 1 byte
  READ_LATCH     : 0xC0, 0xFF, wValue 0x00C2, 1 byte
device_cfg bits: 0=GPIO0 TXLED, 1=GPIO1 RXLED, 2=GPIO2 RS485 (alt-function/reserved)
gpio_pushpull = (gpio_mode >> 8) & 0xF   (1=push-pull, 0=open-drain)
"""
import sys, struct
import usb.core, usb.util

VID, PID = 0x10c4, 0xea60
IFACE = 0

def main():
    dev = usb.core.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("CP2104 not found (micro-USB unplugged?)"); sys.exit(1)

    detached = False
    try:
        try:
            partnum = bytes(dev.ctrl_transfer(0xC0, 0xFF, 0x370B, IFACE, 1))
        except usb.core.USBError:
            # kernel driver in the way -> detach (read-only; we reattach after)
            if dev.is_kernel_driver_active(IFACE):
                dev.detach_kernel_driver(IFACE); detached = True
            partnum = bytes(dev.ctrl_transfer(0xC0, 0xFF, 0x370B, IFACE, 1))

        cfg   = bytes(dev.ctrl_transfer(0xC0, 0xFF, 0x370C, IFACE, 13))
        latch = bytes(dev.ctrl_transfer(0xC0, 0xFF, 0x00C2, IFACE, 1))
    finally:
        if detached:
            usb.util.dispose_resources(dev)
            try: dev.attach_kernel_driver(IFACE)
            except Exception as e: print(f"(warning: reattach failed: {e})")

    print(f"PARTNUM   : 0x{partnum[0]:02x}  ({'CP2104' if partnum[0]==0x04 else 'other'})")
    print(f"PORTCONFIG raw ({len(cfg)}B): {cfg.hex(' ')}")
    if len(cfg) >= 13:
        gpio_mode    = struct.unpack_from('<H', cfg, 0)[0]
        reset_state  = struct.unpack_from('<H', cfg, 4)[0]
        suspend_state= struct.unpack_from('<H', cfg, 10)[0]
        device_cfg   = cfg[12]
    else:
        print("unexpected config length"); return
    pushpull = (gpio_mode >> 8) & 0xF
    reset_l  = reset_state & 0xF
    live     = latch[0] & 0xF
    altf = {0: device_cfg & 1, 1: (device_cfg>>1)&1, 2: (device_cfg>>2)&1, 3: 0}
    altname = {0:"TXLED", 1:"RXLED", 2:"RS485", 3:"-"}

    print(f"gpio_mode =0x{gpio_mode:04x}  reset_state=0x{reset_state:04x}  "
          f"suspend=0x{suspend_state:04x}  device_cfg=0x{device_cfg:02x}")
    print(f"live latch=0x{latch[0]:02x}  (pins 0-3 = {live:04b})")
    print()
    print("GPIO | altfunc      | drive       | reset-latch | live | driver dir")
    print("-----+--------------+-------------+-------------+------+-----------")
    for i in range(4):
        af  = f"YES ({altname[i]})" if altf[i] else "no"
        drv = "push-pull" if (pushpull>>i)&1 else "open-drain"
        rl  = (reset_l>>i)&1
        lv  = (live>>i)&1
        # driver marks input when open-drain AND reset latch=1 (per cp2104_gpioconf_init)
        ddir = "input(reserved)" if altf[i] else ("input" if (not (pushpull>>i)&1 and rl) else "output")
        print(f"  {i}  | {af:12} | {drv:11} | {rl:^11} | {lv:^4} | {ddir}")
    print()
    print("Interpretation: a pin we can drive via raw WRITE_LATCH must be a real GPIO")
    print("(not alt-function). Compare against the schematic's DTR->RESET net.")

if __name__ == "__main__":
    main()
