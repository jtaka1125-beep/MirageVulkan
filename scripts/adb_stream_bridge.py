"""
ADB Screen Stream Bridge
Reads H.264 from `adb exec-out screenrecord` and sends via UDP to MirageVulkan receiver.
Usage: python adb_stream_bridge.py <serial> <udp_port> [width] [height] [bitrate]
"""
import subprocess
import socket
import sys
import time
import struct

def find_nal_units(data):
    """Split byte stream into NAL units by 00 00 00 01 start codes."""
    nals = []
    positions = []
    i = 0
    while i < len(data) - 3:
        if data[i] == 0 and data[i+1] == 0 and data[i+2] == 0 and data[i+3] == 1:
            positions.append(i)
            i += 4
        elif data[i] == 0 and data[i+1] == 0 and data[i+2] == 1:
            positions.append(i)
            i += 3
        else:
            i += 1
    
    for idx in range(len(positions)):
        start = positions[idx]
        end = positions[idx + 1] if idx + 1 < len(positions) else len(data)
        nals.append(data[start:end])
    
    remainder = b''
    if positions:
        # Keep last NAL as potential incomplete
        remainder = data[positions[-1]:]
        nals = nals[:-1]
    else:
        remainder = data
    
    return nals, remainder

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <serial> <udp_port> [width] [height] [bitrate]")
        sys.exit(1)
    
    serial = sys.argv[1]
    udp_port = int(sys.argv[2])
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 720
    height = int(sys.argv[4]) if len(sys.argv) > 4 else 1280
    bitrate = int(sys.argv[5]) if len(sys.argv) > 5 else 2000000
    
    target = ("127.0.0.1", udp_port)
    
    cmd = [
        "adb", "-s", serial,
        "exec-out",
        f"screenrecord --output-format=h264 --size {width}x{height} --bit-rate {bitrate} --time-limit 0 -"
    ]
    
    print(f"[Bridge] {serial} -> UDP:{udp_port} ({width}x{height} {bitrate//1000}kbps)")
    print(f"[Bridge] CMD: {' '.join(cmd)}")
    
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    while True:
        try:
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                     bufsize=0)
            print(f"[Bridge] ADB process started (PID {proc.pid})")
            
            buffer = b''
            frame_count = 0
            bytes_sent = 0
            start_time = time.time()
            
            while True:
                chunk = proc.stdout.read(8192)
                if not chunk:
                    break
                
                buffer += chunk
                nals, buffer = find_nal_units(buffer)
                
                for nal in nals:
                    # Send NAL unit as UDP packet (with start code)
                    if len(nal) <= 1400:
                        sock.sendto(nal, target)
                    else:
                        # Fragment large NALs
                        for i in range(0, len(nal), 1400):
                            frag = nal[i:i+1400]
                            sock.sendto(frag, target)
                    
                    frame_count += 1
                    bytes_sent += len(nal)
                    
                    if frame_count % 60 == 0:
                        elapsed = time.time() - start_time
                        if elapsed > 0:
                            fps = frame_count / elapsed
                            mbps = bytes_sent * 8 / elapsed / 1_000_000
                            print(f"[Bridge] {serial}: frames={frame_count} fps={fps:.1f} bitrate={mbps:.1f} Mbps")
            
            proc.wait()
            print(f"[Bridge] ADB process ended (frames={frame_count})")
            
        except KeyboardInterrupt:
            print(f"\n[Bridge] Stopped")
            break
        except Exception as e:
            print(f"[Bridge] Error: {e}")
        
        print(f"[Bridge] Restarting in 2s...")
        time.sleep(2)
    
    sock.close()

if __name__ == "__main__":
    main()
