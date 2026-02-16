#!/usr/bin/env python3
"""
screenshot_compare.py - 全端末の画面スクショを同時取得・並べて保存

Usage:
    python screenshot_compare.py                # 全端末スクショ取得
    python screenshot_compare.py --grid         # 1枚の比較画像に合成
    python screenshot_compare.py --open         # 取得後にフォルダを開く
"""

import subprocess
import os
import sys
import argparse
import datetime
import time

SCREENSHOT_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "screenshots")


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


def take_screenshot(serial, model, output_dir):
    """1台のスクショを取得"""
    remote_path = "/sdcard/mirage_screenshot_tmp.png"
    local_path = os.path.join(output_dir, f"{model}_{serial}.png")

    # 端末上でスクショ取得
    r = subprocess.run(["adb", "-s", serial, "shell", "screencap", "-p", remote_path],
                       capture_output=True, text=True, timeout=15)
    if r.returncode != 0:
        print(f"  [{model}] screencap失敗")
        return None

    # PCにpull
    r = subprocess.run(["adb", "-s", serial, "pull", remote_path, local_path],
                       capture_output=True, text=True, timeout=15)
    if r.returncode != 0:
        print(f"  [{model}] pull失敗")
        return None

    # 端末上の一時ファイル削除
    subprocess.run(["adb", "-s", serial, "shell", "rm", remote_path],
                   capture_output=True, timeout=5)

    size = os.path.getsize(local_path) / 1024
    print(f"  [{model}] {size:.0f}KB → {os.path.basename(local_path)}")
    return local_path


def create_grid(image_paths, output_path):
    """複数スクショを1枚のグリッド画像に合成 (PILが無い場合はスキップ)"""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        print("  [SKIP] Pillow未インストール (pip install Pillow でグリッド合成可能)")
        return False

    images = []
    for path in image_paths:
        if path and os.path.exists(path):
            img = Image.open(path)
            images.append((os.path.basename(path).replace(".png", ""), img))

    if not images:
        return False

    # 全画像を同一高さにリサイズ
    target_h = 800
    resized = []
    for label, img in images:
        ratio = target_h / img.height
        new_w = int(img.width * ratio)
        resized.append((label, img.resize((new_w, target_h), Image.LANCZOS)))

    # ラベル用スペース
    label_h = 40
    total_w = sum(img.width for _, img in resized) + (len(resized) - 1) * 10
    total_h = target_h + label_h

    grid = Image.new("RGB", (total_w, total_h), (30, 30, 30))
    draw = ImageDraw.Draw(grid)

    x = 0
    for label, img in resized:
        # ラベル
        draw.text((x + 10, 5), label, fill=(255, 255, 255))
        # 画像
        grid.paste(img, (x, label_h))
        x += img.width + 10

    grid.save(output_path)
    print(f"  [GRID] {output_path} ({total_w}x{total_h})")
    return True


def main():
    parser = argparse.ArgumentParser(description="端末スクショ比較")
    parser.add_argument("--grid", action="store_true", help="グリッド画像合成")
    parser.add_argument("--open", action="store_true", help="フォルダを開く")
    parser.add_argument("--serial", help="特定端末のみ")
    args = parser.parse_args()

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = os.path.join(SCREENSHOT_ROOT, timestamp)
    os.makedirs(output_dir, exist_ok=True)

    if args.serial:
        devices = [args.serial]
    else:
        devices = get_usb_devices()

    if not devices:
        print("接続中のUSBデバイスなし")
        sys.exit(1)

    print(f"=== Screenshot Compare ({timestamp}) ===")
    print(f"端末: {len(devices)} 台\n")

    paths = []
    for serial in devices:
        model = get_model(serial)
        path = take_screenshot(serial, model, output_dir)
        paths.append(path)

    # グリッド合成
    if args.grid:
        grid_path = os.path.join(output_dir, "comparison_grid.png")
        create_grid(paths, grid_path)

    print(f"\n保存先: {output_dir}")

    if args.open:
        os.startfile(output_dir)


if __name__ == "__main__":
    main()
