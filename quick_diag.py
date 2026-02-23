import os
p = os.path.expandvars(r'%APPDATA%\MirageSystem\mirage_vulkan.log')
with open(p, 'r', encoding='utf-8', errors='replace') as f:
    lines = f.readlines()
print(f'Total: {len(lines)} lines')

print('\n=== MAINVIEW diag (all) ===')
for l in lines:
    if 'MAINVIEW diag' in l: print(l.rstrip())

print('\n=== stageUpdate (device breakdown) ===')
for l in lines:
    if 'stageUpdate' in l: print(l.rstrip())

print('\n=== Recorded texture uploads ===')
for l in lines:
    if 'Recorded' in l and 'texture' in l: print(l.rstrip())

print('\n=== VkTex Skipping/Mismatch ===')
for l in lines:
    if ('Skipping' in l or 'Size upgrade' in l or 'mismatch' in l) and 'VkTex' in l:
        print(l.rstrip())

print('\n=== beginFrame recent (fi/img distribution) ===')
bf = [l.rstrip() for l in lines if 'beginFrame' in l and 'fi=' in l]
print(f'  total beginFrame logs: {len(bf)}')
for l in bf[-5:]: print(' ', l)

print('\n=== Errors ===')
for l in lines:
    if '[ERROR]' in l: print(l.rstrip())
