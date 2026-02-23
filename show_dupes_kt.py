
p = r'C:\MirageWork\MirageVulkan\android\capture\src\main\java\com\mirage\capture\usb\Protocol.kt'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
# Show ranges with duplicates
for i in range(29, 42):
    print(f'{i+1}: {lines[i]}')
print('...')
for i in range(92, 112):
    print(f'{i+1}: {lines[i]}')
print('...')
for i in range(200, 260):
    print(f'{i+1}: {lines[i]}')
