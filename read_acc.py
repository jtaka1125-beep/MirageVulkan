import os

targets = ['Boot','Activity','Config','UdpSender','DebugCmd']
for root, dirs, fs in os.walk(r'C:\MirageWork\MirageVulkan\android\accessory\src'):
    for f in fs:
        if any(x in f for x in targets):
            p = os.path.join(root, f)
            print(f'=== {f} ({os.path.getsize(p)}B) ===')
            with open(p,'rb') as fh: print(fh.read().decode('utf-8','replace'))
            print()
