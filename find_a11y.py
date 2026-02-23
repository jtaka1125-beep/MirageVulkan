
import os
base = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access'
for root,dirs,files in os.walk(base):
    for f in files:
        print(os.path.join(root,f))
