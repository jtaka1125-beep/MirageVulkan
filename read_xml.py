import os

files_to_read = [
    r'C:\MirageWork\MirageVulkan\android\accessory\src\main\res\xml\mirage_accessibility.xml',
    r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\usb\Protocol.kt',
]
for p in files_to_read:
    print(f'=== {os.path.basename(p)} ===')
    with open(p,'rb') as f: print(f.read().decode('utf-8','replace'))
    print()
