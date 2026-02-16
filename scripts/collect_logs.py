#!/usr/bin/env python3
"""
collect_logs.py - 全端末のMirage関連logcatを収集・保存

Usage:
    python collect_logs.py                    # 最新バッファのスナップショット
    python collect_logs.py --live             # リアルタイムストリーム (Ctrl+C停止)
    python collect_logs.py --since 60         # 直近60秒分
    python collect_logs.py --tag VideoSender  # 追加タグフィルタ
    python collect_logs.py --all-tags         # Mirageタグ以外も含む全ログ
"""

import subprocess
import os
import sys
import argparse
import datetime
import time
import threading

LOG_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs")

# Mirage関連のlogcatタグ
MIRAGE_TAGS = [
    "MirageMain", "MirageCapture", "MirageAccessory",
    "CaptureService", "CaptureController", "ScreenCaptureService",
    "H264Encoder", "VideoSender", "UsbVideoSender", "UdpSender", "TcpVideoSender",
    "AccessoryIoService", "Protocol", "AOA",
    "TxService", "WatchdogService",
    "AudioCapture", "OpusEncoder",
    "BootReceiver", "MirageAccessibility",
    "SurfaceRepeater", "RtpH264Packetizer",
]


def get_usb_devices():
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=10)
    devices = []
    for line in r.stdout.strip().split("\n")[1:]:
        parts = line.strip().split("\t")
        if len(parts) >= 2 and parts[1] == "device" and ":" not in parts[0]:
            devices.append(parts[0])
    return devices


def get_model(serial):
    r = subprocess.run(["adb", "-s", serial, "shell", "getprop", "ro.product.model"],
                       capture_output=True, text=True, timeout=5)
    return r.stdout.strip().replace(" ", "_") or serial


def collect_snapshot(serial, model, extra_tags=None, all_tags=False, since_seconds=None):
    """logcatスナップショット取得"""
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    device_dir = os.path.join(LOG_ROOT, f"{model}_{serial}")
    os.makedirs(device_dir, exist_ok=True)
    log_file = os.path.join(device_dir, f"{timestamp}.txt")

    cmd = ["adb", "-s", serial, "logcat", "-d"]

    if since_seconds and since_seconds > 0:
        cmd.extend(["-t", f"{since_seconds}"])

    if not all_tags:
        # Mirageタグでgrepフィルタ
        tags = MIRAGE_TAGS.copy()
        if extra_tags:
            tags.extend(extra_tags)
        tag_pattern = "|".join(tags)

        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        lines = [l for l in r.stdout.split("\n") if any(t in l for t in tags)]
        output = "\n".join(lines)
    else:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        output = r.stdout

    with open(log_file, "w", encoding="utf-8") as f:
        f.write(f"# Device: {model} ({serial})\n")
        f.write(f"# Collected: {timestamp}\n")
        f.write(f"# Lines: {len(output.split(chr(10)))}\n")
        f.write(f"# {'All tags' if all_tags else 'Mirage tags only'}\n\n")
        f.write(output)

    line_count = len([l for l in output.split("\n") if l.strip()])
    return log_file, line_count


def stream_live(serial, model, extra_tags=None, all_tags=False):
    """リアルタイムlogcatストリーム"""
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    device_dir = os.path.join(LOG_ROOT, f"{model}_{serial}")
    os.makedirs(device_dir, exist_ok=True)
    log_file = os.path.join(device_dir, f"live_{timestamp}.txt")

    # logcat バッファクリア→新規ストリーム
    subprocess.run(["adb", "-s", serial, "logcat", "-c"], timeout=5)

    cmd = ["adb", "-s", serial, "logcat"]

    tags = MIRAGE_TAGS.copy()
    if extra_tags:
        tags.extend(extra_tags)

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    f = open(log_file, "w", encoding="utf-8")
    count = 0

    try:
        for line in proc.stdout:
            if all_tags or any(t in line for t in tags):
                sys.stdout.write(f"[{model}] {line}")
                f.write(line)
                count += 1
    except KeyboardInterrupt:
        pass
    finally:
        proc.kill()
        f.close()

    return log_file, count


def main():
    parser = argparse.ArgumentParser(description="Mirageログ収集")
    parser.add_argument("--live", action="store_true", help="リアルタイムストリーム")
    parser.add_argument("--since", type=int, default=0, help="直近N秒分")
    parser.add_argument("--tag", nargs="+", help="追加タグフィルタ")
    parser.add_argument("--all-tags", action="store_true", help="全タグ (フィルタなし)")
    parser.add_argument("--serial", help="特定端末のみ")
    args = parser.parse_args()

    if args.serial:
        devices = [args.serial]
    else:
        devices = get_usb_devices()

    if not devices:
        print("接続中のUSBデバイスなし")
        sys.exit(1)

    print(f"=== Mirage Log Collector ===")
    print(f"端末: {len(devices)} 台")
    print(f"モード: {'live' if args.live else 'snapshot'}")

    if args.live:
        # ライブモードは1台ずつ（複数台はスレッド化）
        if len(devices) == 1:
            serial = devices[0]
            model = get_model(serial)
            print(f"\n[{model}] リアルタイム監視開始 (Ctrl+C で停止)\n")
            log_file, count = stream_live(serial, model, args.tag, args.all_tags)
            print(f"\n保存: {log_file} ({count} lines)")
        else:
            print(f"\n全端末リアルタイム監視 (Ctrl+C で停止)\n")
            threads = []
            for serial in devices:
                model = get_model(serial)
                t = threading.Thread(target=stream_live,
                                     args=(serial, model, args.tag, args.all_tags),
                                     daemon=True)
                t.start()
                threads.append(t)
            try:
                while True:
                    time.sleep(1)
            except KeyboardInterrupt:
                print("\n監視終了")
    else:
        # スナップショットモード
        for serial in devices:
            model = get_model(serial)
            log_file, count = collect_snapshot(serial, model, args.tag, args.all_tags,
                                               args.since if args.since > 0 else None)
            print(f"  [{model}] {count} lines → {log_file}")

        print(f"\nログ保存先: {LOG_ROOT}")


if __name__ == "__main__":
    main()
