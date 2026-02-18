"""
start_all_scrcpy.py - Launch scrcpy-server on all 3 WiFi ADB devices
Streams raw H.264 via UDP to MirrorReceiver ports
"""
import subprocess, socket, time, random, sys, os, threading, struct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SCRCPY_SERVER = os.path.join(SCRIPT_DIR, "..", "tools", "scrcpy-server-v3.3.4")

# WiFi ADB devices -> UDP target port
DEVICES = [
    ("192.168.0.6:5555", 61000),   # A9 #1
    ("192.168.0.8:5555", 61001),   # A9 #2
    ("192.168.0.3:5555", 61002),   # Npad X1
]

MTU = 1400

def send_nal_udp(udp_sock, dest, nal_data):
    """Send NAL unit via UDP with simple fragmentation"""
    if len(nal_data) <= MTU:
        udp_sock.sendto(nal_data, dest)
    else:
        # Strip start code, send fragments
        if nal_data[:4] == b'\x00\x00\x00\x01':
            sc = nal_data[:4]; payload = nal_data[4:]
        elif nal_data[:3] == b'\x00\x00\x01':
            sc = b'\x00\x00\x00\x01'; payload = nal_data[3:]
        else:
            sc = b'\x00\x00\x00\x01'; payload = nal_data
        
        chunk_size = MTU - 4
        for i in range(0, len(payload), chunk_size):
            chunk = payload[i:i+chunk_size]
            if i == 0:
                udp_sock.sendto(sc + chunk, dest)
            else:
                udp_sock.sendto(chunk, dest)


def bridge_device(serial, udp_port):
    """Start scrcpy-server and bridge TCP->UDP for one device"""
    scid = f"{random.randint(0x20000000, 0x7FFFFFFF):08x}"
    tcp_port = 27180 + udp_port % 10  # unique TCP port per device
    tag = serial.split(":")[0].split(".")[-1]  # last octet for brevity
    
    print(f"[{tag}] Starting scrcpy: serial={serial} scid={scid} tcp={tcp_port} -> udp={udp_port}")
    
    # Push server jar (idempotent)
    r = subprocess.run(["adb", "-s", serial, "push", SCRCPY_SERVER,
                        "/data/local/tmp/scrcpy-server.jar"],
                       capture_output=True, text=True, timeout=30)
    if r.returncode != 0:
        print(f"[{tag}] Push failed: {r.stderr}")
        return
    
    # ADB forward
    r = subprocess.run(["adb", "-s", serial, "forward", f"tcp:{tcp_port}",
                        f"localabstract:scrcpy_{scid}"],
                       capture_output=True, text=True, timeout=10)
    if r.returncode != 0:
        print(f"[{tag}] Forward failed: {r.stderr}")
        return
    
    # Kill any existing scrcpy process
    subprocess.run(["adb", "-s", serial, "shell", "pkill", "-f", "scrcpy"],
                   capture_output=True, timeout=5)
    time.sleep(0.5)
    
    # Start scrcpy-server
    server = subprocess.Popen(
        ["adb", "-s", serial, "shell",
         "CLASSPATH=/data/local/tmp/scrcpy-server.jar",
         "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.4",
         "tunnel_forward=true", "audio=false", "control=false",
         "raw_stream=true", "max_size=800", "video_bit_rate=2000000",
         "max_fps=30", "cleanup=false", f"scid={scid}"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    
    time.sleep(2)
    if server.poll() is not None:
        err = server.stdout.read().decode(errors="replace")
        print(f"[{tag}] Server died: {err[:200]}")
        return
    
    # TCP connect with retry
    tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_sock.settimeout(5)
    connected = False
    for attempt in range(15):
        try:
            tcp_sock.connect(("127.0.0.1", tcp_port))
            connected = True
            break
        except (ConnectionRefusedError, socket.timeout, OSError):
            time.sleep(0.5)
    
    if not connected:
        print(f"[{tag}] TCP connect failed after 15 attempts")
        server.terminate()
        subprocess.run(["adb", "-s", serial, "forward", "--remove", f"tcp:{tcp_port}"], capture_output=True)
        return
    
    # UDP sender
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_dest = ("127.0.0.1", udp_port)
    
    print(f"[{tag}] BRIDGE ACTIVE: TCP:{tcp_port} -> UDP:{udp_port}")
    
    # Bridge loop
    total_bytes = 0
    nal_count = 0
    start_time = time.time()
    buf = bytearray()
    
    try:
        while True:
            data = tcp_sock.recv(65536)
            if not data:
                print(f"[{tag}] TCP disconnected")
                break
            
            buf.extend(data)
            
            while True:
                idx = buf.find(b'\x00\x00\x00\x01')
                if idx < 0:
                    break
                
                next_idx = buf.find(b'\x00\x00\x00\x01', idx + 4)
                next_idx3 = buf.find(b'\x00\x00\x01', idx + 4)
                if next_idx3 >= 0 and (next_idx < 0 or next_idx3 < next_idx):
                    next_idx = next_idx3
                
                if next_idx < 0:
                    if len(buf) > 100000:
                        nal = bytes(buf[idx:])
                        send_nal_udp(udp_sock, udp_dest, nal)
                        nal_count += 1
                        total_bytes += len(nal)
                        buf.clear()
                    break
                
                nal = bytes(buf[idx:next_idx])
                send_nal_udp(udp_sock, udp_dest, nal)
                nal_count += 1
                total_bytes += len(nal)
                buf = buf[next_idx:]
                
                if nal_count % 100 == 0:
                    elapsed = time.time() - start_time
                    fps = nal_count / elapsed
                    mbps = total_bytes * 8 / elapsed / 1e6
                    print(f"[{tag}] NALs={nal_count} fps={fps:.1f} {mbps:.2f}Mbps")
    
    except Exception as e:
        print(f"[{tag}] Error: {e}")
    finally:
        elapsed = time.time() - start_time
        print(f"[{tag}] DONE: {nal_count} NALs in {elapsed:.1f}s")
        tcp_sock.close()
        udp_sock.close()
        server.terminate()
        subprocess.run(["adb", "-s", serial, "forward", "--remove", f"tcp:{tcp_port}"], capture_output=True)


def main():
    print("=== Starting scrcpy bridges for all devices ===")
    threads = []
    for serial, udp_port in DEVICES:
        t = threading.Thread(target=bridge_device, args=(serial, udp_port), daemon=True)
        t.start()
        threads.append(t)
        time.sleep(1)  # Stagger
    
    print(f"\n=== {len(threads)} bridges launching. Press Ctrl+C to stop ===\n")
    
    try:
        while True:
            alive = sum(1 for t in threads if t.is_alive())
            if alive == 0:
                print("All bridges exited")
                break
            time.sleep(5)
    except KeyboardInterrupt:
        print("\nStopping...")


if __name__ == "__main__":
    main()
