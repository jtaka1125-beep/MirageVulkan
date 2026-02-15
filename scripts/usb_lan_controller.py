#!/usr/bin/env python3
"""
USB-LAN Controller for MirageTestKit

Sends commands to Android device via TCP over USB tethering network.
Protocol matches mirage/usb/protocol.hpp

Usage:
    python usb_lan_controller.py [command] [args...]

Commands:
    ping                    - Send ping
    tap <x> <y>             - Tap at normalized coordinates (0.0-1.0)
    tap_abs <x> <y> <w> <h> - Tap at absolute coordinates
    back                    - Press back button
    key <keycode>           - Send keycode
    click_id <resource_id>  - Click UI element by resource ID
    click_text <text>       - Click UI element by text
    interactive             - Interactive mode
"""

import socket
import struct
import sys
import time
import argparse
from typing import Optional, Tuple

# Protocol constants (must match Protocol.kt)
MAGIC = 0x4D495241  # "MIRA" in little-endian
VERSION = 1
HEADER_SIZE = 14

CMD_PING = 0
CMD_TAP = 1
CMD_BACK = 2
CMD_KEY = 3
CMD_CONFIG = 4
CMD_CLICK_ID = 5
CMD_CLICK_TEXT = 6
CMD_ACK = 0x80

STATUS_OK = 0
STATUS_ERR_UNKNOWN_CMD = 1
STATUS_ERR_INVALID_PAYLOAD = 2
STATUS_ERR_BUSY = 3
STATUS_ERR_NOT_FOUND = 4

# Default settings
DEFAULT_HOST = "192.168.42.129"  # Android's IP when tethering
DEFAULT_PORT = 50000

# Screen resolution for coordinate conversion
DEFAULT_WIDTH = 1080
DEFAULT_HEIGHT = 2340


class MirageClient:
    """TCP client for communicating with Android TcpCommandServer"""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.host = host
        self.port = port
        self.sock: Optional[socket.socket] = None
        self.seq = 0
        self.width = DEFAULT_WIDTH
        self.height = DEFAULT_HEIGHT

    def connect(self) -> bool:
        """Connect to Android device"""
        # Close existing socket if any
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

        new_sock = None
        try:
            new_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            new_sock.settimeout(5.0)
            new_sock.connect((self.host, self.port))
            self.sock = new_sock
            print(f"Connected to {self.host}:{self.port}")
            return True
        except (OSError, socket.error, socket.timeout) as e:
            print(f"Connection failed: {e}")
            if new_sock:
                try:
                    new_sock.close()
                except OSError:
                    pass
            return False

    def disconnect(self):
        """Disconnect from device"""
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    def _build_header(self, cmd: int, payload_len: int) -> bytes:
        """Build protocol header"""
        self.seq += 1
        return struct.pack('<IBBII', MAGIC, VERSION, cmd, self.seq, payload_len)

    def _send_command(self, cmd: int, payload: bytes = b'') -> Tuple[bool, int]:
        """Send command and wait for ACK"""
        if not self.sock:
            return False, -1

        header = self._build_header(cmd, len(payload))
        data = header + payload

        try:
            self.sock.sendall(data)

            # Wait for ACK
            ack_data = self.sock.recv(HEADER_SIZE + 8)
            if len(ack_data) < HEADER_SIZE + 8:
                print("Incomplete ACK received")
                return False, -1

            # Parse ACK
            magic, ver, ack_cmd, ack_seq, payload_len = struct.unpack('<IBBII', ack_data[:HEADER_SIZE])
            if magic != MAGIC or ack_cmd != CMD_ACK:
                print(f"Invalid ACK: magic={hex(magic)}, cmd={ack_cmd}")
                return False, -1

            # Parse ACK payload
            ack_payload = ack_data[HEADER_SIZE:HEADER_SIZE + 8]
            recv_seq, status = struct.unpack('<IB', ack_payload[:5])

            return status == STATUS_OK, status

        except socket.timeout:
            print("Timeout waiting for ACK")
            return False, -1
        except Exception as e:
            print(f"Error sending command: {e}")
            return False, -1

    def ping(self) -> bool:
        """Send ping command"""
        success, status = self._send_command(CMD_PING)
        return success

    def tap(self, x: float, y: float) -> bool:
        """Tap at normalized coordinates (0.0-1.0)"""
        abs_x = int(x * self.width)
        abs_y = int(y * self.height)
        return self.tap_abs(abs_x, abs_y, self.width, self.height)

    def tap_abs(self, x: int, y: int, w: int, h: int) -> bool:
        """Tap at absolute coordinates"""
        # Payload: x(4) + y(4) + w(4) + h(4) + flags(4) = 20 bytes
        payload = struct.pack('<IIIII', x, y, w, h, 0)
        success, status = self._send_command(CMD_TAP, payload)
        return success

    def back(self) -> bool:
        """Press back button"""
        success, status = self._send_command(CMD_BACK)
        return success

    def key(self, keycode: int) -> bool:
        """Send keycode"""
        # Payload: keycode(4) + flags(4) = 8 bytes
        payload = struct.pack('<II', keycode, 0)
        success, status = self._send_command(CMD_KEY, payload)
        return success

    def click_id(self, resource_id: str) -> bool:
        """Click UI element by resource ID"""
        id_bytes = resource_id.encode('utf-8')
        # Payload: len(2) + string
        payload = struct.pack('<H', len(id_bytes)) + id_bytes
        success, status = self._send_command(CMD_CLICK_ID, payload)
        if status == STATUS_ERR_NOT_FOUND:
            print(f"UI element not found: {resource_id}")
        return success

    def click_text(self, text: str) -> bool:
        """Click UI element by text content"""
        text_bytes = text.encode('utf-8')
        # Payload: len(2) + string
        payload = struct.pack('<H', len(text_bytes)) + text_bytes
        success, status = self._send_command(CMD_CLICK_TEXT, payload)
        if status == STATUS_ERR_NOT_FOUND:
            print(f"UI element not found: {text}")
        return success

    def config(self, mirror_host: str, mirror_port: int) -> bool:
        """Send mirror configuration"""
        host_bytes = mirror_host.encode('utf-8')
        # Payload: host_len(2) + host + port(4)
        payload = struct.pack('<H', len(host_bytes)) + host_bytes + struct.pack('<I', mirror_port)
        success, status = self._send_command(CMD_CONFIG, payload)
        return success


def interactive_mode(client: MirageClient):
    """Interactive command mode"""
    print("\nInteractive mode. Commands:")
    print("  ping              - Send ping")
    print("  tap <x> <y>       - Tap (0.0-1.0 coords)")
    print("  back              - Back button")
    print("  key <code>        - Send keycode")
    print("  click_id <id>     - Click by resource ID")
    print("  click_text <text> - Click by text")
    print("  quit              - Exit")
    print()

    while True:
        try:
            cmd = input("> ").strip()
            if not cmd:
                continue

            parts = cmd.split()
            action = parts[0].lower()

            if action == 'quit' or action == 'exit':
                break
            elif action == 'ping':
                if client.ping():
                    print("Pong!")
                else:
                    print("Ping failed")
            elif action == 'tap' and len(parts) >= 3:
                x = float(parts[1])
                y = float(parts[2])
                if client.tap(x, y):
                    print(f"Tapped ({x}, {y})")
                else:
                    print("Tap failed")
            elif action == 'back':
                if client.back():
                    print("Back pressed")
                else:
                    print("Back failed")
            elif action == 'key' and len(parts) >= 2:
                keycode = int(parts[1])
                if client.key(keycode):
                    print(f"Key {keycode} sent")
                else:
                    print("Key failed")
            elif action == 'click_id' and len(parts) >= 2:
                res_id = ' '.join(parts[1:])
                if client.click_id(res_id):
                    print(f"Clicked {res_id}")
                else:
                    print("Click failed")
            elif action == 'click_text' and len(parts) >= 2:
                text = ' '.join(parts[1:])
                if client.click_text(text):
                    print(f"Clicked '{text}'")
                else:
                    print("Click failed")
            else:
                print(f"Unknown command: {cmd}")

        except KeyboardInterrupt:
            break
        except Exception as e:
            print(f"Error: {e}")


def main():
    parser = argparse.ArgumentParser(description='USB-LAN Controller for MirageTestKit')
    parser.add_argument('--host', default=DEFAULT_HOST, help=f'Android IP (default: {DEFAULT_HOST})')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'TCP port (default: {DEFAULT_PORT})')
    parser.add_argument('--width', type=int, default=DEFAULT_WIDTH, help='Screen width for coord conversion')
    parser.add_argument('--height', type=int, default=DEFAULT_HEIGHT, help='Screen height for coord conversion')
    parser.add_argument('command', nargs='?', default='interactive', help='Command to execute')
    parser.add_argument('args', nargs='*', help='Command arguments')

    args = parser.parse_args()

    client = MirageClient(args.host, args.port)
    client.width = args.width
    client.height = args.height

    if not client.connect():
        sys.exit(1)

    try:
        cmd = args.command.lower()

        if cmd == 'interactive':
            interactive_mode(client)
        elif cmd == 'ping':
            success = client.ping()
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'tap' and len(args.args) >= 2:
            x = float(args.args[0])
            y = float(args.args[1])
            success = client.tap(x, y)
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'tap_abs' and len(args.args) >= 4:
            x = int(args.args[0])
            y = int(args.args[1])
            w = int(args.args[2])
            h = int(args.args[3])
            success = client.tap_abs(x, y, w, h)
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'back':
            success = client.back()
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'key' and len(args.args) >= 1:
            keycode = int(args.args[0])
            success = client.key(keycode)
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'click_id' and len(args.args) >= 1:
            res_id = ' '.join(args.args)
            success = client.click_id(res_id)
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        elif cmd == 'click_text' and len(args.args) >= 1:
            text = ' '.join(args.args)
            success = client.click_text(text)
            print("OK" if success else "FAIL")
            sys.exit(0 if success else 1)
        else:
            print(f"Unknown command: {cmd}")
            parser.print_help()
            sys.exit(1)

    finally:
        client.disconnect()


if __name__ == '__main__':
    main()
