import sys
p = r'C:\MirageWork\MirageVulkan\src\route_controller.cpp'
with open(p, 'rb') as f:
    raw = f.read()
c = raw.decode('utf-8')
idx = c.find('usb_congested_ewma =')
sys.stdout.write(repr(c[idx:idx+250]) + '\n')
