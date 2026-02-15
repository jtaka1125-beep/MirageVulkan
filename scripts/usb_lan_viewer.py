#!/usr/bin/env python3
"""
USB-LAN Video Viewer for MirageTestKit

Receives H.264 video via UDP from Android over USB tethering network.
Default port: 60000 (USB-LAN mode)

Usage:
    python usb_lan_viewer.py [--port PORT] [--width W] [--height H]
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

# Default settings for USB-LAN mode
DEFAULT_PORT = 60000
VIDEO_WIDTH = 540   # Half of 1080 for display
VIDEO_HEIGHT = 1170  # Half of 2340 for display


class RtpDepacketizer:
    """Depacketize RTP H.264 stream"""
    def __init__(self):
        self.fu_buffer = bytearray()
        self.last_seq = -1
        self.lost_packets = 0

    def process_packet(self, data):
        """Process RTP packet and return complete NALs"""
        if len(data) < 12:
            return []

        seq = struct.unpack('>H', data[2:4])[0]

        # Check for lost packets
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
            # Single NAL unit
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


class UsbLanVideoViewer:
    """Main GUI window for USB-LAN video viewing"""

    def __init__(self, port: int, width: int, height: int):
        self.port = port
        self.video_width = width
        self.video_height = height

        self.root = tk.Tk()
        self.root.title(f"MirageTestKit USB-LAN Viewer (:{port})")
        self.root.geometry(f"{width + 40}x{height + 100}")
        self.root.configure(bg='#1e1e2e')

        self.running = False
        self.receiver_thread = None
        self.depacketizer = RtpDepacketizer()
        self.decoder = None
        self.current_frame = None
        self.frame_lock = threading.Lock()

        # Stats
        self.packet_count = 0
        self.byte_count = 0
        self.frame_count = 0
        self.last_frame_time = 0
        self.fps = 0.0
        self.fps_update_time = time.time()
        self.fps_frame_count = 0

        self.setup_ui()

    def setup_ui(self):
        # Title bar
        title_frame = tk.Frame(self.root, bg='#3d3d4d', height=40)
        title_frame.pack(fill='x', padx=5, pady=5)
        title_frame.pack_propagate(False)

        tk.Label(
            title_frame,
            text=f"USB-LAN Video Viewer (Port {self.port})",
            fg='#00ffcc', bg='#3d3d4d',
            font=('Arial', 14, 'bold')
        ).pack(side='left', padx=10, pady=5)

        # Video display
        video_frame = tk.Frame(self.root, bg='#2d2d3d')
        video_frame.pack(fill='both', expand=True, padx=10, pady=5)

        self.video_label = tk.Label(video_frame, bg='black')
        self.video_label.pack(padx=5, pady=5)

        # Stats bar
        stats_frame = tk.Frame(self.root, bg='#2d2d3d', height=30)
        stats_frame.pack(fill='x', padx=10, pady=5)
        stats_frame.pack_propagate(False)

        self.stats_label = tk.Label(
            stats_frame,
            text="Waiting for video...",
            fg='#aaaaaa', bg='#2d2d3d',
            font=('Consolas', 10)
        )
        self.stats_label.pack(side='left', padx=10)

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

    def start_receiver(self):
        """Start UDP receiver thread"""
        self.running = True

        # Start decoder
        self.decoder = VideoDecoder(self.video_width, self.video_height)
        if not self.decoder.start():
            self.status_var.set("ERROR: Failed to start ffmpeg decoder")
            return

        # Start receiver thread
        self.receiver_thread = threading.Thread(target=self._receive_loop, daemon=True)
        self.receiver_thread.start()

        self.status_var.set(f"Listening on UDP port {self.port}")

    def _receive_loop(self):
        """UDP receive loop"""
        sock = None
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
            sock.bind(('0.0.0.0', self.port))
            sock.settimeout(0.5)

            print(f"Listening on UDP port {self.port}")

            while self.running:
                try:
                    data, addr = sock.recvfrom(65535)
                    self.packet_count += 1
                    self.byte_count += len(data)

                    # Process RTP packet
                    nals = self.depacketizer.process_packet(data)
                    for nal in nals:
                        if self.decoder:
                            self.decoder.write_nal(nal)

                    # Get decoded frame
                    if self.decoder:
                        frame = self.decoder.get_frame()
                        if frame:
                            with self.frame_lock:
                                self.current_frame = frame
                            self.frame_count += 1
                            self.last_frame_time = time.time()

                except socket.timeout:
                    pass
                except (OSError, socket.error) as e:
                    if self.running:
                        print(f"Receive error: {e}")
                    break
        except (OSError, socket.error) as e:
            print(f"Failed to bind UDP port {self.port}: {e}")
            self.running = False
        finally:
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass

    def update_display(self):
        """Update display (called from main thread)"""
        # Get current frame
        frame = None
        with self.frame_lock:
            if self.current_frame:
                frame = self.current_frame.copy()

        if frame:
            # Display frame
            try:
                photo = ImageTk.PhotoImage(frame)
                self.video_label.configure(image=photo)
                self.video_label.image = photo
            except Exception:
                pass
        else:
            # Show status image
            self._show_status_image()

        # Update FPS
        now = time.time()
        if now - self.fps_update_time >= 1.0:
            self.fps = self.frame_count - self.fps_frame_count
            self.fps_frame_count = self.frame_count
            self.fps_update_time = now

        # Update stats
        status = "Receiving" if self.frame_count > 0 else "Waiting"
        lost = self.depacketizer.lost_packets
        self.stats_label.config(
            text=f"[{status}] Packets: {self.packet_count} | "
                 f"Data: {self.byte_count/1024/1024:.1f} MB | "
                 f"Frames: {self.frame_count} | "
                 f"FPS: {self.fps:.0f} | "
                 f"Lost: {lost}"
        )

        # Schedule next update
        self.root.after(33, self.update_display)  # ~30fps UI update

    def _show_status_image(self):
        """Show status placeholder when no video"""
        img = Image.new('RGB', (self.video_width, self.video_height), (30, 30, 45))
        draw = ImageDraw.Draw(img)

        y = 50
        draw.text((50, y), "USB-LAN Video Viewer", fill=(255, 255, 255))
        y += 40
        draw.text((50, y), f"Port: {self.port}", fill=(180, 180, 200))
        y += 30

        if self.packet_count > 0:
            draw.text((50, y), f"Packets: {self.packet_count}", fill=(100, 255, 100))
            y += 25
            draw.text((50, y), "Decoding...", fill=(255, 200, 100))
        else:
            draw.text((50, y), "Waiting for video stream...", fill=(255, 150, 150))
            y += 40
            draw.text((50, y), "1. Enable USB tethering on Android", fill=(150, 150, 200))
            y += 25
            draw.text((50, y), "2. Start MirageAndroid USB-LAN mode", fill=(150, 150, 200))
            y += 25
            draw.text((50, y), "3. Grant screen capture permission", fill=(150, 150, 200))

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
        """Run the viewer"""
        self.start_receiver()
        self.update_display()
        self.root.mainloop()

    def stop(self):
        """Stop the viewer"""
        self.running = False
        if self.decoder:
            self.decoder.stop()


def main():
    parser = argparse.ArgumentParser(description='USB-LAN Video Viewer for MirageTestKit')
    parser.add_argument('--port', type=int, default=DEFAULT_PORT, help=f'UDP port (default: {DEFAULT_PORT})')
    parser.add_argument('--width', type=int, default=VIDEO_WIDTH, help=f'Video width (default: {VIDEO_WIDTH})')
    parser.add_argument('--height', type=int, default=VIDEO_HEIGHT, help=f'Video height (default: {VIDEO_HEIGHT})')

    args = parser.parse_args()

    print("=" * 60)
    print("MirageTestKit - USB-LAN Video Viewer")
    print("=" * 60)

    # Check ffmpeg
    try:
        subprocess.run(['ffmpeg', '-version'], capture_output=True, timeout=5)
        print("[OK] ffmpeg found")
    except Exception:
        print("[ERROR] ffmpeg not found! Please install ffmpeg.")
        sys.exit(1)

    print(f"Listening on UDP port {args.port}")
    print(f"Video size: {args.width}x{args.height}")
    print()

    viewer = UsbLanVideoViewer(args.port, args.width, args.height)
    try:
        viewer.run()
    except KeyboardInterrupt:
        pass
    finally:
        viewer.stop()


if __name__ == '__main__':
    main()
