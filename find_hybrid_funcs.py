
p=r'C:\MirageWork\MirageVulkan\src\hybrid_command_sender.cpp'
with open(p,'rb') as f: raw=f.read()
c=raw.decode('utf-8')
lines=c.splitlines()
print('total lines:', len(lines))
# Find all function definitions
for i,l in enumerate(lines,1):
    if '::send_' in l or '::try_hid' in l or '::get_hid' in l:
        print(f'{i}: {l}')
