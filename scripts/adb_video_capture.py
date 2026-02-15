#!/usr/bin/env python3
"""
ADB Video Capture - Stream Android screen via ADB (no WiFi/AOA needed)
Uses `adb shell screenrecord --output-format=h264 -` to get raw H.264 stream
Wraps output in RTP packets for compatibility with existing viewers.

Usage:
    python adb_video_capture.py                    # All devices, output to UDP
    python adb_video_capture.py --serial A9250700479  # Specific device
    python adb_video_capture.py --file output.h264    # Save to file (raw, no RTP)
"""

import subprocess
import socket
import threading
import time
import sys
import argparse
import signal
import struct
import random

# Try to load config
try:
    from config_loader import get_pc_ip, get_video_base_port
    DEFAULT_HOST = get_pc_ip()
    DEFAULT_BASE_PORT = get_video_base_port()
except ImportError:
    DEFAULT_HOST = "127.0.0.1"
    DEFAULT_BASE_PORT = 60000


class RtpPacketizer:
    """Wrap H.264 NAL units in RTP packets"""

    def __init__(self, ssrc=None):
        self.sequence_number = random.randint(0, 65535)
        self.timestamp = random.randint(0, 0xFFFFFFFF)
        self.ssrc = ssrc or random.randint(0, 0xFFFFFFFF)
        self.clock_rate = 90000  # 90kHz for video

    def make_rtp_header(self, marker=False, payload_type=96):
        """Create 12-byte RTP header"""
        # Version=2, Padding=0, Extension=0, CSRC count=0
        byte0 = 0x80
        # Marker + Payload type
        byte1 = (0x80 if marker else 0) | (payload_type & 0x7F)

        header = struct.pack('>BBHII',
                             byte0, byte1,
                             self.sequence_number & 0xFFFF,
                             self.timestamp & 0xFFFFFFFF,
                             self.ssrc)

        self.sequence_number = (self.sequence_number + 1) & 0xFFFF
        return header

    def packetize_nal(self, nal_data, max_size=1400):
        """
        Packetize a NAL unit into RTP packets.
        Returns list of complete RTP packets.
        """
        packets = []

        if len(nal_data) <= max_size:
            # Single NAL unit packet
            header = self.make_rtp_header(marker=True)
            packets.append(header + nal_data)
        else:
            # Fragmentation Unit A (FU-A)
            nal_header = nal_data[0]
            nal_type = nal_header & 0x1F
            nri = nal_header & 0x60

            # FU indicator: same NRI, type=28 (FU-A)
            fu_indicator = nri | 28
            offset = 1  # Skip original NAL header

            first = True
            while offset < len(nal_data):
                remaining = len(nal_data) - offset
                chunk_size = min(remaining, max_size - 2)  # -2 for FU indicator + FU header
                last = (offset + chunk_size >= len(nal_data))

                # FU header: S=start, E=end, R=0, Type
                fu_header = nal_type
                if first:
                    fu_header |= 0x80  # Start bit
                if last:
                    fu_header |= 0x40  # End bit

                header = self.make_rtp_header(marker=last)
                payload = bytes([fu_indicator, fu_header]) + nal_data[offset:offset + chunk_size]
                packets.append(header + payload)

                offset += chunk_size
                first = False

        return packets

    def increment_timestamp(self, frame_duration_ms=33):
        """Increment timestamp by frame duration (default ~30fps)"""
        self.timestamp = (self.timestamp + int(self.clock_rate * frame_duration_ms / 1000)) & 0xFFFFFFFF


class H264NalParser:
    """Parse H.264 Annex B stream into NAL units"""

    def __init__(self):
        self.buffer = bytearray()

    def parse(self, data):
        """
        Feed data and return complete NAL units (without start codes).
        """
        self.buffer.extend(data)
        nals = []

        while True:
            # Find start code
            start = self._find_start_code(0)
            if start < 0:
                break

            # Find next start code
            next_start = self._find_start_code(start + 3)
            if next_start < 0:
                # Keep data from current start code for next call
                self.buffer = self.buffer[start:]
                break

            # Extract NAL (without start code)
            start_code_len = 4 if self.buffer[start:start + 4] == b'\x00\x00\x00\x01' else 3
            nal_data = bytes(self.buffer[start + start_code_len:next_start])
            if nal_data:
                nals.append(nal_data)

            self.buffer = self.buffer[next_start:]

        # Prevent buffer from growing too large
        if len(self.buffer) > 1024 * 1024:
            # Keep only last 64KB
            self.buffer = self.buffer[-65536:]

        return nals

    def _find_start_code(self, offset):
        """Find next 00 00 01 or 00 00 00 01 start code"""
        pos = offset
        while pos < len(self.buffer) - 2:
            if self.buffer[pos] == 0 and self.buffer[pos + 1] == 0:
                if self.buffer[pos + 2] == 1:
                    return pos
                if pos < len(self.buffer) - 3 and self.buffer[pos + 2] == 0 and self.buffer[pos + 3] == 1:
                    return pos
            pos += 1
        return -1


class AdbVideoCapture:
    def __init__(self, serial, host="127.0.0.1", port=60000, output_file=None):
        self.serial = serial
        self.host = host
        self.port = port
        self.output_file = output_file
        self.process = None
        self.running = False
        self.bytes_sent = 0
        self.packets_sent = 0

    def start(self):
        """Start capturing and streaming"""
        cmd = [
            "adb", "-s", self.serial, "shell",
            "screenrecord",
            "--output-format=h264",
            "--bit-rate=4000000",  # 4Mbps
            "--size=720x1280",     # Reduce resolution for lower latency
            "-"
        ]

        print(f"[{self.serial}] Starting capture -> {self.host}:{self.port}")
        self.process = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0
        )

        self.running = True

        if self.output_file:
            self._stream_to_file()
        else:
            self._stream_to_udp_rtp()

    def _stream_to_udp_rtp(self):
        """Stream H.264 data via UDP with RTP encapsulation"""
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            nal_parser = H264NalParser()
            rtp = RtpPacketizer()

            last_frame_time = time.time()

            while self.running and self.process.poll() is None:
                data = self.process.stdout.read(4096)
                if not data:
                    break

                # Parse NAL units from H.264 stream
                nals = nal_parser.parse(data)

                for nal in nals:
                    # Packetize and send via RTP
                    packets = rtp.packetize_nal(nal)
                    for pkt in packets:
                        try:
                            sock.sendto(pkt, (self.host, self.port))
                            self.bytes_sent += len(pkt)
                            self.packets_sent += 1
                        except (OSError, socket.error) as e:
                            print(f"[{self.serial}] Send error: {e}")

                    # Check NAL type for timestamp increment
                    nal_type = nal[0] & 0x1F
                    # Increment timestamp after each frame (IDR=5, non-IDR=1)
                    if nal_type in (1, 5):
                        now = time.time()
                        frame_ms = (now - last_frame_time) * 1000
                        last_frame_time = now
                        rtp.increment_timestamp(max(16, min(100, frame_ms)))  # Clamp to reasonable range

            print(f"[{self.serial}] Stopped. Sent {self.packets_sent} RTP packets, {self.bytes_sent/1024:.1f}KB")
        finally:
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass

    def _stream_to_file(self):
        """Save raw H.264 data to file"""
        try:
            with open(self.output_file, 'wb') as f:
                while self.running and self.process.poll() is None:
                    data = self.process.stdout.read(4096)
                    if not data:
                        break
                    f.write(data)
                    self.bytes_sent += len(data)

            print(f"[{self.serial}] Saved {self.bytes_sent/1024:.1f}KB to {self.output_file}")
        except (IOError, OSError) as e:
            print(f"[{self.serial}] Failed to write to file {self.output_file}: {e}")

    def stop(self):
        """Stop capture"""
        self.running = False
        if self.process:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()


def get_connected_devices():
    """Get list of ADB-connected devices"""
    result = subprocess.run(["adb", "devices", "-l"], capture_output=True, text=True)
    devices = []
    for line in result.stdout.strip().split('\n')[1:]:
        if '\tdevice' in line or ' device ' in line:
            parts = line.split()
            serial = parts[0]
            if ':' in serial:  # Skip WiFi ADB
                continue
            model = "Unknown"
            for p in parts:
                if p.startswith("model:"):
                    model = p.replace("model:", "")
                    break
            devices.append({"serial": serial, "model": model})
    return devices


def main():
    parser = argparse.ArgumentParser(description="ADB Video Capture")
    parser.add_argument("--serial", "-s", help="Device serial (default: all devices)")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"UDP host (default: {DEFAULT_HOST})")
    parser.add_argument("--base-port", type=int, default=DEFAULT_BASE_PORT, help=f"Base UDP port (default: {DEFAULT_BASE_PORT})")
    parser.add_argument("--file", "-o", help="Output to file instead of UDP (raw H.264)")
    parser.add_argument("--time-limit", type=int, default=0, help="Time limit in seconds (0=unlimited)")
    args = parser.parse_args()

    print("=" * 60)
    print("ADB Video Capture - MirageTestKit (RTP mode)")
    print("=" * 60)

    if args.serial:
        devices = [{"serial": args.serial, "model": ""}]
    else:
        devices = get_connected_devices()

    if not devices:
        print("No devices found!")
        sys.exit(1)

    print(f"Found {len(devices)} device(s):")
    for i, dev in enumerate(devices):
        port = args.base_port + i
        print(f"  {i+1}. {dev['serial']} ({dev['model']}) -> UDP port {port}")

    print()

    # Create captures
    captures = []
    for i, dev in enumerate(devices):
        port = args.base_port + i
        output_file = f"{args.file}_{i}.h264" if args.file and len(devices) > 1 else args.file
        cap = AdbVideoCapture(dev["serial"], args.host, port, output_file)
        captures.append(cap)

    # Signal handler
    def signal_handler(sig, frame):
        print("\nStopping captures...")
        for cap in captures:
            cap.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Start all captures in threads
    threads = []
    for cap in captures:
        t = threading.Thread(target=cap.start, daemon=True)
        t.start()
        threads.append(t)
        time.sleep(0.5)  # Stagger starts

    print("\nStreaming RTP H.264... Press Ctrl+C to stop\n")

    # Monitor
    start_time = time.time()
    try:
        while any(t.is_alive() for t in threads):
            time.sleep(1)

            # Print stats
            total_packets = sum(cap.packets_sent for cap in captures)
            total_bytes = sum(cap.bytes_sent for cap in captures)
            elapsed = time.time() - start_time

            if elapsed > 0:
                print(f"\r[{elapsed:.0f}s] RTP Packets: {total_packets}, Data: {total_bytes/1024:.1f}KB ({total_bytes*8/elapsed/1000:.1f}kbps)", end="", flush=True)

            # Time limit
            if args.time_limit > 0 and elapsed >= args.time_limit:
                break
    except KeyboardInterrupt:
        pass

    print("\n\nStopping...")
    for cap in captures:
        cap.stop()

    print("Done.")


if __name__ == "__main__":
    main()
