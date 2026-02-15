#!/usr/bin/env python3
"""AOA Switch via libusb-win32 (libusb0) backend - bypasses WinUSB limitation"""

import usb.core
import usb.util
import usb.backend.libusb0 as libusb0_backend
import sys
import time

# AOA constants
AOA_VID = 0x18D1
AOA_GET_PROTOCOL = 51
AOA_SEND_STRING = 52
AOA_START_ACCESSORY = 53

AOA_STRING_MANUFACTURER = 0
AOA_STRING_MODEL = 1
AOA_STRING_DESCRIPTION = 2
AOA_STRING_VERSION = 3
AOA_STRING_URI = 4
AOA_STRING_SERIAL = 5

ANDROID_VIDS = [0x18D1, 0x04E8, 0x22B8, 0x0BB4, 0x12D1, 0x2717, 0x19D2,
                0x1004, 0x0FCE, 0x2A70, 0x0E8D, 0x1782, 0x1F3A, 0x2207]

AOA_PIDS = [0x2D00, 0x2D01, 0x2D02, 0x2D03, 0x2D04, 0x2D05]

def find_backend():
    """Try libusb0 backend first (works with filter driver)"""
    be = libusb0_backend.get_backend()
    if be:
        print("[OK] libusb0 backend found")
        return be
    print("[WARN] libusb0 backend not found, trying default")
    return None

def is_aoa(vid, pid):
    return vid == AOA_VID and pid in AOA_PIDS

def switch_to_aoa(dev):
    """Send AOA identification and start accessory mode"""
    try:
        # Detach kernel driver if needed
        for cfg in dev:
            for intf in cfg:
                if dev.is_kernel_driver_active(intf.bInterfaceNumber):
                    try:
                        dev.detach_kernel_driver(intf.bInterfaceNumber)
                    except:
                        pass
    except:
        pass

    # Get AOA protocol version
    try:
        version = dev.ctrl_transfer(
            usb.util.CTRL_IN | usb.util.CTRL_TYPE_VENDOR,
            AOA_GET_PROTOCOL, 0, 0, 2, timeout=1000
        )
        aoa_ver = version[0] | (version[1] << 8)
        print(f"  AOA protocol version: {aoa_ver}")
        if aoa_ver < 1:
            print("  ERROR: Device does not support AOA")
            return False
    except Exception as e:
        print(f"  ERROR getting AOA version: {e}")
        return False

    # Send accessory strings
    strings = [
        (AOA_STRING_MANUFACTURER, "Mirage"),
        (AOA_STRING_MODEL, "MirageCtl"),
        (AOA_STRING_DESCRIPTION, "Mirage Control Interface"),
        (AOA_STRING_VERSION, "1"),
        (AOA_STRING_URI, "https://github.com/mirage"),
        (AOA_STRING_SERIAL, "MirageCtl001"),
    ]
    for idx, s in strings:
        try:
            dev.ctrl_transfer(
                usb.util.CTRL_OUT | usb.util.CTRL_TYPE_VENDOR,
                AOA_SEND_STRING, 0, idx, s.encode('utf-8') + b'\x00', timeout=1000
            )
        except Exception as e:
            print(f"  ERROR sending string {idx}: {e}")
            return False
    print("  Accessory strings sent")

    # Start accessory mode
    try:
        dev.ctrl_transfer(
            usb.util.CTRL_OUT | usb.util.CTRL_TYPE_VENDOR,
            AOA_START_ACCESSORY, 0, 0, None, timeout=1000
        )
        print("  START_ACCESSORY sent - device will re-enumerate as AOA")
        return True
    except Exception as e:
        print(f"  ERROR starting accessory: {e}")
        return False

def main():
    print("=== AOA Switch (libusb0 backend) ===\n")

    backend = find_backend()

    # Find all Android devices
    devices = list(usb.core.find(find_all=True, backend=backend))
    
    switched = 0
    already_aoa = 0
    
    for dev in devices:
        vid, pid = dev.idVendor, dev.idProduct
        
        if is_aoa(vid, pid):
            print(f"[{dev.bus}:{dev.address}] VID={vid:04X} PID={pid:04X} - Already AOA")
            already_aoa += 1
            continue
        
        if vid not in ANDROID_VIDS:
            continue
        
        serial = "unknown"
        try:
            serial = dev.serial_number
        except:
            pass
        
        print(f"[{dev.bus}:{dev.address}] VID={vid:04X} PID={pid:04X} serial={serial}")
        
        if switch_to_aoa(dev):
            switched += 1
            print("  SUCCESS!")
        else:
            print("  FAILED")
    
    print(f"\n=== Summary ===")
    print(f"Already AOA: {already_aoa}")
    print(f"Switched: {switched}")
    
    if switched > 0:
        print("\nWaiting 3s for re-enumeration...")
        time.sleep(3)
        
        # Check for AOA devices
        aoa_devs = list(usb.core.find(find_all=True, idVendor=AOA_VID, backend=backend))
        aoa_count = sum(1 for d in aoa_devs if d.idProduct in AOA_PIDS)
        print(f"AOA devices found: {aoa_count}")

if __name__ == "__main__":
    main()
