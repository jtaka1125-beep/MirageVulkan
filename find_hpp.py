
p = r'C:\MirageWork\MirageVulkan\src\multi_usb_command_sender.hpp'
with open(p,'rb') as f: raw=f.read()
c = raw.decode('utf-8')
lines = c.splitlines()
for i,l in enumerate(lines,1):
    if 'ErrorStats' in l or 'pending' in l or 'total_errors' in l or 'queue_command' in l or 'send_raw' in l:
        print(f'{i}: {l}')
