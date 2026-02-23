files = {
    'ScreenCaptureService.kt': (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\ScreenCaptureService.kt', 210, 245),
    'H264Encoder.kt': (r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\capture\H264Encoder.kt', 191, 230),
    'AccessoryIoService.kt': (r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\AccessoryIoService.kt', 140, 175),
}
for name, (path, s, e) in files.items():
    with open(path, 'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
    print(f'=== {name} L{s}-{e} ===')
    for i in range(s-1, min(e, len(lines))): print(f'{i+1}: {lines[i]}')
    print()
