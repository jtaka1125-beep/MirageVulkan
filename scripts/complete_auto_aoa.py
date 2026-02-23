#!/usr/bin/env python3
"""
完全自動AOAセットアップ
======================

1. 権限設定（通知、Accessibility）
2. WinUSBドライバ自動インストール（wdi-simple.exe使用）
3. AOA切替
4. AOA許可ダイアログ自動承認 (uiautomator)

Usage:
  python scripts/complete_auto_aoa.py
  python scripts/complete_auto_aoa.py --wdi-path C:\\path\\to\\wdi-simple.exe

Environment:
  MIRAGE_HOME  - MirageComplete root directory (auto-detected if unset)
  WDI_PATH     - wdi-simple.exe のパス（--wdi-path 引数でも指定可能）

Note:
  既存のバッチスクリプトとの関係:
  - scripts/setup_aoa_winusb.bat: AOA切替 + WinUSBインストール（PowerShell版）
    -> 本スクリプトの Phase 3-4 に相当。bat版は install_aoa_winusb.ps1 を使用
  - scripts/aoa_workflow.bat: ADB→WiFi ADB→WinUSB→AOAテストの一連フロー
    -> 本スクリプトの Phase 1-4 全体に相当。bat版はデバイスシリアルがハードコード
  本スクリプトはこれらを Python で統合し、デバイス自動検出に対応したもの。
"""
import subprocess
import shutil
import time
import sys
import os
import argparse
import ctypes
import json
import re
import xml.etree.ElementTree as ET

# Project root: scripts/complete_auto_aoa.py -> ../
MIRAGE_HOME = os.environ.get(
    'MIRAGE_HOME',
    os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
)

# 2-APK split (2026-02-24): accessory handles AOA, capture handles video
PACKAGE_ACCESSORY = "com.mirage.accessory"
PACKAGE_CAPTURE   = "com.mirage.capture"
ACCESSIBILITY_SERVICE = "com.mirage.accessory.access.MirageAccessibilityService"

# Legacy alias for permission setup (kept for apps still using old package on device)
PACKAGE = PACKAGE_ACCESSORY

# AOA switch: MirageComplete/build/aoa_switch.exe
AOA_SWITCH_PATH = os.path.join(MIRAGE_HOME, "build", "aoa_switch.exe")

# WDI simple: 環境変数 WDI_PATH または引数で指定（デフォルトなし）
WDI_SIMPLE_PATH = os.environ.get('WDI_PATH', '')

# ADB path: auto-detect from PATH
ADB = os.environ.get('ADB') or shutil.which('adb') or 'adb'

# MediaTek VID
MEDIATEK_VID = "0E8D"
# Google AOA VID/PID
AOA_VID = "18D1"
AOA_PID = "2D01"

def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except:
        return False

def run_as_admin():
    if not is_admin():
        print("[!] 管理者権限が必要です。再起動します...")
        ctypes.windll.shell32.ShellExecuteW(
            None, "runas", sys.executable, f'"{__file__}"', None, 1
        )
        sys.exit(0)

def run_cmd(cmd, timeout=30):
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, shell=True)
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except Exception as e:
        return False, "", str(e)

def run_adb(args, serial=None):
    cmd = [ADB]
    if serial:
        cmd.extend(['-s', serial])
    cmd.extend(args)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except:
        return False, "", ""

def shell(cmd, serial=None):
    success, out, err = run_adb(['shell'] + cmd.split(), serial)
    return out

def get_devices():
    success, out, _ = run_adb(['devices', '-l'])
    devices = []
    if not success:
        return devices
    for line in out.strip().split('\n')[1:]:
        if 'device' in line and ':' not in line.split()[0]:
            parts = line.split()
            serial = parts[0]
            model = "Unknown"
            for p in parts:
                if p.startswith('model:'):
                    model = p.split(':')[1]
            devices.append((serial, model))
    return devices

# ===== Phase 1: Android権限設定 =====

def setup_android_permissions(serial):
    """Android側の権限を全て設定"""
    print(f"  [1] 通知権限 (accessory + capture)...")
    for pkg in [PACKAGE_ACCESSORY, PACKAGE_CAPTURE]:
        run_adb(['shell', 'cmd', 'appops', 'set', pkg, 'POST_NOTIFICATION', 'allow'], serial)

    print(f"  [2] Accessibility (MirageAccessory)...")
    full_service = f"{PACKAGE_ACCESSORY}/{ACCESSIBILITY_SERVICE}"
    current = shell("settings get secure enabled_accessibility_services", serial)
    if full_service not in (current or ""):
        if current and current != "null" and current.strip():
            new_value = f"{current}:{full_service}"
        else:
            new_value = full_service
        run_adb(['shell', 'settings', 'put', 'secure', 'enabled_accessibility_services', new_value], serial)
        run_adb(['shell', 'settings', 'put', 'secure', 'accessibility_enabled', '1'], serial)

    print(f"  [3] ランタイム権限...")
    for pkg in [PACKAGE_ACCESSORY, PACKAGE_CAPTURE]:
        permissions = [
            "android.permission.RECORD_AUDIO",
            "android.permission.POST_NOTIFICATIONS",
        ]
        for perm in permissions:
            run_adb(['shell', 'pm', 'grant', pkg, perm], serial)

    appops = ["SYSTEM_ALERT_WINDOW", "RUN_IN_BACKGROUND", "RUN_ANY_IN_BACKGROUND"]
    for pkg in [PACKAGE_ACCESSORY, PACKAGE_CAPTURE]:
        for op in appops:
            run_adb(['shell', 'appops', 'set', pkg, op, 'allow'], serial)

    print(f"  [4] バッテリー最適化除外...")
    for pkg in [PACKAGE_ACCESSORY, PACKAGE_CAPTURE]:
        run_adb(['shell', 'dumpsys', 'deviceidle', 'whitelist', f'+{pkg}'], serial)

    print(f"  [5] MirageAccessory 起動...")
    run_adb(['shell', 'am', 'start', '-n',
             f'{PACKAGE_ACCESSORY}/.ui.AccessoryActivity'], serial)

    print("      Done")

# ===== Phase 2: USB デバイス検出 =====

def get_usb_devices():
    """USBデバイス一覧を取得"""
    ps_script = '''
Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like "*VID_*"
} | Select-Object InstanceId, FriendlyName, Status | ConvertTo-Json -Compress
'''
    success, out, _ = run_cmd(f'powershell -NoProfile -Command "{ps_script}"')
    if success and out:
        try:
            devices = json.loads(out)
            if isinstance(devices, dict):
                devices = [devices]
            return devices
        except:
            pass
    return []

def find_android_usb_devices():
    """Androidデバイス（ADB/MTPモード）を検出"""
    devices = get_usb_devices()
    android_devices = []

    for dev in devices:
        instance_id = dev.get('InstanceId', '')
        if f'VID_{MEDIATEK_VID}' in instance_id.upper():
            match = re.search(r'VID_([0-9A-F]{4})&PID_([0-9A-F]{4})', instance_id.upper())
            if match:
                android_devices.append({
                    'instance_id': instance_id,
                    'vid': match.group(1),
                    'pid': match.group(2),
                    'name': dev.get('FriendlyName', 'Unknown')
                })
        elif f'VID_{AOA_VID}' in instance_id.upper() and 'PID_2D0' not in instance_id.upper():
            match = re.search(r'VID_([0-9A-F]{4})&PID_([0-9A-F]{4})', instance_id.upper())
            if match:
                android_devices.append({
                    'instance_id': instance_id,
                    'vid': match.group(1),
                    'pid': match.group(2),
                    'name': dev.get('FriendlyName', 'Unknown')
                })

    return android_devices

def find_aoa_devices():
    """AOAモードのデバイスを検出"""
    devices = get_usb_devices()
    aoa_devices = []

    for dev in devices:
        instance_id = dev.get('InstanceId', '')
        if f'VID_{AOA_VID}' in instance_id.upper() and 'PID_2D0' in instance_id.upper():
            aoa_devices.append(dev)

    return aoa_devices

# ===== Phase 3: WinUSBドライバインストール =====

def install_winusb(vid, pid):
    """wdi-simple.exeでWinUSBをインストール"""
    print(f"\n[WDI] WinUSBインストール (VID={vid}, PID={pid})...")

    if not WDI_SIMPLE_PATH or not os.path.exists(WDI_SIMPLE_PATH):
        print(f"    [ERROR] wdi-simple.exe が見つかりません")
        if WDI_SIMPLE_PATH:
            print(f"    指定パス: {WDI_SIMPLE_PATH}")
        print(f"    --wdi-path 引数または WDI_PATH 環境変数で指定してください")
        return False

    cmd = [
        WDI_SIMPLE_PATH,
        '--vid', f'0x{vid}',
        '--pid', f'0x{pid}',
        '--type', '0',  # WinUSB
        '--name', 'Android Device (AOA)',
    ]

    print(f"    実行: {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        print(f"    stdout: {result.stdout}")
        if result.stderr:
            print(f"    stderr: {result.stderr}")

        if result.returncode == 0:
            print("    WinUSBインストール成功")
            return True
        else:
            print(f"    失敗 (code={result.returncode})")
            return False
    except Exception as e:
        print(f"    エラー: {e}")
        return False

# ===== Phase 4: AOA切替 =====

def switch_to_aoa():
    """AOAモードに切替"""
    print("\n[AOA] AOAモード切替...")

    if not os.path.exists(AOA_SWITCH_PATH):
        print(f"    aoa_switch.exe が見つかりません: {AOA_SWITCH_PATH}")
        return False

    try:
        result = subprocess.run([AOA_SWITCH_PATH], capture_output=True, text=True, timeout=30)
        print(result.stdout)
        if result.stderr:
            print(f"    stderr: {result.stderr}")

        if "LIBUSB_ERROR_ACCESS" in (result.stderr + result.stdout):
            print("\n    [!] LIBUSB_ERROR_ACCESS - WinUSBドライバが必要")
            return False

        if "Switched" in result.stdout and "0 device" not in result.stdout:
            return True
        return result.returncode == 0
    except Exception as e:
        print(f"    エラー: {e}")
        return False

# ===== Phase 5: AOA許可ダイアログ自動承認 =====

def _parse_bounds(bounds_str):
    """'[x1,y1][x2,y2]' -> center (cx, cy)"""
    m = re.findall(r'\[(\d+),(\d+)\]', bounds_str)
    if len(m) < 2:
        return None, None
    x1, y1 = int(m[0][0]), int(m[0][1])
    x2, y2 = int(m[1][0]), int(m[1][1])
    return (x1 + x2) // 2, (y1 + y2) // 2

def _dump_ui(serial):
    """uiautomator dump → XML文字列を返す"""
    run_adb(['shell', 'uiautomator', 'dump', '/sdcard/ui_dump.xml'], serial)
    ok, out, _ = run_adb(['shell', 'cat', '/sdcard/ui_dump.xml'], serial)
    if ok and out.strip().startswith('<'):
        return out
    return None

def _find_node(root, **attrs):
    """XML treeからattribsに一致する最初のnodeを返す"""
    for node in root.iter('node'):
        match = all(
            kw.replace('_', '-') in node.attrib and val.lower() in node.attrib[kw.replace('_', '-')].lower()
            for kw, val in attrs.items()
        )
        if match:
            return node
    return None

def approve_aoa_dialog(serial, timeout=15):
    """
    AOAアクセサリ許可ダイアログを uiautomator で自動承認。

    Androidが表示するダイアログ例:
      「このUSBアクセサリへのアクセスを許可しますか?」
      □ このアプリに常に使用する  [キャンセル] [OK]

    手順:
      1. uiautomator dump でUI取得
      2. 「常に」チェックボックスを ON にする
      3. OK/許可 ボタンをタップ

    Returns:
      True  = ダイアログ検出&承認成功
      False = ダイアログが見つからない or タイムアウト
    """
    print(f"\n[DIALOG] AOA許可ダイアログ自動承認 (serial={serial or 'auto'}, timeout={timeout}s)...")

    # ダイアログ検出に使うキーワード (日本語/英語どちらにも対応)
    DIALOG_KEYWORDS = [
        "usb", "accessory", "アクセサリ", "allow", "permission", "許可"
    ]
    OK_KEYWORDS = ["ok", "allow", "許可", "はい"]
    ALWAYS_KEYWORDS = ["always", "常に"]

    deadline = time.time() + timeout
    while time.time() < deadline:
        xml_str = _dump_ui(serial)
        if not xml_str:
            time.sleep(1)
            continue

        try:
            root = ET.fromstring(xml_str)
        except ET.ParseError:
            time.sleep(1)
            continue

        # ダイアログが出ているか確認（テキストにキーワードが含まれるnode）
        all_texts = [
            (n.attrib.get('text', '') + n.attrib.get('content-desc', '')).lower()
            for n in root.iter('node')
        ]
        dialog_present = any(
            any(kw in t for kw in DIALOG_KEYWORDS)
            for t in all_texts
        )
        if not dialog_present:
            print(f"    ダイアログ未検出 ({int(deadline - time.time())}s残) - 待機中...")
            time.sleep(2)
            continue

        print("    ダイアログ検出!")

        # 「常に使用する」チェックボックス → まだチェックされていなければON
        always_node = None
        for node in root.iter('node'):
            txt = (node.attrib.get('text', '') + node.attrib.get('content-desc', '')).lower()
            if any(kw in txt for kw in ALWAYS_KEYWORDS):
                always_node = node
                break

        if always_node is not None:
            checked = always_node.attrib.get('checked', 'false').lower()
            if checked != 'true':
                cx, cy = _parse_bounds(always_node.attrib.get('bounds', ''))
                if cx is not None:
                    print(f"    「常に使用」チェックボックス タップ ({cx},{cy})")
                    run_adb(['shell', 'input', 'tap', str(cx), str(cy)], serial)
                    time.sleep(0.5)
            else:
                print("    「常に使用」既にチェック済み")
        else:
            print("    「常に使用」チェックボックス: 見つからずスキップ")

        # OK/許可 ボタンをタップ
        ok_node = None
        for node in root.iter('node'):
            txt = (node.attrib.get('text', '') + node.attrib.get('content-desc', '')).lower()
            cls = node.attrib.get('class', '')
            clickable = node.attrib.get('clickable', 'false').lower() == 'true'
            if clickable and any(kw in txt for kw in OK_KEYWORDS):
                ok_node = node
                break

        if ok_node is not None:
            cx, cy = _parse_bounds(ok_node.attrib.get('bounds', ''))
            if cx is not None:
                print(f"    OK/許可 ボタン タップ ({cx},{cy})")
                run_adb(['shell', 'input', 'tap', str(cx), str(cy)], serial)
                time.sleep(1)
                print("    承認完了!")
                return True
            else:
                print("    [WARN] OKボタンのboundsが取得できません")
        else:
            # フォールバック: Enter/Spaceキーで承認
            print("    [WARN] OKボタン特定失敗 → Enterキーで承認試行")
            run_adb(['shell', 'input', 'keyevent', '66'], serial)  # KEYCODE_ENTER
            time.sleep(1)
            return True

    print(f"    [WARN] {timeout}s以内にダイアログが検出されませんでした")
    return False


def approve_aoa_dialog_all(timeout=15):
    """全ADBデバイスに対してAOA許可ダイアログ承認を試みる"""
    devices = get_devices()
    if not devices:
        print("    ADBデバイスなし")
        return

    for serial, model in devices:
        print(f"\n  [{model} / {serial}]")
        approve_aoa_dialog(serial, timeout=timeout)


# ===== Main =====

def main():
    global WDI_SIMPLE_PATH
    parser = argparse.ArgumentParser(description='完全自動AOAセットアップ')
    parser.add_argument('--wdi-path', default=WDI_SIMPLE_PATH,
                        help='wdi-simple.exe のパス (環境変数 WDI_PATH でも指定可)')
    parser.add_argument('--dialog-only', action='store_true',
                        help='AOA許可ダイアログ承認のみ実行（デバイス接続済みの場合）')
    parser.add_argument('--dialog-timeout', type=int, default=15,
                        help='ダイアログ待機タイムアウト秒数 (default: 15)')
    args = parser.parse_args()

    if args.wdi_path:
        WDI_SIMPLE_PATH = args.wdi_path

    print("=" * 60)
    print("完全自動AOAセットアップ (wdi-simple版)")
    print("=" * 60)
    print(f"  MIRAGE_HOME: {MIRAGE_HOME}")
    print(f"  AOA_SWITCH:  {AOA_SWITCH_PATH}")
    print(f"  WDI_SIMPLE:  {WDI_SIMPLE_PATH or '(未指定)'}")
    print(f"  ADB:         {ADB}")

    # --dialog-only: ダイアログ承認のみ（管理者不要）
    if args.dialog_only:
        print("\n[MODE] ダイアログ承認のみ")
        approve_aoa_dialog_all(timeout=args.dialog_timeout)
        return

    # 管理者チェック
    if not is_admin():
        run_as_admin()
        return

    print("[OK] 管理者権限で実行中\n")

    # Phase 1: Android権限
    print("=" * 60)
    print("Phase 1: Android権限設定")
    print("=" * 60)

    devices = get_devices()
    print(f"ADBデバイス: {len(devices)}台\n")

    for serial, model in devices:
        print(f"[{model}] {serial}")
        setup_android_permissions(serial)
        print()

    # Phase 2: USBデバイス確認
    print("=" * 60)
    print("Phase 2: USBデバイス確認")
    print("=" * 60)

    android_usb = find_android_usb_devices()
    aoa_usb = find_aoa_devices()

    print(f"\nAndroidデバイス (非AOA): {len(android_usb)}台")
    for dev in android_usb:
        print(f"  - VID={dev['vid']} PID={dev['pid']} - {dev['name']}")

    print(f"\nAOAデバイス: {len(aoa_usb)}台")
    for dev in aoa_usb:
        print(f"  - {dev.get('InstanceId', 'Unknown')}")

    # Phase 3: WinUSBインストール & AOA切替
    print("\n" + "=" * 60)
    print("Phase 3: WinUSBインストール & AOA切替")
    print("=" * 60)

    if android_usb:
        for dev in android_usb:
            vid = dev['vid']
            pid = dev['pid']
            print(f"\n--- {dev['name']} ---")
            install_winusb(vid, pid)

        time.sleep(2)
        switch_to_aoa()
    else:
        if not aoa_usb:
            print("\n[!] Androidデバイスが見つかりません")
            print("    USBを確認してください")
        else:
            print("\n[OK] 既にAOAモードのデバイスがあります")

    # Phase 4: AOA許可ダイアログ自動承認
    # AOA切替直後にAndroidが「このUSBアクセサリへのアクセスを許可しますか?」を表示する。
    # uiautomatorで「常に使用」チェック後にOKをタップ。
    print("\n" + "=" * 60)
    print("Phase 4: AOA許可ダイアログ自動承認")
    print("=" * 60)
    approve_aoa_dialog_all(timeout=args.dialog_timeout)

    # Phase 5: 結果確認
    print("\n" + "=" * 60)
    print("Phase 5: 最終確認")
    print("=" * 60)

    time.sleep(3)

    aoa_devices = find_aoa_devices()
    print(f"\nAOAデバイス: {len(aoa_devices)}台")
    for dev in aoa_devices:
        print(f"  - {dev.get('InstanceId', 'Unknown')}")

    adb_devices = get_devices()
    print(f"\nADBデバイス: {len(adb_devices)}台")
    for serial, model in adb_devices:
        print(f"  - {model} ({serial})")

    print("\n" + "=" * 60)
    if aoa_devices:
        print("SUCCESS! AOAデバイス検出")
        print("\n次のステップ:")
        print("  mirage_gui.exe を起動してください")
    else:
        print("AOAデバイスなし")
        print("\n考えられる原因:")
        print("  1. WinUSBドライバが正しくインストールされていない")
        print("  2. USBを抜き差しする必要がある")
        print("  3. AOAダイアログを手動で承認してください")
        print("\n手動で試す場合:")
        print("  1. USBケーブルを抜く")
        print("  2. 3秒待つ")
        print("  3. USBケーブルを差し直す")
        print("  4. ダイアログが出たら「常に使用」にチェックして「OK」")
    print("=" * 60)

    input("\nPress Enter to exit...")

if __name__ == "__main__":
    main()
