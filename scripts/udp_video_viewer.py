#!/usr/bin/env python3
"""
UDP Video Viewer - Displays H.264 video streams from Android devices
Receives RTP packets on specified ports and displays decoded video
"""

import sys
import socket
import threading
import subprocess
import struct
import time
import math
import tkinter as tk
from PIL import Image, ImageTk, ImageDraw
import queue
import os

# Video dimensions
VIDEO_WIDTH = 800
VIDEO_HEIGHT = 480


class RtpDepacketizer:
    """Depacketize RTP H.264 stream"""
    def __init__(self):
        self.fu_buffer = bytearray()
        self.last_seq = -1

    def process_packet(self, data):
        """Process RTP packet and return complete NALs"""
        if len(data) < 12:
            return []

        seq = struct.unpack('>H', data[2:4])[0]

        if self.last_seq >= 0 and seq != (self.last_seq + 1) & 0xFFFF:
            self.fu_buffer = bytearray()
        self.last_seq = seq

        payload = data[12:]
        if len(payload) < 1:
            return []

        nal_type = payload[0] & 0x1F
        nals = []

        if nal_type <= 23:
            nals.append(bytes([0, 0, 0, 1]) + payload)
        elif nal_type == 28:  # FU-A
            if len(payload) < 2:
                return []
            fu_header = payload[1]
            start = (fu_header & 0x80) != 0
            end = (fu_header & 0x40) != 0
            nal_type_inner = fu_header & 0x1F

            if start:
                self.fu_buffer = bytearray([0, 0, 0, 1, (payload[0] & 0xE0) | nal_type_inner])
                self.fu_buffer.extend(payload[2:])
            else:
                self.fu_buffer.extend(payload[2:])

            if end:
                nals.append(bytes(self.fu_buffer))
                self.fu_buffer = bytearray()

        return nals


class VideoDecoder:
    """Decode H.264 using ffmpeg subprocess"""
    def __init__(self, width, height):
        self.width = width
        self.height = height
        self.frame_size = width * height * 3
        self.process = None
        self.frame_queue = queue.Queue(maxsize=2)
        self.running = False

    def start(self):
        try:
            cmd = [
                'ffmpeg', '-hide_banner', '-loglevel', 'error',
                '-f', 'h264', '-i', 'pipe:0',
                '-f', 'rawvideo', '-pix_fmt', 'rgb24',
                '-s', f'{self.width}x{self.height}', 'pipe:1'
            ]

            startupinfo = None
            if os.name == 'nt':
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = subprocess.SW_HIDE

            self.process = subprocess.Popen(
                cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, startupinfo=startupinfo
            )

            self.running = True
            threading.Thread(target=self._read_frames, daemon=True).start()
            return True
        except Exception as e:
            print(f"Failed to start ffmpeg: {e}")
            return False

    def _read_frames(self):
        while self.running and self.process:
            try:
                data = self.process.stdout.read(self.frame_size)
                if len(data) == self.frame_size:
                    img = Image.frombytes('RGB', (self.width, self.height), data)
                    try:
                        self.frame_queue.put_nowait(img)
                    except queue.Full:
                        try:
                            self.frame_queue.get_nowait()
                            self.frame_queue.put_nowait(img)
                        except Exception:
                            pass
                elif len(data) == 0:
                    break
            except Exception:
                break

    def write_nal(self, nal_data):
        if self.process and self.process.stdin:
            try:
                self.process.stdin.write(nal_data)
                self.process.stdin.flush()
            except Exception:
                pass

    def get_frame(self):
        try:
            return self.frame_queue.get_nowait()
        except queue.Empty:
            return None

    def stop(self):
        self.running = False
        if self.process:
            try:
                self.process.stdin.close()
                self.process.terminate()
            except Exception:
                pass


class VideoReceiver(threading.Thread):
    """Receive and decode video from UDP port"""
    def __init__(self, port, name):
        super().__init__(daemon=True)
        self.port = port
        self.name = name
        self.running = False
        self.depacketizer = RtpDepacketizer()
        self.packet_count = 0
        self.byte_count = 0
        self.frame_count = 0
        self.decoder = None
        self.last_frame_time = 0
        self.has_video = False
        self.current_frame = None
        self.lock = threading.Lock()

    def run(self):
        self.running = True
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(('0.0.0.0', self.port))
        sock.settimeout(0.1)

        print(f"[{self.name}] Listening on port {self.port}")

        self.decoder = VideoDecoder(VIDEO_WIDTH, VIDEO_HEIGHT)
        if self.decoder.start():
            print(f"[{self.name}] Decoder started")

        while self.running:
            try:
                data, addr = sock.recvfrom(65535)
                self.packet_count += 1
                self.byte_count += len(data)

                nals = self.depacketizer.process_packet(data)
                for nal in nals:
                    if self.decoder:
                        self.decoder.write_nal(nal)

                if self.decoder:
                    frame = self.decoder.get_frame()
                    if frame:
                        self.has_video = True
                        with self.lock:
                            self.current_frame = frame
                        self.frame_count += 1
                        self.last_frame_time = time.time()

            except socket.timeout:
                pass
            except Exception as e:
                print(f"[{self.name}] Error: {e}")

        sock.close()
        if self.decoder:
            self.decoder.stop()

    def get_display_frame(self):
        """Get current frame for display"""
        with self.lock:
            if self.current_frame:
                return self.current_frame.copy()
        return None

    def stop(self):
        self.running = False


class VideoViewer:
    """Main GUI window"""
    def __init__(self, devices):
        self.root = tk.Tk()
        self.root.title("MirageTestKit - UDP Video Viewer")
        self.root.geometry("1280x720")
        self.root.configure(bg='#1e1e2e')

        self.receivers = {}
        self.photo_images = {}
        self.labels = {}
        self.devices = devices

        self.setup_ui()
        self.start_receivers()

    def setup_ui(self):
        # Title bar
        title_frame = tk.Frame(self.root, bg='#3d3d4d', height=50)
        title_frame.pack(fill='x', padx=5, pady=5)
        title_frame.pack_propagate(False)

        tk.Label(title_frame, text="MirageTestKit - UDP Video Viewer",
                 fg='#00ffcc', bg='#3d3d4d', font=('Arial', 16, 'bold')).pack(side='left', padx=15, pady=10)

        # Video container
        video_container = tk.Frame(self.root, bg='#1e1e2e')
        video_container.pack(fill='both', expand=True, padx=10, pady=5)

        # Create device panels
        for i, dev in enumerate(self.devices):
            name = dev['model']
            port = 60000 + i

            # Device frame
            device_frame = tk.Frame(video_container, bg='#2d2d3d', width=400, height=300)
            device_frame.pack(side='left', padx=10, pady=10)
            device_frame.pack_propagate(False)

            # Header
            header = tk.Label(device_frame, text=f"{name} (:{port})",
                             fg='white', bg='#4d4d5d', font=('Arial', 11, 'bold'))
            header.pack(fill='x')

            # Video label
            video_label = tk.Label(device_frame, bg='black', width=380, height=240)
            video_label.pack(padx=5, pady=5)
            self.labels[name] = video_label

            # Stats label
            stats_label = tk.Label(device_frame, text="Waiting...",
                                   fg='#aaaaaa', bg='#2d2d3d', font=('Consolas', 9))
            stats_label.pack()
            self.labels[f"{name}_stats"] = stats_label

        # Status bar
        self.status_var = tk.StringVar(value="Starting...")
        status_bar = tk.Label(self.root, textvariable=self.status_var,
                              fg='#aaaaaa', bg='#151520', anchor='w', font=('Consolas', 10))
        status_bar.pack(fill='x', side='bottom', ipady=5)

    def start_receivers(self):
        """Start video receivers for each device"""
        for i, dev in enumerate(self.devices):
            name = dev['model']
            port = 60000 + i
            receiver = VideoReceiver(port, name)
            receiver.start()
            self.receivers[name] = receiver

    def generate_status_image(self, recv):
        """Generate status image for a receiver"""
        width, height = 380, 240
        img = Image.new('RGB', (width, height), (30, 30, 45))
        draw = ImageDraw.Draw(img)

        y = 20
        draw.text((20, y), f"{recv.name}", fill=(255, 255, 255))
        y += 30
        draw.text((20, y), f"Port: {recv.port}", fill=(180, 180, 200))
        y += 25

        color = (100, 255, 100) if recv.packet_count > 0 else (255, 100, 100)
        draw.text((20, y), f"Packets: {recv.packet_count}", fill=color)
        y += 25
        draw.text((20, y), f"Data: {recv.byte_count/1024:.1f} KB", fill=(100, 200, 255))
        y += 25
        draw.text((20, y), f"Frames: {recv.frame_count}", fill=(200, 200, 100))
        y += 25

        if recv.has_video:
            status, color = "Video OK", (100, 255, 100)
        elif recv.packet_count > 0:
            status, color = "Decoding...", (255, 200, 100)
        else:
            status, color = "Waiting...", (255, 100, 100)
        draw.text((20, y), status, fill=color)

        # Animated dots
        t = time.time() * 3
        for i in range(5):
            x = int(50 + i * 60 + 15 * math.sin(t + i * 0.5))
            brightness = int(128 + 127 * math.sin(t + i))
            draw.ellipse([x-6, 200, x+6, 212], fill=(brightness, 200, brightness))

        return img

    def update_display(self):
        """Update all displays"""
        total_packets = 0
        total_frames = 0

        for name, recv in self.receivers.items():
            total_packets += recv.packet_count
            total_frames += recv.frame_count

            # Get frame to display
            frame = recv.get_display_frame()
            if frame:
                display_img = frame.resize((380, 240), Image.Resampling.LANCZOS)
            else:
                display_img = self.generate_status_image(recv)

            # Update label
            if name in self.labels:
                try:
                    photo = ImageTk.PhotoImage(display_img)
                    self.photo_images[name] = photo
                    self.labels[name].configure(image=photo)
                except Exception as e:
                    pass

            # Update stats
            stats_key = f"{name}_stats"
            if stats_key in self.labels:
                status = "OK" if recv.has_video else ("RX" if recv.packet_count > 0 else "--")
                self.labels[stats_key].configure(
                    text=f"[{status}] {recv.packet_count} pkt | {recv.byte_count/1024:.1f} KB | {recv.frame_count} frm"
                )

        self.status_var.set(f"Total: {total_packets} packets | {total_frames} frames decoded")
        self.root.after(100, self.update_display)

    def run(self):
        self.update_display()
        self.root.mainloop()

    def stop(self):
        for recv in self.receivers.values():
            recv.stop()


def get_adb_devices():
    """Get list of ADB devices"""
    try:
        result = subprocess.run(['adb', 'devices', '-l'], capture_output=True, text=True, timeout=5)
        devices = []
        for line in result.stdout.strip().split('\n')[1:]:
            if '\tdevice' in line or ' device ' in line:
                parts = line.split()
                serial = parts[0]
                model = "Device"
                for part in parts:
                    if part.startswith('model:'):
                        model = part[6:]
                devices.append({'serial': serial, 'model': model})
        return devices
    except Exception as e:
        print(f"Error getting ADB devices: {e}")
        return []


def main():
    print("=" * 60)
    print("MirageTestKit - UDP Video Viewer")
    print("=" * 60)

    # Check ffmpeg
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True, timeout=5)
        print("[OK] ffmpeg found")
    except Exception:
        print("[WARNING] ffmpeg not found")

    # Get devices
    devices = get_adb_devices()
    if not devices:
        print("\n[WARNING] No ADB devices found! Using defaults.")
        devices = [
            {'serial': 'dev0', 'model': 'Device_0'},
            {'serial': 'dev1', 'model': 'Device_1'},
            {'serial': 'dev2', 'model': 'Device_2'},
        ]
    else:
        print(f"\nFound {len(devices)} device(s):")
        for i, dev in enumerate(devices):
            print(f"  [{i}] {dev['model']} ({dev['serial']}) -> port {60000 + i}")

    print("\nStarting viewer...")

    viewer = VideoViewer(devices)
    try:
        viewer.run()
    except KeyboardInterrupt:
        pass
    finally:
        viewer.stop()


if __name__ == '__main__':
    main()
