#!/usr/bin/env python3
"""
Bluetooth Pairing Automation with OCR for MirageSystem
======================================================

Uses ADB screenshots to read Bluetooth pairing PIN and auto-confirm
on Android devices.

Usage:
  python bluetooth_pairing.py [slot] [device]

Examples:
  python bluetooth_pairing.py            # slot=0, auto-detect device
  python bluetooth_pairing.py 1          # slot=1
  python bluetooth_pairing.py 0 192.168.0.7:5555

Flow:
  1. Take screenshot via ADB
  2. OCR detect PIN code (6 digits)
  3. If PIN found, auto-tap "Pair" button on Android

Requirements:
  pip install pytesseract pillow opencv-python

  Tesseract-OCR must also be installed on the system:
    https://github.com/UB-Mannheim/tesseract/wiki

Environment:
  MIRAGE_HOME  - MirageComplete root directory (auto-detected if unset)
  ADB          - Path to adb executable (auto-detected via PATH if unset)
"""

import subprocess
import shutil
import socket
import struct
import time
import re
import sys
import os
import tempfile
from pathlib import Path
from typing import Optional, Tuple

# Project root: scripts/ai/bluetooth_pairing.py -> ../../
MIRAGE_HOME = os.environ.get(
    'MIRAGE_HOME',
    os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
)

# ADB path: environment variable or auto-detect from PATH
ADB = os.environ.get('ADB') or shutil.which('adb') or 'adb'

# Try to import optional dependencies
try:
    import cv2
    import numpy as np
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False
    print("[WARN] opencv-python not installed. Using basic mode.")

try:
    import pytesseract
    HAS_TESSERACT = True
except ImportError:
    HAS_TESSERACT = False
    print("[WARN] pytesseract not installed. OCR disabled.")

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


class ScreenCapture:
    """Capture screen from UDP mirror stream"""

    def __init__(self, port: int = 50000, timeout: float = 2.0):
        self.port = port
        self.timeout = timeout
        self.sock = None

    def __enter__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(('0.0.0.0', self.port))
        self.sock.settimeout(self.timeout)
        return self

    def __exit__(self, *args):
        if self.sock:
            self.sock.close()

    def capture_frame(self) -> Optional[bytes]:
        """Capture a single frame (simplified - real impl needs RTP/H264 decode)"""
        try:
            data, addr = self.sock.recvfrom(65535)
            return data
        except socket.timeout:
            return None


def extract_pin_from_text(text: str) -> Optional[str]:
    """Extract 6-digit PIN from OCR text"""
    patterns = [
        r'\b(\d{6})\b',           # 6 consecutive digits
        r'(\d{3}\s*\d{3})',       # 3+3 with space
        r'(\d{2}\s*\d{2}\s*\d{2})', # 2+2+2 with spaces
    ]

    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            pin = re.sub(r'\s', '', match.group(1))
            if len(pin) == 6 and pin.isdigit():
                return pin
    return None


def detect_pairing_dialog(image_path: str) -> Tuple[Optional[str], Optional[dict]]:
    """
    Detect Bluetooth pairing dialog and extract PIN.
    Returns (pin, button_coords) or (None, None)
    """
    if not HAS_TESSERACT or not HAS_CV2:
        print("[ERROR] OCR requires pytesseract and opencv-python")
        return None, None

    img = cv2.imread(image_path)
    if img is None:
        return None, None

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    text = pytesseract.image_to_string(gray)
    print(f"[OCR] Detected text:\n{text[:200]}...")

    pin = extract_pin_from_text(text)
    if pin:
        print(f"[PIN] Found: {pin}")

    button_coords = None
    try:
        data = pytesseract.image_to_data(gray, output_type=pytesseract.Output.DICT)
        for i, word in enumerate(data['text']):
            word_lower = word.lower() if word else ""
            if any(x in word_lower for x in ['pair', 'ペア', 'ok', '確認']):
                x = data['left'][i]
                y = data['top'][i]
                w = data['width'][i]
                h = data['height'][i]
                if w > 10 and h > 10:
                    button_coords = {
                        'x': x + w // 2,
                        'y': y + h // 2,
                        'text': word
                    }
                    print(f"[BUTTON] Found '{word}' at ({button_coords['x']}, {button_coords['y']})")
                    break
    except Exception as e:
        print(f"[WARN] Button detection failed: {e}")

    return pin, button_coords


def send_tap_command(slot: int, x: int, y: int):
    """Send tap command to miraged via IPC"""
    try:
        ipc_exe = Path(MIRAGE_HOME) / "build" / "ipc_cmd.exe"
        if not ipc_exe.exists():
            # Fallback: check pc/ directory
            ipc_exe = Path(MIRAGE_HOME) / "pc" / "ipc_cmd.exe"
        if ipc_exe.exists():
            cmd = f'{{"type":"tap","slot":{slot},"x":{x},"y":{y}}}'
            result = subprocess.run(
                [str(ipc_exe), cmd],
                capture_output=True,
                text=True,
                timeout=5
            )
            print(f"[TAP] slot={slot} x={x} y={y} -> {result.stdout.strip()}")
            return True
    except Exception as e:
        print(f"[ERROR] Tap failed: {e}")
    return False


def take_screenshot_adb(device: str = None) -> Optional[str]:
    """Take screenshot using ADB"""
    temp_path = os.path.join(tempfile.gettempdir(), "mirage_bt_screen.png")

    cmd = [ADB]
    if device:
        cmd.extend(["-s", device])

    try:
        subprocess.run(cmd + ["shell", "screencap", "-p", "/sdcard/bt_screen.png"],
                      capture_output=True, timeout=10)
        subprocess.run(cmd + ["pull", "/sdcard/bt_screen.png", temp_path],
                      capture_output=True, timeout=10)
        subprocess.run(cmd + ["shell", "rm", "/sdcard/bt_screen.png"],
                      capture_output=True, timeout=5)

        if os.path.exists(temp_path):
            return temp_path
    except Exception as e:
        print(f"[ERROR] Screenshot failed: {e}")
    return None


def auto_pair_bluetooth(slot: int = 0, device: str = None, max_attempts: int = 10):
    """
    Main pairing automation loop.

    Monitors screen for Bluetooth pairing dialog,
    extracts PIN, and taps "Pair" button.
    """
    print(f"=== Bluetooth Pairing Automation ===")
    print(f"Slot: {slot}")
    print(f"Device: {device or 'auto'}")
    print(f"ADB: {ADB}")
    print(f"MIRAGE_HOME: {MIRAGE_HOME}")
    print(f"Watching for pairing dialog...")
    print()

    for attempt in range(max_attempts):
        print(f"[{attempt+1}/{max_attempts}] Checking screen...")

        screenshot = take_screenshot_adb(device)
        if not screenshot:
            print("  No screenshot available, waiting...")
            time.sleep(2)
            continue

        pin, button = detect_pairing_dialog(screenshot)

        if pin and button:
            print(f"\n[SUCCESS] Pairing dialog detected!")
            print(f"  PIN: {pin}")
            print(f"  Button: {button['text']} at ({button['x']}, {button['y']})")

            if send_tap_command(slot, button['x'], button['y']):
                print(f"  Tapped Pair button!")
                print(f"\n=== Pairing should complete ===")
                return True
            else:
                print(f"  [WARN] Tap command failed, trying ADB input...")
                cmd = [ADB]
                if device:
                    cmd.extend(["-s", device])
                cmd.extend(["shell", "input", "tap", str(button['x']), str(button['y'])])
                subprocess.run(cmd, capture_output=True, timeout=5)
                return True

        elif pin:
            print(f"  PIN found: {pin}, but no button detected")
            print(f"  Trying common button positions...")

        # Cleanup
        try:
            os.remove(screenshot)
        except:
            pass

        time.sleep(2)

    print("\n[TIMEOUT] No pairing dialog detected")
    return False


def main():
    slot = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    device = sys.argv[2] if len(sys.argv) > 2 else None

    if not HAS_TESSERACT:
        print("\n[SETUP] Install pytesseract:")
        print("  pip install pytesseract")
        print("  And install Tesseract-OCR from:")
        print("  https://github.com/UB-Mannheim/tesseract/wiki")

    if not HAS_CV2:
        print("\n[SETUP] Install opencv:")
        print("  pip install opencv-python")

    if not HAS_TESSERACT or not HAS_CV2:
        print("\n[WARN] Running in limited mode without OCR")

    auto_pair_bluetooth(slot, device)


if __name__ == "__main__":
    main()
