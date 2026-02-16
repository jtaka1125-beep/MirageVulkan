import subprocess, time, random, socket, threading

serial = "A9250700956"
scid = f"{random.randint(0x20000000, 0x7FFFFFFF):08x}"
port = 27183

print(f"SCID: {scid}")

subprocess.run(["adb", "-s", serial, "forward", f"tcp:{port}", f"localabstract:scrcpy_{scid}"], check=True)

proc = subprocess.Popen(
    ["adb", "-s", serial, "shell",
     f"CLASSPATH=/data/local/tmp/scrcpy-server.jar",
     "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.4",
     "tunnel_forward=true", "audio=false", "control=false",
     "raw_stream=true", "max_size=720", "video_bit_rate=2000000",
     "max_fps=30", "cleanup=false", f"scid={scid}"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)

time.sleep(2)

if proc.poll() is not None:
    print("Server exited:", proc.stdout.read().decode(errors="replace"))
    exit(1)

print("Server running, connecting TCP...")
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", port))

total_bytes = 0
nal_count = 0
start = time.time()
duration = 5  # Read for 5 seconds

while time.time() - start < duration:
    try:
        data = s.recv(65536)
        if not data:
            break
        total_bytes += len(data)
        # Count NAL start codes
        nal_count += data.count(b'\x00\x00\x00\x01')
    except socket.timeout:
        break

elapsed = time.time() - start
print(f"Duration: {elapsed:.1f}s")
print(f"Total bytes: {total_bytes} ({total_bytes*8/elapsed/1e6:.2f} Mbps)")
print(f"NAL units: {nal_count} ({nal_count/elapsed:.1f} NAL/s)")

s.close()
proc.terminate()
print("Done")
