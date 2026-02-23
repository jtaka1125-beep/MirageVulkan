import os

for root, dirs, fs in os.walk(r'C:\MirageWork\MirageVulkan\src'):
    for f in fs:
        if 'protocol' in f.lower() and f.endswith(('.hpp','.h','.cpp')):
            p = os.path.join(root, f)
            print(f'=== {p} ({os.path.getsize(p)}B) ===')
            with open(p,'rb') as fh: t = fh.read().decode('utf-8','replace')
            print(t[:3000])
