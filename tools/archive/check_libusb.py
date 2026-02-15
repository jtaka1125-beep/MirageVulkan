import sys, os, traceback
sys.stdout.reconfigure(encoding='utf-8')
print('Python:', sys.executable)

# Check libusb0.dll locations
for p in [r'C:\Windows\System32\libusb0.dll', 
          r'C:\MirageWork\MirageComplete\driver_installer\usb_driver\amd64\libusb0.dll']:
    print(f'{p}: exists={os.path.exists(p)}')

# Check drivers
drv = r'C:\Windows\System32\drivers\libusb0.sys'
print(f'{drv}: exists={os.path.exists(drv)}')

# Try loading DLL
import ctypes
try:
    dll = ctypes.CDLL('libusb0.dll')
    print('libusb0.dll loaded via ctypes: OK')
except Exception as e:
    print(f'libusb0.dll load error: {e}')

# Try pyusb
try:
    import usb.core
    import usb.backend.libusb0 as lb0
    be = lb0.get_backend()
    print(f'pyusb libusb0 backend: {be}')
    if be:
        devs = list(usb.core.find(find_all=True, backend=be))
        print(f'Devices via libusb0: {len(devs)}')
        for d in devs:
            print(f'  VID={d.idVendor:04X} PID={d.idProduct:04X}')
    else:
        print('No libusb0 backend - trying default')
        devs = list(usb.core.find(find_all=True))
        print(f'Devices via default: {len(devs)}')
        for d in devs:
            print(f'  VID={d.idVendor:04X} PID={d.idProduct:04X}')
except Exception as e:
    traceback.print_exc()
