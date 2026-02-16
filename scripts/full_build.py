#!/usr/bin/env python3
"""
full_build.py - C++ビルド + APKビルド + テスト実行 ワンコマンド

Usage:
    python full_build.py              # フルビルド+テスト
    python full_build.py --cpp-only   # C++のみ
    python full_build.py --apk-only   # APKのみ
    python full_build.py --no-test    # テストスキップ
    python full_build.py --clean      # クリーンビルド
"""

import subprocess
import os
import sys
import time
import argparse

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD_DIR = os.path.join(ROOT, "build")
ANDROID_DIR = os.path.join(ROOT, "android")


def run(cmd, cwd=None, timeout=600):
    print(f"  $ {cmd if isinstance(cmd, str) else ' '.join(cmd)}")
    start = time.time()
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd,
                       timeout=timeout, shell=isinstance(cmd, str))
    elapsed = time.time() - start
    return r, elapsed


def build_cpp(clean=False):
    """CMake C++ビルド"""
    print("\n" + "=" * 60)
    print("[1/3] C++ ビルド (Release)")
    print("=" * 60)

    if clean and os.path.exists(BUILD_DIR):
        print("  クリーンビルド: buildディレクトリ再生成")
        import shutil
        shutil.rmtree(BUILD_DIR, ignore_errors=True)

    os.makedirs(BUILD_DIR, exist_ok=True)

    # CMake configure (必要な場合のみ)
    cache = os.path.join(BUILD_DIR, "CMakeCache.txt")
    if not os.path.exists(cache):
        print("  [CMake Configure]")
        r, t = run(["cmake", "..", "-G", "Visual Studio 17 2022", "-A", "x64"],
                    cwd=BUILD_DIR)
        if r.returncode != 0:
            print(f"  [FAIL] CMake configure ({t:.1f}s)")
            print(r.stderr[-500:])
            return False
        print(f"  [OK] Configure ({t:.1f}s)")

    # Build
    print("  [CMake Build]")
    r, t = run(["cmake", "--build", ".", "--config", "Release"], cwd=BUILD_DIR)
    if r.returncode != 0:
        print(f"  [FAIL] Build ({t:.1f}s)")
        # エラー行だけ抽出
        for line in r.stdout.split("\n"):
            if "error" in line.lower():
                print(f"    {line.strip()}")
        return False
    print(f"  [OK] Build ({t:.1f}s)")
    return True


def run_tests():
    """CTestテスト実行"""
    print("\n" + "=" * 60)
    print("[2/3] テスト実行")
    print("=" * 60)

    r, t = run(["ctest", "--output-on-failure", "-C", "Release"], cwd=BUILD_DIR)

    # 結果パース
    passed = failed = 0
    for line in r.stdout.split("\n"):
        if "tests passed" in line:
            print(f"  {line.strip()}")
        if "Passed" in line:
            passed += 1
        if "***Failed" in line or "FAILED" in line:
            failed += 1
            print(f"  [FAIL] {line.strip()}")

    if r.returncode != 0:
        print(f"  [FAIL] {failed} テスト失敗 ({t:.1f}s)")
        return False

    print(f"  [OK] {passed}/10 PASS ({t:.1f}s)")
    return True


def build_apk():
    """Gradle APKビルド"""
    print("\n" + "=" * 60)
    print("[3/3] APK ビルド (Release)")
    print("=" * 60)

    gradlew = os.path.join(ANDROID_DIR, "gradlew.bat")
    if not os.path.exists(gradlew):
        print("  [SKIP] gradlew.bat not found")
        return True

    r, t = run([gradlew, ":app:assembleRelease", ":accessory:assembleRelease",
                ":capture:assembleRelease"], cwd=ANDROID_DIR, timeout=600)

    if r.returncode != 0:
        print(f"  [FAIL] Gradle build ({t:.1f}s)")
        # BUILD FAILED行を探す
        for line in r.stdout.split("\n"):
            if "BUILD" in line or "error" in line.lower() or "FAILURE" in line:
                print(f"    {line.strip()}")
        return False

    # APKサイズ確認
    for module in ["app", "accessory", "capture"]:
        apk = os.path.join(ANDROID_DIR, module, "build", "outputs", "apk", "release",
                           f"{module}-release.apk")
        if os.path.exists(apk):
            size = os.path.getsize(apk) / 1024 / 1024
            print(f"  [OK] {module}-release.apk ({size:.1f}MB)")
        else:
            print(f"  [WARN] {module}-release.apk not found")

    print(f"  [OK] APKビルド完了 ({t:.1f}s)")
    return True


def main():
    parser = argparse.ArgumentParser(description="MirageVulkan フルビルド")
    parser.add_argument("--cpp-only", action="store_true")
    parser.add_argument("--apk-only", action="store_true")
    parser.add_argument("--no-test", action="store_true")
    parser.add_argument("--clean", action="store_true")
    args = parser.parse_args()

    start = time.time()
    results = {}

    print("=" * 60)
    print("MirageVulkan Full Build")
    print("=" * 60)

    # C++
    if not args.apk_only:
        results["C++ Build"] = build_cpp(clean=args.clean)
        if results["C++ Build"] and not args.no_test:
            results["Tests"] = run_tests()

    # APK
    if not args.cpp_only:
        results["APK Build"] = build_apk()

    # サマリー
    total = time.time() - start
    print(f"\n{'=' * 60}")
    print(f"結果サマリー ({total:.1f}s)")
    print(f"{'=' * 60}")
    all_ok = True
    for name, ok in results.items():
        status = "OK" if ok else "FAIL"
        if not ok:
            all_ok = False
        print(f"  {name}: {status}")

    print(f"\n{'ALL PASS' if all_ok else 'SOME FAILED'}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
