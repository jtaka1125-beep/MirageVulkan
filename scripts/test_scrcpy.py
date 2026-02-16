"""
Test scrcpy-server: start server, connect TCP, receive raw H.264, save to file.
"""
import subprocess, socket, time, sys, os

SERIAL = sys.argv[1] if len(sys.argv) > 1 else "A9250700956"
TCP_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 27183
SCID = sys.argv[3] if len(sys.argv) > 3 else "00000001"
SERVER_JAR = "/data/local/tmp/scrcpy-server.jar"
VERSION = "3.3.4"

print(f"[*] Testing scrcpy-server on {SERIAL}, TCP:{TCP_PORT}, SCID:{SCID}")

# Kill existing
subprocess.run(["adb", "-s", SERIAL, "shell", "pkill", "-f", "scrcpy"], 
               capture_output=True, timeout=5)
time.sleep(0.5)

# Setup adb forward
r = subprocess.run(["adb", "-s", SERIAL, "forward", f"tcp:{TCP_PORT}", 
                     f"localabstract:scrcpy_{SCID}"], 
                    capture_output=True, text=True, timeout=5)
print(f"[*] adb forward: {r.stdout.strip()} {r.stderr.strip()}")

# Start server
cmd = ["adb", "-s", SERIAL, "shell",
    f"CLASSPATH={SERVER_JAR}",
    "app_process", "/", "com.genymobile.scrcpy.Server", VERSION,
    "tunnel_forward=true", "audio=false", "control=false",
    "raw_stream=true", "max_size=720", "video_bit_rate=2000000",
    "max_fps=30", "cleanup=false", f"scid={SCID}"]
print(f"[*] Starting server...")
server_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
time.sleep(2)

if server_proc.poll() is not None:
    out = server_proc.stdout.read().decode(errors='replace')
    print(f"[!] Server exited: {out}")
    sys.exit(1)

# Connect TCP
print(f"[*] Connecting to localhost:{TCP_PORT}...")
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5)
connected = False
for attempt in range(10):
    try:
        sock.connect(("127.0.0.1", TCP_PORT))
        print(f"[+] Connected!")
        connected = True
        break
    except Exception as e:
        print(f"  retry {attempt+1}: {e}")
        time.sleep(0.5)

if not connected:
    print("[!] Failed to connect")
    server_proc.terminate()
    sys.exit(1)

# Receive data for 5 seconds
output_file = os.path.join("C:\\MirageWork\\MirageVulkan\\tools", f"test_{SERIAL}.h264")
total_bytes = 0
start_time = time.time()
sock.settimeout(2)

with open(output_file, "wb") as f:
    while time.time() - start_time < 5:
        try:
            data = sock.recv(65536)
            if not data:
                print("[!] Connection closed")
                break
            f.write(data)
            total_bytes += len(data)
            if total_bytes == len(data):
                hex_preview = ' '.join(f'{b:02x}' for b in data[:32])
                print(f"[+] First bytes: {hex_preview}")
                if data[:4] == b'\x00\x00\x00\x01':
                    nal_type = data[4] & 0x1F
                    print(f"[+] H.264 NAL type: {nal_type} (7=SPS, 8=PPS, 5=IDR)")
        except socket.timeout:
            continue

elapsed = time.time() - start_time
if total_bytes > 0:
    print(f"\n[+] SUCCESS: {total_bytes:,} bytes in {elapsed:.1f}s ({total_bytes*8/elapsed/1e6:.2f} Mbps)")
else:
    print(f"\n[!] No data received")
print(f"[*] Saved to {output_file}")

sock.close()
server_proc.terminate()
try:
    server_proc.wait(timeout=3)
except:
    server_proc.kill()

subprocess.run(["adb", "-s", SERIAL, "forward", "--remove", f"tcp:{TCP_PORT}"],
               capture_output=True, timeout=5)
print("[*] Done")
