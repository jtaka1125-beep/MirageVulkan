
import sys
p = r'C:\MirageWork\MirageVulkan\android\accessory\src\main\java\com\mirage\accessory\access\MirageAccessibilityService.kt'
with open(p,'rb') as f:
    raw = f.read()
print(f"size={len(raw)}, bom={raw[:4].hex()}", file=sys.stderr)
# try utf-8
kt = raw.decode('utf-8', errors='replace')
idx = kt.find('SYSTEM_DIALOG')
print(f"idx={idx}", file=sys.stderr)
if idx >= 0:
    print(kt[idx:idx+400], file=sys.stderr)
