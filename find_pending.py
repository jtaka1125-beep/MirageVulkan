
p = r'C:\MirageWork\MirageVulkan\src\hybrid_command_sender.cpp'
with open(p,'rb') as f: c=f.read().decode('utf-8').replace('\r\n','\n')
lines=c.splitlines()
for i,l in enumerate(lines,1):
    if '_pending' in l:
        print(f'{i}: {l}')
