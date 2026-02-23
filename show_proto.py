
import sys
p = r'C:\MirageWork\MirageVulkan\src\mirage_protocol.hpp'
with open(p,'rb') as f: raw = f.read()
sys.stdout.buffer.write(raw[:500])
