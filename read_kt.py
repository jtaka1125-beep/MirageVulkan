import os

for root, dirs, fs in os.walk(r'C:\MirageWork\MirageVulkan\android\capture\src'):
    for f in fs:
        if any(x in f for x in ['Boot','Watchdog','Activity','Config']):
            p = os.path.join(root, f)
            print(f'=== {f} ({os.path.getsize(p)}B) ===')
            with open(p,'rb') as fh: print(fh.read().decode('utf-8','replace'))
            print()
