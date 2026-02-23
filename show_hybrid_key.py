
p=r'C:\MirageWork\MirageVulkan\src\hybrid_command_sender.cpp'
with open(p,'rb') as f: c=f.read().decode('utf-8')
lines=c.splitlines()
# Show key sections
for start,end in [(147,170),(294,340),(319,360)]:
    print(f'\n--- lines {start}-{end} ---')
    for i in range(start-1, min(end, len(lines))):
        print(f'{i+1}: {lines[i]}')
