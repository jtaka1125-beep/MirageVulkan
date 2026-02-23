
import os, glob

# AccessoryIoService.kt を探す
base = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory'
for root,dirs,files in os.walk(base):
    for f in files:
        if 'AccessoryIoService' in f:
            print(os.path.join(root,f))
