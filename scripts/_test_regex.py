import re
cmd = 'shell CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 3.3.4 tunnel_forward=true audio=false control=false raw_stream=true max_size=720 video_bit_rate=2000000 max_fps=30 cleanup=false scid=20000029'
pattern = r'\$\(|\`|;\s*rm|;\s*dd|>\s*/|<\s*/|\|\s*sh|\|\s*bash'
m = re.search(pattern, cmd, re.IGNORECASE)
print(f"Match: {m}")
if m:
    print(f"Matched: [{m.group()}] at pos {m.start()}-{m.end()}")
    print(f"Context: ...{cmd[max(0,m.start()-10):m.end()+10]}...")

# Also test push and pkill
cmds = [
    'push tools/scrcpy-server-v3.3.4 /data/local/tmp/scrcpy-server.jar',
    'shell pkill -f scrcpy 2>/dev/null',
    'forward tcp:27183 localabstract:scrcpy_00000001',
]
for c in cmds:
    m = re.search(pattern, c, re.IGNORECASE)
    print(f"  '{c[:50]}...' -> Match: {m}")
    if m:
        print(f"  Matched: [{m.group()}]")
