import os

files = [
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\ScreenCaptureService.kt',
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\ipc\AccessoryCommandReceiver.kt',
    r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt',
    r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt',
]

for f in files:
    print(f'=== {os.path.basename(f)} ({os.path.getsize(f)}B) ===')
    with open(f, 'rb') as fh: t = fh.read().decode('utf-8', errors='replace')
    for kw in ['attachUsbStream','detachUsbStream','UsbVideoSender','OutputStream','fun ','50200']:
        idxs = []
        start = 0
        while True:
            i = t.find(kw, start)
            if i == -1: break
            line = t[:i].count('\n') + 1
            idxs.append(line)
            start = i + 1
        if idxs:
            print(f'  {kw}: lines {idxs}')
