import os

for mod, pkg in [('app','android'), ('capture','capture'), ('accessory','accessory')]:
    for root, dirs, fs in os.walk(rf'C:\MirageWork\MirageVulkan\android\{mod}\src'):
        for f in fs:
            if f == 'Config.kt':
                p = os.path.join(root, f)
                with open(p,'rb') as fh: t = fh.read().decode('utf-8','replace')
                for line in t.splitlines():
                    if any(x in line for x in ['HOST','PORT','IP','host','port']):
                        print(f'{mod}: {line.strip()}')
