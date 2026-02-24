#!/usr/bin/env python3
"""
deploy_apk.py - APKビルド→全端末デプロイ→起動→権限承認 ワンクリック

Usage:
    python deploy_apk.py                        # 全モジュール ビルド+デプロイ
    python deploy_apk.py --module capture       # captureのみ
    python deploy_apk.py --module accessory     # accessoryのみ
    python deploy_apk.py --skip-build           # ビルドスキップ（既存APKをデプロイ）
    python deploy_apk.py --debug                # debugビルド
"""

import subprocess
import os
import sys
import argparse
import time
import glob

ANDROID_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android")

# NOTE: "app" (旧モノリス) は 2026-02-24 に :capture + :accessory に分割済み。
# android/app/ ソースは参照用に保持するが、settings.gradle.kts から除外済み。
MODULES = {
    "accessory": {
        "package": "com.mirage.accessory",
        "activity": "com.mirage.accessory.ui.AccessoryActivity",
        "apk_release": "accessory/build/outputs/apk/release/accessory-release.apk",
        "apk_debug": "accessory/build/outputs/apk/debug/accessory-debug.apk",
    },
    "capture": {
        "package": "com.mirage.capture",
        "activity": None,  # サービスのみ、Activity起動不要（ADB broadcastで制御）
        "apk_release": "capture/build/outputs/apk/release/capture-release.apk",
        "apk_debug": "capture/build/outputs/apk/debug/capture-debug.apk",
    },
}


def run(cmd, cwd=None, timeout=300):
    """コマンド実行"""
    print(f"    $ {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=timeout, shell=isinstance(cmd, str))
    if r.returncode != 0 and r.stderr:
        print(f"    [WARN] {r.stderr.strip()[:200]}")
    return r


def get_devices():
    """接続中デバイスシリアル一覧 (USB + WiFi ADB)"""
    r = subprocess.run(["adb", "devices"], capture_output=True, text=True, timeout=10)
    devices = []
    for line in r.stdout.strip().split("\n")[1:]:
        parts = line.strip().split("\t")
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])
    return devices


def get_usb_devices():
    """後方互換スタブ"""
    return get_devices()


def build_modules(modules, debug=False):
    """Gradleビルド"""
    build_type = "Debug" if debug else "Release"
    tasks = [f":{m}:assemble{build_type}" for m in modules]

    print(f"\n[BUILD] {', '.join(modules)} ({build_type})")
    gradlew = os.path.join(ANDROID_DIR, "gradlew.bat")

    cmd = [gradlew] + tasks
    r = run(cmd, cwd=ANDROID_DIR, timeout=600)

    if r.returncode != 0:
        print(f"  [FAIL] ビルド失敗")
        print(r.stdout[-500:] if r.stdout else "")
        print(r.stderr[-500:] if r.stderr else "")
        return False

    print(f"  [OK] ビルド成功")
    return True


def deploy_module(module_name, module_info, devices, debug=False):
    """1モジュールを全端末にデプロイ"""
    apk_key = "apk_debug" if debug else "apk_release"
    apk_path = os.path.join(ANDROID_DIR, module_info[apk_key])

    if not os.path.exists(apk_path):
        print(f"  [SKIP] APK not found: {apk_path}")
        return 0

    apk_size = os.path.getsize(apk_path) / 1024 / 1024
    print(f"\n[DEPLOY] {module_name} ({apk_size:.1f}MB) → {len(devices)} 台")

    success = 0
    for serial in devices:
        model = subprocess.run(
            ["adb", "-s", serial, "shell", "getprop", "ro.product.model"],
            capture_output=True, text=True, timeout=5
        ).stdout.strip()

        print(f"  [{model} / {serial}]")

        # インストール (-r で上書き, -g で全権限付与)
        r = run(["adb", "-s", serial, "install", "-r", "-g", apk_path], timeout=120)
        if r.returncode != 0:
            print(f"    [FAIL] インストール失敗")
            continue
        print(f"    [OK] インストール完了")

        # アプリ起動
        if module_info["activity"]:
            time.sleep(1)
            run(["adb", "-s", serial, "shell", "am", "start", "-n",
                 f"{module_info['package']}/{module_info['activity']}"], timeout=10)
            print(f"    [OK] 起動")

        success += 1

    return success


def grant_permissions(devices):
    """MediaProjection等の権限自動承認"""
    print(f"\n[PERMISSIONS] 権限設定")
    for serial in devices:
        model = subprocess.run(
            ["adb", "-s", serial, "shell", "getprop", "ro.product.model"],
            capture_output=True, text=True, timeout=5
        ).stdout.strip()

        # MirageAccessory の AccessibilityService 有効化
        acc_svc = "com.mirage.accessory/com.mirage.accessory.access.MirageAccessibilityService"
        run(["adb", "-s", serial, "shell", "settings", "put", "secure",
             "enabled_accessibility_services", acc_svc], timeout=5)

        # バッテリー最適化除外 (capture + accessory)
        for pkg in ["com.mirage.capture", "com.mirage.accessory"]:
            run(["adb", "-s", serial, "shell", "dumpsys", "deviceidle", "whitelist",
                 f"+{pkg}"], timeout=5)

        print(f"  [{model}] AccessibilityService + バッテリー最適化除外 設定済み")


def main():
    parser = argparse.ArgumentParser(description="MirageAPK ワンクリックデプロイ")
    parser.add_argument("--module", choices=list(MODULES.keys()), nargs="+",
                        help="特定モジュールのみ (default: all)")
    parser.add_argument("--skip-build", action="store_true", help="ビルドスキップ")
    parser.add_argument("--debug", action="store_true", help="debugビルド")
    parser.add_argument("--no-start", action="store_true", help="アプリ起動しない")
    parser.add_argument("--no-permissions", action="store_true", help="権限設定スキップ")
    args = parser.parse_args()

    target_modules = args.module or list(MODULES.keys())
    devices = get_devices()

    if not devices:
        print("デバイスが見つかりません (USB/WiFi ADB 未接続)")
        sys.exit(1)

    print("=" * 60)
    print(f"MirageAPK Deploy")
    print(f"  モジュール: {', '.join(target_modules)}")
    print(f"  端末: {len(devices)} 台 ({', '.join(devices)})")
    print(f"  ビルド: {'skip' if args.skip_build else ('debug' if args.debug else 'release')}")
    print("=" * 60)

    # バックアップ (デプロイ前に自動バックアップ)
    backup_script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "device_backup.py")
    if os.path.exists(backup_script):
        print("\n[BACKUP] デプロイ前バックアップ...")
        run([sys.executable, backup_script, "--no-ab"], timeout=120)

    # ビルド
    if not args.skip_build:
        if not build_modules(target_modules, debug=args.debug):
            sys.exit(1)

    # デプロイ
    total_success = 0
    for name in target_modules:
        info = MODULES[name].copy()
        if args.no_start:
            info["activity"] = None
        total_success += deploy_module(name, info, devices, debug=args.debug)

    # 権限設定
    if not args.no_permissions:
        grant_permissions(devices)

    # サマリー
    print(f"\n{'=' * 60}")
    print(f"デプロイ完了: {total_success}/{len(target_modules) * len(devices)}")
    print("=" * 60)


if __name__ == "__main__":
    main()
