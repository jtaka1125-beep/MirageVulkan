import subprocess, time, sys

serial = "A9250700956"
scid = "10000001"
port = 27183

# Forward
subprocess.run(["adb", "-s", serial, "forward", f"tcp:{port}", f"localabstract:scrcpy_{scid}"], check=True)

# Start server in background
proc = subprocess.Popen(
    ["adb", "-s", serial, "shell",
     f"CLASSPATH=/data/local/tmp/scrcpy-server.jar",
     "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.4",
     "tunnel_forward=true", "audio=false", "control=false",
     "raw_stream=true", "max_size=720", "video_bit_rate=2000000",
     "max_fps=30", "cleanup=false", f"scid={scid}"],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT
)

# Wait a bit and check output
time.sleep(3)
if proc.poll() is not None:
    out = proc.stdout.read().decode(errors="replace")
    print(f"EXITED with code {proc.returncode}")
    print(out)
else:
    print(f"RUNNING PID={proc.pid}")
    # Read available output
    import select
    proc.stdout.close()
    print("Server started OK")
