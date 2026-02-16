#!/usr/bin/env python3
"""
bump_version.py - APK versionCode 自動インクリメント

Usage:
    python bump_version.py                  # 全モジュール versionCode +1
    python bump_version.py --module app     # appのみ
    python bump_version.py --set 10         # 特定の値に設定
    python bump_version.py --show           # 現在の値を表示するだけ
"""

import os
import re
import sys
import argparse

ANDROID_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android")

MODULES = {
    "app": os.path.join(ANDROID_DIR, "app", "build.gradle.kts"),
    "accessory": os.path.join(ANDROID_DIR, "accessory", "build.gradle.kts"),
    "capture": os.path.join(ANDROID_DIR, "capture", "build.gradle.kts"),
}


def read_version(gradle_path):
    """build.gradle.ktsからversionCode/versionNameを読み取り"""
    with open(gradle_path, "r", encoding="utf-8") as f:
        content = f.read()

    code_match = re.search(r'versionCode\s*=\s*(\d+)', content)
    name_match = re.search(r'versionName\s*=\s*"([^"]+)"', content)

    code = int(code_match.group(1)) if code_match else None
    name = name_match.group(1) if name_match else None

    return code, name


def write_version_code(gradle_path, new_code):
    """versionCodeを書き換え"""
    with open(gradle_path, "r", encoding="utf-8") as f:
        content = f.read()

    new_content = re.sub(
        r'(versionCode\s*=\s*)\d+',
        f'\\g<1>{new_code}',
        content
    )

    with open(gradle_path, "w", encoding="utf-8") as f:
        f.write(new_content)

    return new_content != content


def main():
    parser = argparse.ArgumentParser(description="APK versionCode管理")
    parser.add_argument("--module", choices=list(MODULES.keys()), nargs="+",
                        help="特定モジュールのみ")
    parser.add_argument("--set", type=int, help="versionCodeを指定値に設定")
    parser.add_argument("--show", action="store_true", help="表示のみ")
    args = parser.parse_args()

    targets = args.module or list(MODULES.keys())

    print("=== APK Version Manager ===\n")

    # 現在の値を表示
    current = {}
    for name in targets:
        path = MODULES[name]
        if not os.path.exists(path):
            print(f"  [{name}] build.gradle.kts not found")
            continue

        code, vname = read_version(path)
        current[name] = (code, vname, path)
        print(f"  {name:<12} versionCode={code:<5} versionName={vname}")

    if args.show:
        return

    print()

    # インクリメント or 設定
    for name in targets:
        if name not in current:
            continue

        code, vname, path = current[name]

        if args.set is not None:
            new_code = args.set
        else:
            new_code = code + 1

        if new_code == code:
            print(f"  [{name}] 変更なし (versionCode={code})")
            continue

        changed = write_version_code(path, new_code)
        if changed:
            print(f"  [{name}] versionCode: {code} → {new_code}")
        else:
            print(f"  [{name}] 書き換え失敗")

    # 変更後の確認
    print("\n--- 更新後 ---")
    for name in targets:
        path = MODULES[name]
        if os.path.exists(path):
            code, vname = read_version(path)
            print(f"  {name:<12} versionCode={code:<5} versionName={vname}")


if __name__ == "__main__":
    main()
