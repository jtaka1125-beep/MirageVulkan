#!/usr/bin/env python3
"""
Hybrid Video Viewer for MirageTestKit
Receives H.264 video via USB AOA (priority) or UDP WiFi (fallback).

USB Protocol: [MAGIC(4)] [LENGTH(4)] [RTP DATA]
MAGIC = 0x56494430 ("VID0")

Usage:
    python hybrid_video_viewer.py [--port PORT] [--usb] [--wifi-only]
"""

import sys
import socket
import threading
import subprocess
import struct
import time
import math
import argparse
import tkinter as tk
from PIL import Image, ImageTk, ImageDraw
import queue
import os

# Try to import USB support
try:
    import usb.core
    import usb.util
    HAS_USB = True
except ImportError:
    HAS_USB = False

DEFAULT_PORT = 60000
VIDEO_WIDTH = 540
VIDEO_HEIGHT = 1170
USB_MAGIC = 0x56494430  # "VID0"
AOA_VID = 0x18D1
AOA_PIDS = [0x2D00, 0x2D01]


class RtpDepacketizer:
    """Depacketize RTP H.264 stream"""
    def __init__(self):
        self.fu_buffer = bytearray()
        self.last_seq = -1
        self.lost_packets = 0

    def process_packet(self, data):
        if len(data) < 12:
            return []
        seq = struct.unpack('>H', data[2:4])[0]
        if self.last_seq >= 0:
            expected = (self.last_seq + 1) & 0xFFFF
            if seq != expected:
                self.lost_packets += 1
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
        self.frame_queue = queue.Queue(maxsize=3)
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
                    # Use a loop to handle race condition when dropping old frames
                    for _ in range(3):  # Max 3 retry attempts
                        try:
                            self.frame_queue.put_nowait(img)
                            break  # Success
                        except queue.Full:
                            try:
                                self.frame_queue.get_nowait()  # Drop oldest frame
                            except queue.Empty:
                                pass  # Queue was emptied by consumer, retry put
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


class UsbVideoReceiver:
    """Receive video from USB AOA device"""
    def __init__(self, callback):
        self.callback = callback
        self.running = False
        self.device = None
        self.ep_in = None
        self.thread = None
        self.packet_count = 0
        self.byte_count = 0

    def find_aoa_device(self):
        if not HAS_USB:
            return None
        for pid in AOA_PIDS:
            dev = usb.core.find(idVendor=AOA_VID, idProduct=pid)
            if dev:
                return dev
        return None

    def start(self):
        self.device = self.find_aoa_device()
        if not self.device:
            print("[USB] No AOA device found")
            return False

        try:
            self.device.set_configuration()
            cfg = self.device.get_active_configuration()
            intf = cfg[(0, 0)]

            # Find bulk IN endpoint
            self.ep_in = usb.util.find_descriptor(
                intf,
                custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN
            )
            if not self.ep_in:
                print("[USB] No IN endpoint found")
                return False

            print(f"[USB] Connected to AOA device (EP: 0x{self.ep_in.bEndpointAddress:02x})")
            self.running = True
            self.thread = threading.Thread(target=self._receive_loop, daemon=True)
            self.thread.start()
            return True

        except Exception as e:
            print(f"[USB] Failed to initialize: {e}")
            return False

    def _receive_loop(self):
        buffer = bytearray()
        while self.running:
            try:
                data = self.ep_in.read(16384, timeout=500)
                buffer.extend(data)

                # Parse packets: [MAGIC(4)][LEN(4)][DATA(LEN)]
                while len(buffer) >= 8:
                    magic = struct.unpack('>I', buffer[:4])[0]
                    if magic != USB_MAGIC:
                        # Sync error, skip byte
                        buffer = buffer[1:]
                        continue

                    length = struct.unpack('>I', buffer[4:8])[0]
                    if length > 65535:
                        # Invalid length, skip
                        buffer = buffer[1:]
                        continue

                    if len(buffer) < 8 + length:
                        break  # Need more data

                    rtp_data = bytes(buffer[8:8+length])
                    buffer = buffer[8+length:]

                    self.packet_count += 1
                    self.byte_count += len(rtp_data)
                    self.callback(rtp_data)

            except usb.core.USBTimeoutError:
                pass
            except usb.core.USBError as e:
                if self.running:
                    print(f"[USB] Error: {e}")
                break

        print("[USB] Receiver stopped")

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1)

    def is_connected(self):
        return self.running and self.device is not None


class WifiVideoReceiver:
    """Receive video from WiFi UDP"""
    def __init__(self, port, callback):
        self.port = port
        self.callback = callback
        self.running = False
        self.thread = None
        self.packet_count = 0
        self.byte_count = 0

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.thread.start()
        print(f"[WiFi] Listening on UDP port {self.port}")
        return True

    def _receive_loop(self):
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind(('0.0.0.0', self.port))
            sock.settimeout(0.5)

            while self.running:
                try:
                    data, addr = sock.recvfrom(65535)
                    self.packet_count += 1
                    self.byte_count += len(data)
                    self.callback(data)
                except socket.timeout:
                    pass
                except (OSError, socket.error) as e:
                    if self.running:
                        print(f"[WiFi] Error: {e}")
                    break
        except (OSError, socket.error) as e:
            print(f"[WiFi] Failed to start receiver: {e}")
        finally:
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=1)


class HybridVideoViewer:
    """Main GUI with USB priority, WiFi fallback"""

    def __init__(self, port: int, width: int, height: int, usb_enabled: bool, wifi_only: bool):
        self.port = port
        self.video_width = width
        self.video_height = height
        self.usb_enabled = usb_enabled and HAS_USB and not wifi_only
        self.wifi_only = wifi_only

        self.root = tk.Tk()
        mode = "WiFi Only" if wifi_only else ("USB+WiFi" if self.usb_enabled else "WiFi")
        self.root.title(f"MirageTestKit Hybrid Viewer ({mode})")
        self.root.geometry(f"{width + 40}x{height + 120}")
        self.root.configure(bg='#1e1e2e')

        self.running = False
        self.depacketizer = RtpDepacketizer()
        self.decoder = None
        self.usb_receiver = None
        self.wifi_receiver = None
        self.current_frame = None
        self.frame_lock = threading.Lock()

        self.frame_count = 0
        self.fps = 0.0
        self.fps_update_time = time.time()
        self.fps_frame_count = 0
        self.active_source = "None"

        self.setup_ui()

    def setup_ui(self):
        # Title bar
        title_frame = tk.Frame(self.root, bg='#3d3d4d', height=40)
        title_frame.pack(fill='x', padx=5, pady=5)
        title_frame.pack_propagate(False)

        tk.Label(
            title_frame,
            text="Hybrid Video Viewer (USB→WiFi)",
            fg='#00ffcc', bg='#3d3d4d',
            font=('Arial', 14, 'bold')
        ).pack(side='left', padx=10, pady=5)

        # Source indicator
        self.source_label = tk.Label(
            title_frame,
            text="[?]",
            fg='#888888', bg='#3d3d4d',
            font=('Arial', 12)
        )
        self.source_label.pack(side='right', padx=10)

        # Video display
        video_frame = tk.Frame(self.root, bg='#2d2d3d')
        video_frame.pack(fill='both', expand=True, padx=10, pady=5)

        self.video_label = tk.Label(video_frame, bg='black')
        self.video_label.pack(padx=5, pady=5)

        # Stats bar
        stats_frame = tk.Frame(self.root, bg='#2d2d3d', height=50)
        stats_frame.pack(fill='x', padx=10, pady=5)
        stats_frame.pack_propagate(False)

        self.stats_label = tk.Label(
            stats_frame,
            text="Initializing...",
            fg='#aaaaaa', bg='#2d2d3d',
            font=('Consolas', 10),
            justify='left'
        )
        self.stats_label.pack(side='left', padx=10, anchor='nw')

        # Status bar
        self.status_var = tk.StringVar(value="Starting...")
        status_bar = tk.Label(
            self.root,
            textvariable=self.status_var,
            fg='#aaaaaa', bg='#151520',
            anchor='w',
            font=('Consolas', 9)
        )
        status_bar.pack(fill='x', side='bottom', ipady=3)

    def on_rtp_packet(self, data):
        """Called when RTP packet received from any source"""
        nals = self.depacketizer.process_packet(data)
        for nal in nals:
            if self.decoder:
                self.decoder.write_nal(nal)

        if self.decoder:
            frame = self.decoder.get_frame()
            if frame:
                with self.frame_lock:
                    self.current_frame = frame
                self.frame_count += 1

    def start_receivers(self):
        self.running = True

        # Start decoder
        self.decoder = VideoDecoder(self.video_width, self.video_height)
        if not self.decoder.start():
            self.status_var.set("ERROR: Failed to start ffmpeg decoder")
            return

        # Start USB receiver (if enabled)
        if self.usb_enabled:
            self.usb_receiver = UsbVideoReceiver(self.on_rtp_packet)
            if self.usb_receiver.start():
                self.active_source = "USB"
            else:
                self.usb_receiver = None

        # Start WiFi receiver
        self.wifi_receiver = WifiVideoReceiver(self.port, self.on_rtp_packet)
        self.wifi_receiver.start()
        if self.active_source == "None":
            self.active_source = "WiFi"

        self.status_var.set(f"Active: {self.active_source} | WiFi port: {self.port}")

    def update_display(self):
        # Get current frame
        frame = None
        with self.frame_lock:
            if self.current_frame:
                frame = self.current_frame.copy()

        if frame:
            try:
                photo = ImageTk.PhotoImage(frame)
                self.video_label.configure(image=photo)
                self.video_label.image = photo
            except Exception:
                pass
        else:
            self._show_status_image()

        # Update FPS
        now = time.time()
        if now - self.fps_update_time >= 1.0:
            self.fps = self.frame_count - self.fps_frame_count
            self.fps_frame_count = self.frame_count
            self.fps_update_time = now

        # Determine active source
        usb_active = self.usb_receiver and self.usb_receiver.packet_count > 0
        wifi_active = self.wifi_receiver and self.wifi_receiver.packet_count > 0

        if usb_active:
            self.active_source = "USB"
            self.source_label.config(text="[USB]", fg='#00ff00')
        elif wifi_active:
            self.active_source = "WiFi"
            self.source_label.config(text="[WiFi]", fg='#ffff00')
        else:
            self.source_label.config(text="[---]", fg='#888888')

        # Update stats
        usb_pkts = self.usb_receiver.packet_count if self.usb_receiver else 0
        usb_mb = (self.usb_receiver.byte_count / 1024 / 1024) if self.usb_receiver else 0
        wifi_pkts = self.wifi_receiver.packet_count if self.wifi_receiver else 0
        wifi_mb = (self.wifi_receiver.byte_count / 1024 / 1024) if self.wifi_receiver else 0

        stats = f"USB: {usb_pkts} pkts ({usb_mb:.1f}MB) | WiFi: {wifi_pkts} pkts ({wifi_mb:.1f}MB)\n"
        stats += f"Frames: {self.frame_count} | FPS: {self.fps:.0f} | Lost: {self.depacketizer.lost_packets}"
        self.stats_label.config(text=stats)

        self.root.after(33, self.update_display)

    def _show_status_image(self):
        img = Image.new('RGB', (self.video_width, self.video_height), (30, 30, 45))
        draw = ImageDraw.Draw(img)

        y = 50
        draw.text((50, y), "Hybrid Video Viewer", fill=(255, 255, 255))
        y += 40
        draw.text((50, y), f"USB: {'Enabled' if self.usb_enabled else 'Disabled'}",
                  fill=(100, 255, 100) if self.usb_enabled else (150, 150, 150))
        y += 25
        draw.text((50, y), f"WiFi Port: {self.port}", fill=(180, 180, 200))
        y += 40

        if self.frame_count == 0:
            draw.text((50, y), "Waiting for video stream...", fill=(255, 150, 150))

        # Animated indicator
        t = time.time() * 2
        cx = self.video_width // 2
        cy = self.video_height - 50
        for i in range(8):
            angle = i * (math.pi / 4) + t
            x = int(cx + 20 * math.cos(angle))
            y_pos = int(cy + 20 * math.sin(angle))
            brightness = int(100 + 100 * ((math.sin(t + i * 0.5) + 1) / 2))
            draw.ellipse([x-4, y_pos-4, x+4, y_pos+4], fill=(brightness, brightness, brightness))

        try:
            photo = ImageTk.PhotoImage(img)
            self.video_label.configure(image=photo)
            self.video_label.image = photo
        except Exception:
            pass

    def run(self):
        self.start_receivers()
        self.update_display()
        self.root.mainloop()

    def stop(self):
        self.running = False
        if self.usb_receiver:
            self.usb_receiver.stop()
        if self.wifi_receiver:
            self.wifi_receiver.stop()
        if self.decoder:
            self.decoder.stop()


def main():
    parser = argparse.ArgumentParser(description='Hybrid Video Viewer (USB priority, WiFi fallback)')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'WiFi UDP port (default: {DEFAULT_PORT})')
    parser.add_argument('--width', type=int, default=VIDEO_WIDTH, help=f'Video width (default: {VIDEO_WIDTH})')
    parser.add_argument('--height', type=int, default=VIDEO_HEIGHT, help=f'Video height (default: {VIDEO_HEIGHT})')
    parser.add_argument('--usb', action='store_true', help='Enable USB receiver (requires pyusb)')
    parser.add_argument('--wifi-only', action='store_true', help='WiFi only mode (no USB)')

    args = parser.parse_args()

    print("=" * 60)
    print("MirageTestKit - Hybrid Video Viewer")
    print("=" * 60)

    # Check dependencies
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True, timeout=5)
        print("[OK] ffmpeg found")
    except Exception:
        print("[ERROR] ffmpeg not found!")
        sys.exit(1)

    if HAS_USB:
        print("[OK] pyusb available")
    else:
        print("[WARN] pyusb not available (USB disabled)")

    print(f"WiFi port: {args.port}")
    print(f"USB: {'Enabled' if (args.usb and HAS_USB and not args.wifi_only) else 'Disabled'}")
    print()

    viewer = HybridVideoViewer(
        args.port, args.width, args.height,
        usb_enabled=args.usb,
        wifi_only=args.wifi_only
    )
    try:
        viewer.run()
    except KeyboardInterrupt:
        pass
    finally:
        viewer.stop()


if __name__ == '__main__':
    main()
