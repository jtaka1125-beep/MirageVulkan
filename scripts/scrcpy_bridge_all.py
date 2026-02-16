"""
scrcpy_bridge_all.py - Launch scrcpy bridge for all 3 devices
Each device gets: scrcpy TCP port -> UDP port for MirrorReceiver
"""
import subprocess, sys, os, time, signal

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BRIDGE_SCRIPT = os.path.join(SCRIPT_DIR, "scrcpy_bridge.py")

# Device serial -> (scrcpy_tcp_port, udp_target_port)
# UDP ports must match what MirrorReceiver is listening on
DEVICES = {
    "A9250700956":  (27183, 0),   # UDP port 0 = auto (will be filled by arg)
    "A9250700479":  (27184, 0),
    "93020523431940": (27185, 0),
}

def main():
    udp_ports = sys.argv[1:4] if len(sys.argv) >= 4 else ["61502", "61503", "61504"]
    
    devs = list(DEVICES.items())
    procs = []
    
    for i, (serial, (tcp_port, _)) in enumerate(devs):
        udp_port = udp_ports[i] if i < len(udp_ports) else str(61502 + i)
        print(f"Starting bridge: {serial} tcp={tcp_port} udp={udp_port}")
        p = subprocess.Popen(
            [sys.executable, BRIDGE_SCRIPT, serial, str(tcp_port), udp_port],
            stdout=sys.stdout, stderr=sys.stderr
        )
        procs.append((serial, p))
        time.sleep(1)  # Stagger starts
    
    print(f"\n=== All {len(procs)} bridges running. Press Ctrl+C to stop ===\n")
    
    try:
        while True:
            # Check if any died
            for serial, p in procs:
                if p.poll() is not None:
                    print(f"[{serial}] Bridge exited with code {p.returncode}")
            time.sleep(5)
    except KeyboardInterrupt:
        print("\nShutting down...")
        for serial, p in procs:
            p.terminate()
        for serial, p in procs:
            p.wait(timeout=5)
        print("All bridges stopped")


if __name__ == "__main__":
    main()
