#!/usr/bin/env python3
"""
Android デバイス用 WinUSB 自動インストーラー
MediaTek (VID=0E8D) などのデバイスに WinUSB をインストール

使用方法:
  python install_android_winusb.py

管理者権限が必要です。
"""

import subprocess
import sys
import os
import ctypes
from pathlib import Path


def is_admin():
    """管理者権限を確認"""
    try:
        return ctypes.windll.shell32.IsUserAnAdmin() != 0
    except:
        return False


def run_as_admin():
    """管理者権限で再起動"""
    script = sys.argv[0]
    params = " ".join(sys.argv[1:])

    print("[INFO] 管理者権限で再起動します...")
    ctypes.windll.shell32.ShellExecuteW(
        None, "runas", sys.executable, f'"{script}" {params}', None, 1
    )
    sys.exit(0)


def find_wdi_simple():
    """wdi-simple.exe を探す"""
    script_dir = Path(__file__).parent

    candidates = [
        script_dir / "driver_installer" / "tools" / "wdi" / "wdi-simple.exe",
        script_dir / "wdi-simple.exe",
        Path(os.environ.get("WDI_PATH", "")) / "wdi-simple.exe",
    ]

    for path in candidates:
        if path.exists():
            return str(path)

    return None


def get_android_devices():
    """
    接続されている Android デバイスの VID/PID を取得
    PowerShell で USB デバイスを列挙
    """
    ps_script = '''
$devices = Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like "USB\\VID_*" -and $_.Status -eq "OK"
}

$result = @()

foreach ($dev in $devices) {
    $instanceId = $dev.InstanceId
    if ($instanceId -match "VID_([0-9A-F]{4})&PID_([0-9A-F]{4})") {
        $vid = $Matches[1]
        $pid = $Matches[2]

        # Android デバイスの VID 一覧
        $androidVids = @("18D1", "04E8", "22B8", "2717", "2A70", "0E8D", "1782", "1F3A", "2207", "0BB4", "1004", "0FCE")

        if ($androidVids -contains $vid) {
            # Service を確認
            $service = (Get-PnpDeviceProperty -InstanceId $instanceId -KeyName "DEVPKEY_Device_Service" -ErrorAction SilentlyContinue).Data

            $result += @{
                vid = $vid
                pid = $pid
                instanceId = $instanceId
                service = $service
                name = $dev.FriendlyName
            }
        }
    }
}

$result | ConvertTo-Json
'''

    try:
        result = subprocess.run(
            ["powershell", "-NoProfile", "-Command", ps_script],
            capture_output=True, text=True, timeout=30
        )

        if result.returncode == 0 and result.stdout.strip():
            import json
            devices = json.loads(result.stdout)

            # 単一デバイスの場合はリスト化
            if isinstance(devices, dict):
                devices = [devices]

            return devices
    except Exception as e:
        print(f"[ERROR] デバイス検出失敗: {e}")

    return []


def install_winusb(vid, pid, name, wdi_path):
    """
    wdi-simple.exe で WinUSB をインストール
    """
    print(f"[INFO] WinUSB をインストール: VID={vid} PID={pid} ({name})")

    try:
        cmd = [
            wdi_path,
            "--vid", f"0x{vid}",
            "--pid", f"0x{pid}",
            "--type", "0",  # WinUSB
            "--name", name or "Android Device",
            "--dest", os.path.join(os.environ.get("TEMP", "."), f"wdi_{vid}_{pid}")
        ]

        print(f"[CMD] {' '.join(cmd)}")

        result = subprocess.run(
            cmd,
            capture_output=True, text=True, timeout=60
        )

        if result.returncode == 0:
            print(f"[OK] WinUSB インストール成功: VID={vid} PID={pid}")
            return True
        else:
            print(f"[ERROR] インストール失敗: {result.stderr}")
            return False

    except Exception as e:
        print(f"[ERROR] 例外: {e}")
        return False


def main():
    print()
    print("=" * 60)
    print("  Android WinUSB 自動インストーラー")
    print("=" * 60)
    print()

    # 管理者権限確認
    if not is_admin():
        run_as_admin()
        return

    print("[OK] 管理者権限で実行中")
    print()

    # wdi-simple.exe を探す
    wdi_path = find_wdi_simple()
    if not wdi_path:
        print("[ERROR] wdi-simple.exe が見つかりません")
        print("        driver_installer/tools/wdi/ に配置してください")
        input("\n[Press Enter to exit]")
        return

    print(f"[OK] wdi-simple: {wdi_path}")
    print()

    # Android デバイス検出
    print("[INFO] Android デバイスを検出中...")
    devices = get_android_devices()

    if not devices:
        print("[WARN] Android デバイスが見つかりません")
        print("       USB ケーブルで接続してください")
        input("\n[Press Enter to exit]")
        return

    print(f"[OK] {len(devices)} 台のデバイスを検出")
    print()

    # WinUSB が必要なデバイスをフィルタ
    needs_install = []
    for dev in devices:
        service = dev.get("service", "")
        if service != "WinUSB" and service != "WinUsb":
            needs_install.append(dev)
            print(f"  - VID={dev['vid']} PID={dev['pid']}: Service={service or 'None'} -> WinUSB が必要")
        else:
            print(f"  - VID={dev['vid']} PID={dev['pid']}: WinUSB 済み")

    print()

    if not needs_install:
        print("[OK] すべてのデバイスに WinUSB がインストール済みです")
        input("\n[Press Enter to exit]")
        return

    # インストール
    print(f"[INFO] {len(needs_install)} 台のデバイスに WinUSB をインストールします...")
    print()

    success = 0
    failed = 0

    for dev in needs_install:
        if install_winusb(dev["vid"], dev["pid"], dev.get("name"), wdi_path):
            success += 1
        else:
            failed += 1

    print()
    print("=" * 60)
    print(f"  完了: 成功={success} 失敗={failed}")
    print("=" * 60)

    if success > 0:
        print()
        print("[INFO] USB デバイスを再接続してください")

    input("\n[Press Enter to exit]")


if __name__ == "__main__":
    main()
