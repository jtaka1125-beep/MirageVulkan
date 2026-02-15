import subprocess, time, sys, argparse, socket, threading, re, os
from pathlib import Path

# Load config from config.json
try:
    from config_loader import get_pc_ip, get_video_base_port
    PC_IP = get_pc_ip()
    BASE_PORT = get_video_base_port()
except ImportError:
    # Fallback if config_loader not available
    PC_IP = "192.168.0.8"
    BASE_PORT = 60000
# OCR dependencies (optional)
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

def get_connected_devices():
    result = subprocess.run(["adb", "devices", "-l"], capture_output=True, text=True)
    devices = []
    for line in result.stdout.strip().split(chr(10))[1:]:
        if chr(9)+"device" in line or " device " in line:
            parts = line.split()
            serial = parts[0]
            if ":" in serial: continue
            model = "Unknown"
            for p in parts:
                if p.startswith("model:"): model = p.replace("model:", ""); break
            devices.append({"serial": serial, "model": model})
    return devices

def check_accessibility_enabled(serial):
    result = subprocess.run(["adb", "-s", serial, "shell", "settings", "get", "secure", "enabled_accessibility_services"], capture_output=True, text=True)
    return "com.mirage.android" in result.stdout

def enable_accessibility_via_adb(serial, device_name):
    print(f"    {device_name}: Enabling accessibility via ADB...")
    subprocess.run(["adb", "-s", serial, "shell", "settings", "put", "secure", "enabled_accessibility_services", "com.mirage.android/com.mirage.android.access.MirageAccessibilityService"], capture_output=True)
    subprocess.run(["adb", "-s", serial, "shell", "settings", "put", "secure", "accessibility_enabled", "1"], capture_output=True)
    if check_accessibility_enabled(serial):
        print(f"    {device_name}: Accessibility enabled via ADB (OK)")
        return True
    return False

def go_home(serial):
    subprocess.run(["adb", "-s", serial, "shell", "input", "keyevent", "KEYCODE_HOME"], capture_output=True)

def stop_app(serial):
    subprocess.run(["adb", "-s", serial, "shell", "am", "force-stop", "com.mirage.android"], capture_output=True)

def launch_mirror(serial, host, port):
    print(f"  Launching mirror on {serial} -> {host}:{port}")
    subprocess.run(["adb", "-s", serial, "shell", "am", "start", "-n", "com.mirage.android/.ui.MainActivity", "--ez", "auto_mirror", "true", "--es", "mirror_host", host, "--ei", "mirror_port", str(port)], capture_output=True)

def get_android_version(serial):
    result = subprocess.run(["adb", "-s", serial, "shell", "getprop", "ro.build.version.sdk"], capture_output=True, text=True)
    try: return int(result.stdout.strip())
    except ValueError:
        print(f"    Warning: Could not parse SDK version for {serial}")
        return 0

def setup_permissions_all_devices(devices):
    print(chr(10)+"[*] Setting up permissions...")
    for dev in devices:
        enable_accessibility_via_adb(dev["serial"], dev["model"])
        time.sleep(0.5)
        go_home(dev["serial"])

# ========== OCR Functions ==========
def take_screenshot_adb(serial):
    temp_path = Path(__file__).parent / f"temp_screen_{serial}.png"
    try:
        subprocess.run(["adb", "-s", serial, "shell", "screencap", "-p", "/sdcard/ocr_screen.png"], capture_output=True, timeout=10)
        subprocess.run(["adb", "-s", serial, "pull", "/sdcard/ocr_screen.png", str(temp_path)], capture_output=True, timeout=10)
        subprocess.run(["adb", "-s", serial, "shell", "rm", "/sdcard/ocr_screen.png"], capture_output=True, timeout=5)
        if temp_path.exists(): return str(temp_path)
    except Exception:
        print(f"    Screenshot error: {e}")
    return None

def ocr_screen(image_path):
    if not HAS_TESSERACT or not HAS_CV2: return "", []
    img = cv2.imread(image_path)
    if img is None: return "", []
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    full_text = pytesseract.image_to_string(gray, lang='eng+jpn')
    word_positions = []
    try:
        data = pytesseract.image_to_data(gray, output_type=pytesseract.Output.DICT)
        for i, word in enumerate(data['text']):
            if word and len(word.strip()) > 0:
                word_positions.append({'text': word, 'x': data['left'][i], 'y': data['top'][i], 'w': data['width'][i], 'h': data['height'][i]})
    except Exception:
        pass  # OCR word extraction failed, continue with basic text
    return full_text, word_positions

def extract_6digit_code(text):
    for pattern in [r'\b(\d{6})\b', r'(\d{3}[\s\-]?\d{3})']:
        match = re.search(pattern, text)
        if match:
            code = re.sub(r'[\s\-]', '', match.group(1))
            if len(code) == 6 and code.isdigit(): return code
    return None

def find_button_position(word_positions, keywords):
    for word_info in word_positions:
        word_lower = word_info['text'].lower()
        for kw in keywords:
            if kw.lower() in word_lower:
                return (word_info['x'] + word_info['w'] // 2, word_info['y'] + word_info['h'] // 2)
    return None

def tap_screen(serial, x, y):
    subprocess.run(["adb", "-s", serial, "shell", "input", "tap", str(x), str(y)], capture_output=True)
    print(f"    Tap sent at ({x}, {y})")

# ========== WiFi ADB Pairing (OCR) ==========
def setup_wifi_adb_ocr(serial, device_name, max_attempts=20):
    if not HAS_TESSERACT or not HAS_CV2:
        print(f"    {device_name}: OCR not available (install pytesseract, opencv-python)")
        return False
    print(f"    {device_name}: Starting WiFi ADB setup via OCR...")
    print(f"    Please open: Settings > Developer Options > Wireless debugging > Pair device")
    for attempt in range(max_attempts):
        screenshot = take_screenshot_adb(serial)
        if not screenshot: time.sleep(1); continue
        full_text, _ = ocr_screen(screenshot)
        try:
            os.remove(screenshot)
        except (OSError, IOError) as e:
            # Log but don't fail - temp files will be cleaned up eventually
            print(f"    [DEBUG] Failed to remove temp file {screenshot}: {e}")
        ip_match = re.search(r'(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}):(\d{4,5})', full_text)
        code = extract_6digit_code(full_text)
        if ip_match and code:
            ip_port = f"{ip_match.group(1)}:{ip_match.group(2)}"
            print(f"    {device_name}: Found pairing - IP:Port={ip_port}, Code={code}")
            result = subprocess.run(["adb", "pair", ip_port, code], capture_output=True, text=True, timeout=30)
            output = result.stdout + result.stderr
            if "success" in output.lower() or "paired" in output.lower():
                print(f"    {device_name}: WiFi ADB paired successfully!")
                ip = ip_match.group(1)
                subprocess.run(["adb", "connect", f"{ip}:5555"], capture_output=True, timeout=10)
                return True
            else:
                print(f"    {device_name}: Pairing failed - {output.strip()}")
        print(f"    [{attempt+1}/{max_attempts}] Waiting for pairing screen...", end=chr(13))
        time.sleep(2)
    print(f"    {device_name}: WiFi ADB setup timeout")
    return False

# ========== Bluetooth Pairing for ADB (OCR) ==========
def setup_bluetooth_adb_ocr(serial, device_name, max_attempts=30):
    if not HAS_TESSERACT or not HAS_CV2:
        print(f"    {device_name}: OCR not available (install pytesseract, opencv-python)")
        return False
    print(f"    {device_name}: Starting Bluetooth ADB setup via OCR...")
    print(f"    Please initiate Bluetooth pairing from PC")
    for attempt in range(max_attempts):
        screenshot = take_screenshot_adb(serial)
        if not screenshot: time.sleep(1); continue
        full_text, word_positions = ocr_screen(screenshot)
        try:
            os.remove(screenshot)
        except (OSError, IOError) as e:
            print(f"    [DEBUG] Failed to remove temp file {screenshot}: {e}")
        text_lower = full_text.lower()
        bt_keywords = ['bluetooth', 'pair', 'passkey', 'pin', 'confirm', 'ペア', 'ブルートゥース']
        if any(kw in text_lower for kw in bt_keywords):
            code = extract_6digit_code(full_text)
            if code:
                print(f"    {device_name}: Found Bluetooth pairing dialog! PIN={code}")
                button_keywords = ['pair', 'ペア', 'ok', '確認', 'accept', '許可', 'allow']
                button_pos = find_button_position(word_positions, button_keywords)
                if button_pos:
                    print(f"    {device_name}: Tapping Pair button at {button_pos}")
                    tap_screen(serial, button_pos[0], button_pos[1])
                    time.sleep(1)
                    print(f"    {device_name}: Bluetooth pairing confirmed!")
                    return True
                else:
                    print(f"    {device_name}: Pair button not found, please tap manually")
        print(f"    [{attempt+1}/{max_attempts}] Waiting for Bluetooth pairing dialog...", end=chr(13))
        time.sleep(2)
    print(f"    {device_name}: Bluetooth pairing timeout")
    return False

def setup_wifi_adb_all_devices(devices):
    print(chr(10)+"[*] Setting up WiFi ADB via OCR...")
    for dev in devices:
        setup_wifi_adb_ocr(dev["serial"], dev["model"])
        time.sleep(0.5)

def setup_bluetooth_adb_all_devices(devices):
    print(chr(10)+"[*] Setting up Bluetooth ADB via OCR...")
    for dev in devices:
        setup_bluetooth_adb_ocr(dev["serial"], dev["model"])
        time.sleep(0.5)

def main():
    parser = argparse.ArgumentParser(description="MirageTestKit Auto Device Setup")
    parser.add_argument("--ip", default=PC_IP)
    parser.add_argument("--base-port", type=int, default=BASE_PORT)
    parser.add_argument("--setup-permissions", action="store_true", help="Setup accessibility permissions via ADB")
    parser.add_argument("--setup-wifi", action="store_true", help="Setup WiFi ADB via OCR")
    parser.add_argument("--setup-bluetooth", action="store_true", help="Setup Bluetooth ADB via OCR")
    parser.add_argument("--no-launch", action="store_true", help="Skip launching mirror app")
    args = parser.parse_args()
    print("=" * 60 + chr(10) + "MirageTestKit - Auto Device Setup" + chr(10) + "=" * 60)
    devices = get_connected_devices()
    if not devices: print("No devices found!"); sys.exit(1)
    print(f"Found {len(devices)} device(s):")
    for i, dev in enumerate(devices):
        dev["sdk"] = get_android_version(dev["serial"])
        print(f"  {i+1}. {dev['model']} -> Port {args.base_port + i}")
    if args.setup_permissions: setup_permissions_all_devices(devices)
    if args.setup_wifi: setup_wifi_adb_all_devices(devices)
    if args.setup_bluetooth: setup_bluetooth_adb_all_devices(devices)
    if not args.no_launch:
        for dev in devices: stop_app(dev["serial"])
        time.sleep(1)
        for i, dev in enumerate(devices):
            launch_mirror(dev["serial"], args.ip, args.base_port + i)
    print(chr(10)+"Setup complete!")

if __name__ == "__main__": main()

