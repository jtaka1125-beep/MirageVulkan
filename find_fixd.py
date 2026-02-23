
p=r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.cpp'
with open(p,'rb') as f: raw=f.read()
c=raw.decode('utf-8')
lines=c.splitlines()
print('total lines:', len(lines))
for i,l in enumerate(lines,1):
    if 'pending' in l.lower() or 'FIX-D' in l or 'process_pending' in l or 'PendingAck' in l or 'retry' in l.lower():
        print(f'{i}: {l}')
