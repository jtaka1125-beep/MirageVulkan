#!/usr/bin/env python3
"""
device_backup.py - Android端末一括バックアップスクリプト

接続中の全端末から以下を自動保存:
  C:\MirageWork\device_backup\{model}_{serial}\{YYYYMMDD_HHMMSS}\
    - device_info.txt        : getprop全量 + 解像度 + OS情報
    - installed_packages.txt : インストール済みAPK一覧
    - settings_system.txt    : system設定ダンプ
    - settings_secure.txt    : secure設定ダンプ
    - settings_global.txt    : global設定ダンプ
    - wifi_config.txt        : WiFi接続情報
    - bluetooth_info.txt     : BT MACアドレス・ペアリング情報
    - developer_options.txt  : 開発者オプション関連設定
    - sdcard_xml/            : /sdcard/*.xml ファイル群
    - mirage_apps.txt        : Mirageアプリ版数情報
    - backup.ab              : adb backup (対応端末のみ)

Usage:
    python device_backup.py              # 全端末バックアップ
    python device_backup.py --serial XX  # 特定端末のみ
    python device_backup.py --no-ab      # adb backup をスキップ
"""

import subprocess
import os
import sys
import datetime
import argparse
import re

BACKUP_ROOT = r"C:\MirageWork\device_backup"


def run_adb(serial, *args, timeout=30):
    """ADBコマンドを実行して出力を返す"""
    cmd = ["adb", "-s", serial] + list(args)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except subprocess.TimeoutExpired:
        return "[TIMEOUT]"
    except Exception as e:
        return f"[ERROR: {e}]"


def shell(serial, command, timeout=30):
    """adb shell コマンド実行"""
    return run_adb(serial, "shell", command, timeout=timeout)


def get_connected_devices():
    """接続中のデバイスシリアル一覧を取得"""
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=10)
    devices = []
    for line in r.stdout.strip().split("\n")[1:]:
        parts = line.strip().split("\t")
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def get_device_label(serial):
    """デバイスのラベル (model_serial) を生成"""
    model = shell(serial, "getprop ro.product.model").replace(" ", "_")
    # WiFi接続の場合はIPをラベルに使用
    if ":" in serial:
        short_id = serial.replace(":", "_").replace(".", "_")
    else:
        short_id = serial
    return f"{model}_{short_id}" if model and model != "[TIMEOUT]" else short_id


def backup_device(serial, backup_dir, skip_ab=False):
    """1台のデバイスをバックアップ"""
    os.makedirs(backup_dir, exist_ok=True)
    results = {}

    # 1. デバイス情報 (getprop全量 + 解像度 + OS)
    print(f"  [1/9] デバイス情報...")
    info = []
    info.append("=== getprop (full) ===")
    info.append(shell(serial, "getprop"))
    info.append("\n=== Screen Resolution ===")
    info.append(shell(serial, "wm size"))
    info.append(shell(serial, "wm density"))
    info.append("\n=== OS Info ===")
    info.append(f"Android: {shell(serial, 'getprop ro.build.version.release')}")
    info.append(f"SDK: {shell(serial, 'getprop ro.build.version.sdk')}")
    info.append(f"Build: {shell(serial, 'getprop ro.build.display.id')}")
    info.append(f"Security Patch: {shell(serial, 'getprop ro.build.version.security_patch')}")
    with open(os.path.join(backup_dir, "device_info.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(info))
    results["device_info"] = "OK"

    # 2. インストール済みAPK一覧
    print(f"  [2/9] パッケージ一覧...")
    pkgs = shell(serial, "pm list packages -f")
    with open(os.path.join(backup_dir, "installed_packages.txt"), "w", encoding="utf-8") as f:
        f.write(pkgs)
    pkg_count = len([l for l in pkgs.split("\n") if l.strip()])
    results["packages"] = f"OK ({pkg_count} packages)"

    # 3. Settings ダンプ (system / secure / global)
    print(f"  [3/9] Settings ダンプ...")
    for ns in ["system", "secure", "global"]:
        data = shell(serial, f"settings list {ns}")
        with open(os.path.join(backup_dir, f"settings_{ns}.txt"), "w", encoding="utf-8") as f:
            f.write(data)
    results["settings"] = "OK (system/secure/global)"

    # 4. WiFi設定
    print(f"  [4/9] WiFi設定...")
    wifi = []
    wifi.append("=== WiFi Info ===")
    wifi.append(shell(serial, "dumpsys wifi | grep -E 'mWifiInfo|SSID|mNetworkInfo'"))
    wifi.append("\n=== Saved Networks ===")
    wifi.append(shell(serial, "cmd wifi list-networks 2>/dev/null || echo 'N/A'"))
    with open(os.path.join(backup_dir, "wifi_config.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(wifi))
    results["wifi"] = "OK"

    # 5. Bluetooth情報
    print(f"  [5/9] Bluetooth情報...")
    bt = []
    bt.append("=== BT Address ===")
    bt.append(shell(serial, "settings get secure bluetooth_address"))
    bt.append("\n=== BT Name ===")
    bt.append(shell(serial, "settings get secure bluetooth_name"))
    bt.append("\n=== Paired Devices ===")
    bt.append(shell(serial, "dumpsys bluetooth_manager | grep -A2 'Bonded devices'"))
    with open(os.path.join(backup_dir, "bluetooth_info.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(bt))
    results["bluetooth"] = "OK"

    # 6. 開発者オプション
    print(f"  [6/9] 開発者オプション...")
    dev = []
    dev_keys = [
        "development_settings_enabled", "adb_enabled",
        "debug_app", "wait_for_debugger", "stay_on_while_plugged_in",
        "show_touches", "pointer_location",
        "transition_animation_scale", "window_animation_scale",
        "animator_duration_scale", "force_gpu_rendering",
    ]
    for key in dev_keys:
        val = shell(serial, f"settings get global {key}")
        dev.append(f"{key} = {val}")
    dev.append("\n=== USB Config ===")
    dev.append(f"usb_config = {shell(serial, 'getprop persist.sys.usb.config')}")
    dev.append(f"adb_wifi_enabled = {shell(serial, 'getprop service.adb.tcp.port')}")
    with open(os.path.join(backup_dir, "developer_options.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(dev))
    results["developer"] = "OK"

    # 7. /sdcard/ の設定系XMLファイル
    print(f"  [7/9] /sdcard/ XMLファイル...")
    xml_dir = os.path.join(backup_dir, "sdcard_xml")
    os.makedirs(xml_dir, exist_ok=True)
    xml_list = shell(serial, "ls /sdcard/*.xml 2>/dev/null")
    xml_count = 0
    if xml_list and "No such file" not in xml_list:
        for xml_file in xml_list.strip().split("\n"):
            xml_file = xml_file.strip()
            if xml_file:
                fname = os.path.basename(xml_file)
                run_adb(serial, "pull", xml_file, os.path.join(xml_dir, fname), timeout=15)
                xml_count += 1
    results["sdcard_xml"] = f"OK ({xml_count} files)"

    # 8. Mirageアプリ情報
    print(f"  [8/9] Mirageアプリ情報...")
    mirage = []
    for pkg in ["com.mirage.android", "com.mirage.accessory", "com.mirage.capture"]:
        ver = shell(serial, f"dumpsys package {pkg} | grep versionName")
        mirage.append(f"{pkg}: {ver.strip()}")
    with open(os.path.join(backup_dir, "mirage_apps.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(mirage))
    results["mirage_apps"] = "OK"

    # 9. adb backup (オプション)
    if not skip_ab:
        print(f"  [9/9] adb backup (手動承認が必要な場合あり)...")
        ab_path = os.path.join(backup_dir, "backup.ab")
        try:
            subprocess.run(
                ["adb", "-s", serial, "backup", "-apk", "-shared", "-all", "-f", ab_path],
                timeout=120
            )
            if os.path.exists(ab_path) and os.path.getsize(ab_path) > 100:
                results["adb_backup"] = f"OK ({os.path.getsize(ab_path)} bytes)"
            else:
                results["adb_backup"] = "SKIPPED (empty/unsupported)"
        except subprocess.TimeoutExpired:
            results["adb_backup"] = "TIMEOUT (120s)"
        except Exception as e:
            results["adb_backup"] = f"ERROR: {e}"
    else:
        results["adb_backup"] = "SKIPPED (--no-ab)"

    return results


def main():
    parser = argparse.ArgumentParser(description="Android端末一括バックアップ")
    parser.add_argument("--serial", help="特定端末のシリアルのみ")
    parser.add_argument("--no-ab", action="store_true", help="adb backupをスキップ")
    args = parser.parse_args()

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")

    # デバイス取得
    if args.serial:
        devices = [args.serial]
    else:
        devices = get_connected_devices()

    if not devices:
        print("接続中のデバイスがありません")
        sys.exit(1)

    # WiFi ADB接続はUSB接続とペアになることが多いので、USB接続のみに絞る
    usb_devices = [d for d in devices if ":" not in d]
    wifi_devices = [d for d in devices if ":" in d]

    # USB接続がある端末はUSBを優先、WiFiのみの端末はWiFiを使う
    target_devices = usb_devices if usb_devices else wifi_devices
    if not args.serial:
        # 重複排除: 同一端末のUSB/WiFi両方がある場合USB優先
        print(f"検出デバイス: USB={len(usb_devices)}, WiFi={len(wifi_devices)}")
        print(f"バックアップ対象: {len(target_devices)} 台 (USB優先)")

    print(f"{'='*60}")
    print(f"MirageSystem Device Backup - {timestamp}")
    print(f"{'='*60}")

    all_results = {}
    for serial in target_devices:
        label = get_device_label(serial)
        backup_dir = os.path.join(BACKUP_ROOT, label, timestamp)
        print(f"\n[{serial}] → {backup_dir}")

        results = backup_device(serial, backup_dir, skip_ab=args.no_ab)
        all_results[serial] = results

        print(f"  --- 完了 ---")
        for key, val in results.items():
            print(f"    {key}: {val}")

    # サマリー
    print(f"\n{'='*60}")
    print(f"バックアップ完了: {len(target_devices)} 台")
    print(f"保存先: {BACKUP_ROOT}")
    print(f"{'='*60}")


if __name__ == "__main__":
    main()
