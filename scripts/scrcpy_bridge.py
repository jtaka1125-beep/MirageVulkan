"""
scrcpy_bridge.py - Bridge scrcpy TCP stream to UDP for MirrorReceiver
Usage: python scrcpy_bridge.py <serial> <scrcpy_tcp_port> <udp_target_port>

Connects to scrcpy-server via ADB forward TCP, reads raw H.264,
sends NAL units to localhost UDP for MirrorReceiver consumption.
"""
import subprocess, socket, time, random, sys, threading, os

def main():
    if len(sys.argv) < 4:
        print(f"Usage: {sys.argv[0]} <serial> <scrcpy_tcp_port> <udp_target_port>")
        sys.exit(1)
    
    serial = sys.argv[1]
    tcp_port = int(sys.argv[2])
    udp_port = int(sys.argv[3])
    
    scid = f"{random.randint(0x20000000, 0x7FFFFFFF):08x}"
    print(f"[{serial}] SCID={scid} tcp={tcp_port} -> udp=localhost:{udp_port}")
    
    # Push server (idempotent)
    subprocess.run(["adb", "-s", serial, "push", 
                     os.path.join(os.path.dirname(__file__), "..", "tools", "scrcpy-server-v3.3.4"),
                     "/data/local/tmp/scrcpy-server.jar"],
                    capture_output=True)
    
    # ADB forward
    subprocess.run(["adb", "-s", serial, "forward", f"tcp:{tcp_port}", 
                     f"localabstract:scrcpy_{scid}"], check=True)
    
    # Start scrcpy-server
    server_proc = subprocess.Popen(
        ["adb", "-s", serial, "shell",
         "CLASSPATH=/data/local/tmp/scrcpy-server.jar",
         "app_process", "/", "com.genymobile.scrcpy.Server", "3.3.4",
         "tunnel_forward=true", "audio=false", "control=false",
         "raw_stream=true", "max_size=720", "video_bit_rate=2000000",
         "max_fps=30", "cleanup=false", f"scid={scid}"],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT
    )
    
    time.sleep(2)
    if server_proc.poll() is not None:
        err = server_proc.stdout.read().decode(errors="replace")
        print(f"[{serial}] Server failed: {err}")
        sys.exit(1)
    
    print(f"[{serial}] Server running, connecting TCP...")
    
    # TCP connect with retry
    tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    tcp_sock.settimeout(5)
    for attempt in range(10):
        try:
            tcp_sock.connect(("127.0.0.1", tcp_port))
            break
        except (ConnectionRefusedError, socket.timeout):
            time.sleep(0.5)
    else:
        print(f"[{serial}] TCP connect failed")
        server_proc.terminate()
        sys.exit(1)
    
    # UDP sender
    udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp_dest = ("127.0.0.1", udp_port)
    
    print(f"[{serial}] Bridging TCP:{tcp_port} -> UDP:{udp_port}")
    
    # Bridge loop: read H.264 from TCP, parse NAL units, send via UDP
    total_bytes = 0
    nal_count = 0
    start_time = time.time()
    buf = bytearray()
    MTU = 1400
    
    try:
        while True:
            data = tcp_sock.recv(65536)
            if not data:
                print(f"[{serial}] TCP disconnected")
                break
            
            buf.extend(data)
            
            # Find and send complete NAL units
            while True:
                # Find first start code
                idx = buf.find(b'\x00\x00\x00\x01')
                if idx < 0:
                    break
                
                # Find next start code
                next_idx = buf.find(b'\x00\x00\x00\x01', idx + 4)
                # Also check for 3-byte start code
                next_idx3 = buf.find(b'\x00\x00\x01', idx + 4)
                if next_idx3 >= 0 and (next_idx < 0 or next_idx3 < next_idx):
                    next_idx = next_idx3
                
                if next_idx < 0:
                    # No complete NAL yet, keep buffering
                    # But if buffer is huge, send what we have
                    if len(buf) > 100000:
                        nal = bytes(buf[idx:])
                        send_nal_udp(udp_sock, udp_dest, nal, MTU)
                        nal_count += 1
                        total_bytes += len(nal)
                        buf.clear()
                    break
                
                # Complete NAL unit from idx to next_idx
                nal = bytes(buf[idx:next_idx])
                send_nal_udp(udp_sock, udp_dest, nal, MTU)
                nal_count += 1
                total_bytes += len(nal)
                
                # Remove processed data
                buf = buf[next_idx:]
                
                # Stats every 60 NALs
                if nal_count % 60 == 0:
                    elapsed = time.time() - start_time
                    fps = nal_count / elapsed
                    mbps = total_bytes * 8 / elapsed / 1e6
                    print(f"[{serial}] NALs={nal_count} fps={fps:.1f} bitrate={mbps:.2f}Mbps")
    
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(f"[{serial}] Error: {e}")
    finally:
        elapsed = time.time() - start_time
        print(f"[{serial}] Total: {nal_count} NALs in {elapsed:.1f}s ({nal_count/max(elapsed,1):.1f} NAL/s)")
        tcp_sock.close()
        udp_sock.close()
        server_proc.terminate()
        # Remove forward
        subprocess.run(["adb", "-s", serial, "forward", "--remove", f"tcp:{tcp_port}"], capture_output=True)


def send_nal_udp(udp_sock, dest, nal_data, mtu=1400):
    """Send a NAL unit via UDP, fragmenting if necessary."""
    if len(nal_data) <= mtu:
        udp_sock.sendto(nal_data, dest)
    else:
        # Simple fragmentation: first chunk has start code, rest are raw
        offset = 0
        if nal_data[:4] == b'\x00\x00\x00\x01':
            start_code = nal_data[:4]
            payload = nal_data[4:]
        elif nal_data[:3] == b'\x00\x00\x01':
            start_code = b'\x00\x00\x00\x01'
            payload = nal_data[3:]
        else:
            start_code = b'\x00\x00\x00\x01'
            payload = nal_data
        
        chunk_size = mtu - 4  # Reserve space for start code in first packet
        for i in range(0, len(payload), chunk_size):
            chunk = payload[i:i+chunk_size]
            if i == 0:
                udp_sock.sendto(start_code + chunk, dest)
            else:
                udp_sock.sendto(chunk, dest)


if __name__ == "__main__":
    main()
