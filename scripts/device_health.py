#!/usr/bin/env python3
"""
device_health.py - 接続中全端末のヘルスチェック一覧表示

Usage:
    python device_health.py           # 全端末チェック
    python device_health.py --watch   # 10秒間隔で継続監視
"""

import subprocess
import argparse
import time
import sys


def shell(serial, cmd, timeout=5):
    try:
        r = subprocess.run(["adb", "-s", serial, "shell", cmd],
                           capture_output=True, text=True, timeout=timeout)
        return r.stdout.strip()
    except:
        return ""


def get_all_devices():
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=10)
    devices = []
    for line in r.stdout.strip().split("\n")[1:]:
        parts = line.strip().split("\t")
        if len(parts) >= 2:
            devices.append({"serial": parts[0], "state": parts[1]})
    return devices


def check_device(serial):
    """1台のデバイスを診断"""
    info = {}

    # 基本情報
    info["model"] = shell(serial, "getprop ro.product.model") or "?"
    info["android"] = shell(serial, "getprop ro.build.version.release") or "?"

    # 解像度
    wm = shell(serial, "wm size")
    info["resolution"] = wm.split(": ")[-1] if ": " in wm else "?"

    # 接続タイプ
    info["conn"] = "WiFi" if ":" in serial else "USB"

    # WiFi IP
    if info["conn"] == "USB":
        ip_raw = shell(serial, "ip route | grep wlan0 | grep src")
        if "src " in ip_raw:
            info["ip"] = ip_raw.split("src ")[-1].split()[0]
        else:
            info["ip"] = "-"
    else:
        info["ip"] = serial.split(":")[0]

    # WiFi ADB
    tcp_port = shell(serial, "getprop service.adb.tcp.port")
    info["wifi_adb"] = f":{tcp_port}" if tcp_port and tcp_port != "0" else "OFF"

    # Mirageアプリ状態
    apps = {}
    for pkg, label in [("com.mirage.android", "main"),
                        ("com.mirage.accessory", "accy"),
                        ("com.mirage.capture", "capt")]:
        # インストール確認
        installed = shell(serial, f"pm list packages {pkg}")
        if not installed:
            apps[label] = "N/A"
            continue

        # 実行中確認
        procs = shell(serial, f"ps -A | grep {pkg}")
        apps[label] = "RUN" if procs else "STOP"

    info["apps"] = apps

    # バッテリー
    batt = shell(serial, "dumpsys battery | grep level")
    info["battery"] = batt.split(": ")[-1] + "%" if ": " in batt else "?"

    # 画面状態
    screen = shell(serial, "dumpsys power | grep 'Display Power'")
    info["screen"] = "ON" if "ON" in screen else "OFF"

    # UDPポート待ち受け (映像ストリーム)
    udp = shell(serial, "ss -u -l -n | grep -c ':6[0-9]'")
    info["video_ports"] = udp if udp else "0"

    return info


def print_report(devices_info):
    """レポート表示"""
    # ヘッダー
    print(f"{'Serial':<22} {'Model':<10} {'Conn':<5} {'IP':<16} "
          f"{'Android':<8} {'Res':<12} {'main':<5} {'accy':<5} {'capt':<5} "
          f"{'Batt':<6} {'Scrn':<5}")
    print("-" * 120)

    for serial, info in devices_info.items():
        apps = info.get("apps", {})
        main_st = apps.get("main", "?")
        accy_st = apps.get("accy", "?")
        capt_st = apps.get("capt", "?")

        # 色分け用マーク
        def mark(s):
            if s == "RUN":
                return "* RUN"
            elif s == "STOP":
                return "  ---"
            else:
                return "  N/A"

        print(f"{serial:<22} {info.get('model', '?'):<10} {info.get('conn', '?'):<5} "
              f"{info.get('ip', '-'):<16} "
              f"{info.get('android', '?'):<8} {info.get('resolution', '?'):<12} "
              f"{mark(main_st):<5} {mark(accy_st):<5} {mark(capt_st):<5} "
              f"{info.get('battery', '?'):<6} {info.get('screen', '?'):<5}")


def run_check():
    """1回のチェック実行"""
    all_devices = get_all_devices()

    if not all_devices:
        print("接続中のデバイスなし")
        return

    print(f"\n=== MirageSystem Device Health === ({time.strftime('%H:%M:%S')})")
    print(f"検出: {len(all_devices)} 台\n")

    online = [d for d in all_devices if d["state"] == "device"]
    offline = [d for d in all_devices if d["state"] != "device"]

    # オンラインデバイスのチェック
    devices_info = {}
    for d in online:
        devices_info[d["serial"]] = check_device(d["serial"])

    print_report(devices_info)

    # オフラインデバイス
    if offline:
        print(f"\n[OFFLINE] ", end="")
        for d in offline:
            print(f"{d['serial']} ({d['state']})", end="  ")
        print()

    # 異常検出サマリー
    warnings = []
    for serial, info in devices_info.items():
        model = info.get("model", serial)
        apps = info.get("apps", {})
        if apps.get("main") == "STOP":
            warnings.append(f"{model}: MirageAndroid停止中")
        if apps.get("accy") == "STOP":
            warnings.append(f"{model}: MirageAccessory停止中")
        batt = info.get("battery", "?").replace("%", "")
        try:
            if int(batt) < 20:
                warnings.append(f"{model}: バッテリー残量低下 ({batt}%)")
        except:
            pass

    if warnings:
        print(f"\n[WARNING]")
        for w in warnings:
            print(f"  ! {w}")
    else:
        print(f"\n[OK] 全端末正常")


def main():
    parser = argparse.ArgumentParser(description="端末ヘルスチェック")
    parser.add_argument("--watch", action="store_true", help="継続監視 (10秒間隔)")
    parser.add_argument("--interval", type=int, default=10, help="監視間隔(秒)")
    args = parser.parse_args()

    if args.watch:
        try:
            while True:
                os.system("cls" if sys.platform == "win32" else "clear")
                run_check()
                time.sleep(args.interval)
        except KeyboardInterrupt:
            print("\n監視終了")
    else:
        run_check()


if __name__ == "__main__":
    import os
    main()
