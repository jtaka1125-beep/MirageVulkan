#!/usr/bin/env python3
"""
OCR-based Auto-Pairing for MirageSystem
========================================

Handles both Bluetooth and WiFi ADB pairing by reading codes from the
Android screen via ADB screenshot + OCR.

Usage:
  python auto_pairing_ocr.py bluetooth [device]
  python auto_pairing_ocr.py wifi_adb [device]
  python auto_pairing_ocr.py auto [device]

Examples:
  python auto_pairing_ocr.py wifi_adb
  python auto_pairing_ocr.py bluetooth 192.168.0.7:5555
  python auto_pairing_ocr.py auto

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
import time
import re
import sys
import os
import tempfile
from pathlib import Path
from typing import Optional, Tuple, List
from dataclasses import dataclass

# Project root: scripts/ai/auto_pairing_ocr.py -> ../../
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

try:
    import pytesseract
    HAS_TESSERACT = True
except ImportError:
    HAS_TESSERACT = False

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False


@dataclass
class PairingInfo:
    """Detected pairing information"""
    type: str  # 'bluetooth' or 'wifi_adb'
    code: str  # 6-digit code
    ip_port: Optional[str] = None  # For WiFi ADB: "192.168.x.x:xxxxx"
    button_pos: Optional[Tuple[int, int]] = None  # For auto-tap


def take_screenshot_adb(device: str = None) -> Optional[str]:
    """Take screenshot using ADB"""
    temp_path = os.path.join(tempfile.gettempdir(), "mirage_pairing_screen.png")

    cmd = [ADB]
    if device:
        cmd.extend(["-s", device])

    try:
        subprocess.run(
            cmd + ["shell", "screencap", "-p", "/sdcard/pairing_screen.png"],
            capture_output=True, timeout=10
        )
        subprocess.run(
            cmd + ["pull", "/sdcard/pairing_screen.png", temp_path],
            capture_output=True, timeout=10
        )
        subprocess.run(
            cmd + ["shell", "rm", "/sdcard/pairing_screen.png"],
            capture_output=True, timeout=5
        )

        if os.path.exists(temp_path):
            return temp_path
    except Exception as e:
        print(f"[ERROR] Screenshot failed: {e}")
    return None


def ocr_screen(image_path: str) -> Tuple[str, List[dict]]:
    """
    OCR the screen and return (full_text, word_positions).
    word_positions = [{'text': 'word', 'x': int, 'y': int, 'w': int, 'h': int}, ...]
    """
    if not HAS_TESSERACT or not HAS_CV2:
        return "", []

    img = cv2.imread(image_path)
    if img is None:
        return "", []

    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    full_text = pytesseract.image_to_string(gray, lang='eng+jpn')

    word_positions = []
    try:
        data = pytesseract.image_to_data(gray, output_type=pytesseract.Output.DICT)
        for i, word in enumerate(data['text']):
            if word and len(word.strip()) > 0:
                word_positions.append({
                    'text': word,
                    'x': data['left'][i],
                    'y': data['top'][i],
                    'w': data['width'][i],
                    'h': data['height'][i]
                })
    except Exception as e:
        print(f"[WARN] Word position extraction failed: {e}")

    return full_text, word_positions


def extract_6digit_code(text: str) -> Optional[str]:
    """Extract 6-digit code from text"""
    patterns = [
        r'\b(\d{6})\b',              # 123456
        r'(\d{3}[\s\-]?\d{3})',      # 123 456 or 123-456
        r'(\d{2}[\s\-]?\d{2}[\s\-]?\d{2})',  # 12 34 56
    ]

    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            code = re.sub(r'[\s\-]', '', match.group(1))
            if len(code) == 6 and code.isdigit():
                return code
    return None


def extract_wifi_adb_info(text: str) -> Tuple[Optional[str], Optional[str]]:
    """
    Extract WiFi ADB pairing info from screen.
    Returns (ip:port, pairing_code)
    """
    ip_pattern = r'(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}):(\d{4,5})'
    ip_match = re.search(ip_pattern, text)

    ip_port = None
    if ip_match:
        ip_port = f"{ip_match.group(1)}:{ip_match.group(2)}"

    code = extract_6digit_code(text)

    return ip_port, code


def find_button(word_positions: List[dict], keywords: List[str]) -> Optional[Tuple[int, int]]:
    """Find button position by matching keywords"""
    for word_info in word_positions:
        word_lower = word_info['text'].lower()
        for kw in keywords:
            if kw.lower() in word_lower:
                cx = word_info['x'] + word_info['w'] // 2
                cy = word_info['y'] + word_info['h'] // 2
                return (cx, cy)
    return None


def detect_pairing_screen(image_path: str) -> Optional[PairingInfo]:
    """Analyze screenshot to detect pairing dialog type and extract info."""
    full_text, word_positions = ocr_screen(image_path)

    if not full_text:
        return None

    text_lower = full_text.lower()
    print(f"[OCR] Text preview:\n{full_text[:300]}...")

    # Detect WiFi ADB pairing screen
    wifi_adb_keywords = ['wireless', 'ワイヤレス', 'pairing code', 'ペアリング',
                          'wifi debug', 'adb', 'developer']
    if any(kw in text_lower for kw in wifi_adb_keywords):
        ip_port, code = extract_wifi_adb_info(full_text)
        if code:
            print(f"[DETECT] WiFi ADB pairing screen")
            return PairingInfo(
                type='wifi_adb',
                code=code,
                ip_port=ip_port
            )

    # Detect Bluetooth pairing screen
    bt_keywords = ['bluetooth', 'ブルートゥース', 'pair', 'ペアリング',
                   'passkey', 'pin', 'confirm']
    if any(kw in text_lower for kw in bt_keywords):
        code = extract_6digit_code(full_text)
        if code:
            print(f"[DETECT] Bluetooth pairing screen")
            button_keywords = ['pair', 'ペア', 'ok', '確認', 'accept', '許可']
            button_pos = find_button(word_positions, button_keywords)
            return PairingInfo(
                type='bluetooth',
                code=code,
                button_pos=button_pos
            )

    return None


def execute_wifi_adb_pair(ip_port: str, code: str) -> bool:
    """Execute adb pair command with the code"""
    print(f"[ADB] Pairing with {ip_port} using code {code}...")

    try:
        result = subprocess.run(
            [ADB, "pair", ip_port, code],
            capture_output=True,
            text=True,
            timeout=30
        )

        output = result.stdout + result.stderr
        print(f"[ADB] Output: {output}")

        if "success" in output.lower() or "paired" in output.lower():
            print(f"[SUCCESS] WiFi ADB paired!")

            ip = ip_port.split(':')[0]
            connect_result = subprocess.run(
                [ADB, "connect", f"{ip}:5555"],
                capture_output=True,
                text=True,
                timeout=10
            )
            print(f"[ADB] Connect: {connect_result.stdout}")
            return True

    except Exception as e:
        print(f"[ERROR] Pairing failed: {e}")
    return False


def send_tap_adb(device: str, x: int, y: int) -> bool:
    """Send tap via ADB input"""
    cmd = [ADB]
    if device:
        cmd.extend(["-s", device])
    cmd.extend(["shell", "input", "tap", str(x), str(y)])

    try:
        subprocess.run(cmd, capture_output=True, timeout=5)
        print(f"[TAP] Sent tap at ({x}, {y})")
        return True
    except Exception as e:
        print(f"[ERROR] Tap failed: {e}")
    return False


def auto_pair_loop(mode: str, device: str = None, max_attempts: int = 30, interval: float = 2.0):
    """
    Main pairing automation loop.

    Args:
        mode: 'bluetooth' or 'wifi_adb' or 'auto' (detect automatically)
        device: ADB device serial (optional)
        max_attempts: Maximum screenshot attempts
        interval: Seconds between attempts
    """
    print(f"=== Auto Pairing with OCR ===")
    print(f"Mode: {mode}")
    print(f"Device: {device or 'auto'}")
    print(f"ADB: {ADB}")
    print(f"MIRAGE_HOME: {MIRAGE_HOME}")
    print(f"Watching for pairing dialog...")
    print()

    if not HAS_TESSERACT or not HAS_CV2:
        print("[ERROR] OCR requires: pip install pytesseract opencv-python")
        print("Also install Tesseract-OCR from:")
        print("  https://github.com/UB-Mannheim/tesseract/wiki")
        return False

    for attempt in range(max_attempts):
        print(f"\r[{attempt+1}/{max_attempts}] Checking...", end='', flush=True)

        screenshot = take_screenshot_adb(device)
        if not screenshot:
            time.sleep(interval)
            continue

        info = detect_pairing_screen(screenshot)

        try:
            os.remove(screenshot)
        except:
            pass

        if not info:
            time.sleep(interval)
            continue

        print()  # Newline after progress

        if mode != 'auto' and info.type != mode:
            print(f"[SKIP] Detected {info.type} but looking for {mode}")
            time.sleep(interval)
            continue

        print(f"\n[FOUND] {info.type} pairing screen!")
        print(f"  Code: {info.code}")

        if info.type == 'wifi_adb':
            if info.ip_port:
                print(f"  IP:Port: {info.ip_port}")
                if execute_wifi_adb_pair(info.ip_port, info.code):
                    return True
            else:
                print(f"  [WARN] Could not extract IP:Port from screen")
                print(f"  Manual pair: adb pair <ip:port> {info.code}")

        elif info.type == 'bluetooth':
            if info.button_pos:
                print(f"  Button at: {info.button_pos}")
                if send_tap_adb(device, info.button_pos[0], info.button_pos[1]):
                    print(f"[SUCCESS] Tapped Pair button!")
                    return True
            else:
                print(f"  [WARN] Could not find Pair button")
                print(f"  Please tap Pair manually")

        time.sleep(interval)

    print(f"\n[TIMEOUT] No pairing dialog detected after {max_attempts} attempts")
    return False


def main():
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python auto_pairing_ocr.py bluetooth [device]")
        print("  python auto_pairing_ocr.py wifi_adb [device]")
        print("  python auto_pairing_ocr.py auto [device]")
        print()
        print("Examples:")
        print("  python auto_pairing_ocr.py wifi_adb")
        print("  python auto_pairing_ocr.py bluetooth 192.168.0.7:5555")
        print("  python auto_pairing_ocr.py auto")
        return

    mode = sys.argv[1].lower()
    device = sys.argv[2] if len(sys.argv) > 2 else None

    if mode not in ['bluetooth', 'wifi_adb', 'auto']:
        print(f"Unknown mode: {mode}")
        print("Use: bluetooth, wifi_adb, or auto")
        return

    auto_pair_loop(mode, device)


if __name__ == "__main__":
    main()
