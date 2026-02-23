
p_hpp = r'C:\MirageWork\MirageVulkan\src\adb_touch_fallback.hpp'
with open(p_hpp,'rb') as f: hpp = f.read().decode('utf-8').replace('\r\n','\n')
lines = hpp.splitlines()
for i,l in enumerate(lines,1):
    if 'AdbTouchFallback()' in l or 'thread' in l or 'queue' in l or '~Adb' in l:
        print(f'{i}: {l}')
print('---')
for i in range(max(0,len(lines)-20), len(lines)):
    print(f'{i+1}: {lines[i]}')
