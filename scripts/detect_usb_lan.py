#!/usr/bin/env python3
"""
Detect USB-LAN (USB Tethering) Network

Detects when an Android device is connected via USB tethering
and provides network information for communication.

Usage:
    python detect_usb_lan.py [--wait] [--json]

When USB tethering is enabled:
- Android acts as DHCP server
- PC gets IP in 192.168.42.x range
- Android IP is typically 192.168.42.129
"""

import socket
import subprocess
import sys
import json
import time
import argparse
import platform
from typing import Optional, Dict, List

# USB Tethering network characteristics
TETHER_SUBNET = "192.168.42"
ANDROID_DEFAULT_IP = "192.168.42.129"
COMMAND_PORT = 50000
VIDEO_PORT = 60000


def get_network_interfaces_windows() -> List[Dict]:
    """Get network interfaces on Windows"""
    interfaces = []
    try:
        result = subprocess.run(
            ['ipconfig', '/all'],
            capture_output=True, text=True, timeout=10
        )

        current_adapter = None
        current_info = {}

        for line in result.stdout.split('\n'):
            line = line.strip()

            # New adapter section
            if line and not line.startswith(' ') and ':' in line:
                if current_adapter and current_info.get('ipv4'):
                    interfaces.append(current_info)
                current_adapter = line.rstrip(':')
                current_info = {'name': current_adapter, 'ipv4': None, 'gateway': None}

            # IPv4 Address
            elif 'IPv4' in line and ':' in line:
                parts = line.split(':')
                if len(parts) >= 2:
                    ip = parts[1].strip().replace('(Preferred)', '').strip()
                    current_info['ipv4'] = ip

            # Default Gateway
            elif 'Default Gateway' in line and ':' in line:
                parts = line.split(':')
                if len(parts) >= 2:
                    gw = parts[1].strip()
                    if gw:
                        current_info['gateway'] = gw

        # Don't forget the last adapter
        if current_adapter and current_info.get('ipv4'):
            interfaces.append(current_info)

    except Exception as e:
        print(f"Error getting interfaces: {e}", file=sys.stderr)

    return interfaces


def get_network_interfaces_unix() -> List[Dict]:
    """Get network interfaces on Unix/Linux/macOS"""
    interfaces = []
    try:
        result = subprocess.run(
            ['ip', 'addr'],
            capture_output=True, text=True, timeout=10
        )

        import re
        current_intf = None
        current_info = {}

        for line in result.stdout.split('\n'):
            # Interface line
            match = re.match(r'^\d+:\s+(\S+):', line)
            if match:
                if current_intf and current_info.get('ipv4'):
                    interfaces.append(current_info)
                current_intf = match.group(1)
                current_info = {'name': current_intf, 'ipv4': None, 'gateway': None}

            # IPv4 address
            match = re.search(r'inet\s+(\d+\.\d+\.\d+\.\d+)', line)
            if match and current_intf:
                current_info['ipv4'] = match.group(1)

        if current_intf and current_info.get('ipv4'):
            interfaces.append(current_info)

    except FileNotFoundError:
        # Try ifconfig as fallback
        try:
            result = subprocess.run(
                ['ifconfig'],
                capture_output=True, text=True, timeout=10
            )

            import re
            current_intf = None
            current_info = {}

            for line in result.stdout.split('\n'):
                if line and not line.startswith(' ') and not line.startswith('\t'):
                    if current_intf and current_info.get('ipv4'):
                        interfaces.append(current_info)
                    parts = line.split(':')
                    current_intf = parts[0]
                    current_info = {'name': current_intf, 'ipv4': None, 'gateway': None}

                match = re.search(r'inet\s+(\d+\.\d+\.\d+\.\d+)', line)
                if match and current_intf:
                    current_info['ipv4'] = match.group(1)

            if current_intf and current_info.get('ipv4'):
                interfaces.append(current_info)

        except Exception as e:
            print(f"Error getting interfaces: {e}", file=sys.stderr)

    return interfaces


def detect_usb_tethering() -> Optional[Dict]:
    """
    Detect USB tethering connection.
    Returns dict with connection info or None if not found.
    """
    if platform.system() == 'Windows':
        interfaces = get_network_interfaces_windows()
    else:
        interfaces = get_network_interfaces_unix()

    # Look for interface with 192.168.42.x IP
    for intf in interfaces:
        ip = intf.get('ipv4', '')
        if ip and ip.startswith(TETHER_SUBNET):
            return {
                'detected': True,
                'interface': intf['name'],
                'pc_ip': ip,
                'android_ip': ANDROID_DEFAULT_IP,
                'command_port': COMMAND_PORT,
                'video_port': VIDEO_PORT,
                'subnet': TETHER_SUBNET + '.0/24'
            }

    return None


def check_android_reachable(android_ip: str, timeout: float = 2.0) -> bool:
    """Check if Android device is reachable via TCP ping"""
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(timeout)
        result = sock.connect_ex((android_ip, COMMAND_PORT))
        sock.close()
        return result == 0
    except (OSError, socket.error, socket.timeout):
        return False


def wait_for_tethering(timeout: int = 60, interval: float = 2.0) -> Optional[Dict]:
    """Wait for USB tethering to be detected"""
    print(f"Waiting for USB tethering... (timeout: {timeout}s)")
    start_time = time.time()

    while time.time() - start_time < timeout:
        result = detect_usb_tethering()
        if result:
            # Verify Android is reachable
            if check_android_reachable(result['android_ip']):
                result['android_reachable'] = True
                return result
            else:
                result['android_reachable'] = False
                print(f"Found tethering network but Android not reachable on port {COMMAND_PORT}")
                print("Make sure TcpCommandServer is running on Android")

        time.sleep(interval)
        sys.stdout.write('.')
        sys.stdout.flush()

    print("\nTimeout waiting for USB tethering")
    return None


def main():
    parser = argparse.ArgumentParser(description='Detect USB Tethering Network')
    parser.add_argument('--wait', action='store_true', help='Wait for tethering to be detected')
    parser.add_argument('--timeout', type=int, default=60, help='Wait timeout in seconds')
    parser.add_argument('--json', action='store_true', help='Output in JSON format')
    parser.add_argument('--check-port', action='store_true', help='Also check if command port is open')

    args = parser.parse_args()

    if args.wait:
        result = wait_for_tethering(args.timeout)
    else:
        result = detect_usb_tethering()
        if result and args.check_port:
            result['android_reachable'] = check_android_reachable(result['android_ip'])

    if args.json:
        if result:
            print(json.dumps(result, indent=2))
        else:
            print(json.dumps({'detected': False}))
    else:
        if result:
            print("\n=== USB Tethering Detected ===")
            print(f"Interface:    {result['interface']}")
            print(f"PC IP:        {result['pc_ip']}")
            print(f"Android IP:   {result['android_ip']}")
            print(f"Command Port: {result['command_port']}")
            print(f"Video Port:   {result['video_port']}")
            if 'android_reachable' in result:
                status = "OK" if result['android_reachable'] else "NOT REACHABLE"
                print(f"TCP Server:   {status}")
            print()
            print("To control Android:")
            print(f"  python usb_lan_controller.py --host {result['android_ip']} interactive")
            print()
            print("To view video:")
            print(f"  python udp_video_viewer.py  (port {result['video_port']})")
        else:
            print("USB Tethering not detected")
            print()
            print("To enable USB tethering on Android:")
            print("  1. Connect USB cable to PC")
            print("  2. Settings > Network > Hotspot & tethering > USB tethering")
            print("  3. Start MirageAndroid app and enable USB-LAN mode")

    sys.exit(0 if result else 1)


if __name__ == '__main__':
    main()
