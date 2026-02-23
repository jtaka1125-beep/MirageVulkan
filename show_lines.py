import codecs, re

p = r'C:\MirageWork\MirageVulkan\src\gui\gui_threads.cpp'
with codecs.open(p, 'r', 'utf-8-sig') as f:
    content = f.read()

lines = content.splitlines()
print(f'Total lines: {len(lines)}')
for i in range(485, min(545, len(lines))):
    print(f'{i+1}: {lines[i][:90]}')
