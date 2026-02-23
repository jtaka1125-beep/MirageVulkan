p = r'C:\MirageWork\MirageVulkan\src\usb_command_api.cpp'
with open(p,'rb') as f: lines = f.read().decode('utf-8','replace').splitlines()
print(f'=== usb_command_api.cpp ({len(lines)} lines) ===')
# First 80 lines for context, then function signatures
for i,l in enumerate(lines,1):
    if i <= 80:
        print(f'{i}: {l}')
