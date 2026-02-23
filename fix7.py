p = r'C:\MirageWork\MirageVulkan\android\accessory\build.gradle.kts'
with open(p,'rb') as f:
    content = f.read().decode('utf-8')
content2 = content.replace('minSdk = 26', 'minSdk = 29')
if content2 != content:
    with open(p,'wb') as f:
        f.write(content2.encode('utf-8'))
    print("FIX-7: minSdk 26 -> 29 OK")
else:
    print("ERROR: not found")
