import os

def count_lines(path):
    try:
        with open(path, 'rb') as f: return len(f.read().decode('utf-8','replace').splitlines())
    except: return 0

mods = ['app', 'capture', 'accessory']
base = r'C:\MirageWork\MirageVulkan\android'
for mod in mods:
    src = os.path.join(base, mod, 'src', 'main', 'java')
    total_lines = 0
    files = []
    for root, dirs, fs in os.walk(src):
        for f in fs:
            if f.endswith('.kt'):
                p = os.path.join(root, f)
                n = count_lines(p)
                total_lines += n
                rel = p.replace(src + os.sep, '')
                files.append((n, rel))
    files.sort(reverse=True)
    print(f'=== {mod} ({len(files)} files, {total_lines} lines) ===')
    for n, rel in files:
        print(f'  {n:4d}  {rel}')
