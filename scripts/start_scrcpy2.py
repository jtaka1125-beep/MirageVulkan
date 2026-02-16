import subprocess, time, random

serial = "A9250700956"
scid = f"{random.randint(0x20000000, 0x7FFFFFFF):08x}"
port = 27183

print(f"Using SCID: {scid}")

# Forward
subprocess.run(["adb", "-s", serial, "forward", f"tcp:{port}", f"localabstract:scrcpy_{scid}"], check=True)

# Start server
proc = subprocess.Popen(
    ["adb", "-s", serial, "shell",
     f"CLASSPATH=/data/local/tmp/scrcpy-server.jar",
     "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.4",
     "tunnel_forward=true", "audio=false", "control=false",
     "raw_stream=true", "max_size=720", "video_bit_rate=2000000",
     "max_fps=30", "cleanup=false", f"scid={scid}"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)

time.sleep(3)
if proc.poll() is not None:
    out = proc.stdout.read().decode(errors="replace")
    print(f"EXITED code={proc.returncode}")
    print(out)
else:
    print(f"RUNNING PID={proc.pid}")
    # Try TCP connect
    import socket
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(2)
    try:
        s.connect(("127.0.0.1", port))
        data = s.recv(4096)
        print(f"Received {len(data)} bytes from scrcpy!")
        print(f"First 32 bytes: {data[:32].hex()}")
        s.close()
    except Exception as e:
        print(f"TCP connect error: {e}")
    
    proc.terminate()
    print("Server terminated")
