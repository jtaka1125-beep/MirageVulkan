#!/usr/bin/env python3
"""
MirageSystem 統合CLIツール

全PC側操作をサブコマンド方式で統合。
ADBデバイス管理、APKビルド/インストール、ミラーリング、リモート操作を一元管理する。

使い方:
    python scripts/mirage_cli.py setup              # 初期セットアップ（全自動）
    python scripts/mirage_cli.py build              # APKビルド
    python scripts/mirage_cli.py install             # APKインストール
    python scripts/mirage_cli.py mirror              # ミラーリング開始
    python scripts/mirage_cli.py view                # 映像ビューア起動
    python scripts/mirage_cli.py tap 540 960         # リモートタップ
    python scripts/mirage_cli.py swipe 540 1500 540 500 300
    python scripts/mirage_cli.py text "hello"        # テキスト入力
    python scripts/mirage_cli.py key back            # キーイベント送信
    python scripts/mirage_cli.py screenshot           # スクリーンショット取得
    python scripts/mirage_cli.py log                  # ログ表示
"""

import argparse
import os
import platform
import re
import signal
import socket
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path


# ============================================================================
# 定数
# ============================================================================

# プロジェクトルート（このスクリプトの親の親ディレクトリ）
PROJECT_ROOT = Path(__file__).resolve().parent.parent

# Android関連
CAPTURE_PACKAGE = "com.mirage.capture"
CAPTURE_ACTIVITY = "com.mirage.capture/.ui.CaptureActivity"
ACCESSORY_PACKAGE = "com.mirage.accessory"
ACCESSIBILITY_SERVICE = (
    "com.mirage.android/com.mirage.android.access.MirageAccessibilityService"
)

# APKパス
APK_PATHS = {
    "capture": {
        "debug": PROJECT_ROOT / "android" / "capture" / "build" / "outputs" / "apk" / "debug" / "capture-debug.apk",
        "release": PROJECT_ROOT / "android" / "capture" / "build" / "outputs" / "apk" / "release" / "capture-release.apk",
    },
    "accessory": {
        "debug": PROJECT_ROOT / "android" / "accessory" / "build" / "outputs" / "apk" / "debug" / "accessory-debug.apk",
        "release": PROJECT_ROOT / "android" / "accessory" / "build" / "outputs" / "apk" / "release" / "accessory-release.apk",
    },
}

# ビルド環境
JAVA_HOME = r"C:\Program Files\Eclipse Adoptium\jdk-17.0.13.11-hotspot"
ANDROID_HOME = r"C:\Users\jun\AppData\Local\Android\Sdk"

# ネットワーク
DEFAULT_MIRROR_PORT = 50000
RECV_BUFFER_SIZE = 65536
RTP_HEADER_MIN = 12

# キーイベントエイリアス（名前 → KEYCODE）
KEY_ALIASES = {
    "back": "KEYCODE_BACK",
    "home": "KEYCODE_HOME",
    "menu": "KEYCODE_MENU",
    "recent": "KEYCODE_APP_SWITCH",
    "recents": "KEYCODE_APP_SWITCH",
    "enter": "KEYCODE_ENTER",
    "tab": "KEYCODE_TAB",
    "del": "KEYCODE_DEL",
    "delete": "KEYCODE_FORWARD_DEL",
    "up": "KEYCODE_DPAD_UP",
    "down": "KEYCODE_DPAD_DOWN",
    "left": "KEYCODE_DPAD_LEFT",
    "right": "KEYCODE_DPAD_RIGHT",
    "power": "KEYCODE_POWER",
    "volup": "KEYCODE_VOLUME_UP",
    "voldown": "KEYCODE_VOLUME_DOWN",
    "mute": "KEYCODE_VOLUME_MUTE",
    "play": "KEYCODE_MEDIA_PLAY_PAUSE",
    "camera": "KEYCODE_CAMERA",
    "search": "KEYCODE_SEARCH",
    "space": "KEYCODE_SPACE",
    "escape": "KEYCODE_ESCAPE",
    "esc": "KEYCODE_ESCAPE",
}


# ============================================================================
# ユーティリティ関数
# ============================================================================

def print_header(title):
    """セクションヘッダーを表示する"""
    print()
    print("=" * 60)
    print(f"  {title}")
    print("=" * 60)


def print_step(step_num, total, message):
    """ステップ表示"""
    print(f"\n[{step_num}/{total}] {message}")


def print_ok(message):
    """成功メッセージ"""
    print(f"  [OK] {message}")


def print_warn(message):
    """警告メッセージ"""
    print(f"  [警告] {message}")


def print_error(message):
    """エラーメッセージ"""
    print(f"  [エラー] {message}")


def print_info(message):
    """情報メッセージ"""
    print(f"  [INFO] {message}")


def run_cmd(cmd, timeout=30, check=False, capture=True):
    """
    外部コマンドを実行する。

    Args:
        cmd: コマンドリスト
        timeout: タイムアウト秒数
        check: Trueの場合、非ゼロ終了で例外を投げる
        capture: 出力をキャプチャするか
    Returns:
        subprocess.CompletedProcess
    """
    try:
        return subprocess.run(
            cmd,
            capture_output=capture,
            text=True,
            timeout=timeout,
        )
    except FileNotFoundError:
        print_error(f"コマンドが見つかりません: {cmd[0]}")
        sys.exit(1)
    except subprocess.TimeoutExpired:
        print_error(f"コマンドがタイムアウトしました ({timeout}秒): {' '.join(cmd)}")
        sys.exit(1)


def adb_cmd(serial, *args, timeout=30):
    """
    ADBコマンドを実行する。

    Args:
        serial: デバイスシリアル番号
        *args: ADBサブコマンドと引数
        timeout: タイムアウト秒数
    Returns:
        subprocess.CompletedProcess
    """
    cmd = ["adb", "-s", serial] + list(args)
    return run_cmd(cmd, timeout=timeout)


def adb_shell(serial, *args, timeout=30):
    """
    ADB shellコマンドを実行する。

    Args:
        serial: デバイスシリアル番号
        *args: shellコマンドと引数
        timeout: タイムアウト秒数
    Returns:
        subprocess.CompletedProcess
    """
    return adb_cmd(serial, "shell", *args, timeout=timeout)


# ============================================================================
# ADBデバイス検出
# ============================================================================

def find_devices():
    """
    接続中の全ADBデバイスを検出する。

    Returns:
        list[dict]: デバイス情報のリスト
            各要素: {"serial": str, "state": str, "type": "USB"|"WiFi"}
    """
    result = run_cmd(["adb", "devices", "-l"], timeout=10)
    if result.returncode != 0:
        print_error(f"adb devices 実行失敗: {result.stderr.strip()}")
        return []

    devices = []
    for line in result.stdout.strip().splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2:
            serial = parts[0]
            state = parts[1]
            conn_type = "WiFi" if ":" in serial else "USB"
            devices.append({
                "serial": serial,
                "state": state,
                "type": conn_type,
            })
    return devices


def find_device(preferred_serial=None):
    """
    接続中のデバイスを1台検出してシリアル番号を返す。

    Args:
        preferred_serial: 指定があればそのデバイスを優先
    Returns:
        str: デバイスのシリアル番号
    Raises:
        RuntimeError: デバイスが見つからない場合
    """
    devices = find_devices()
    # device状態のもののみ
    online = [d for d in devices if d["state"] == "device"]

    if not online:
        raise RuntimeError(
            "ADBデバイスが見つかりません。\n"
            "  - USBケーブルを確認してください\n"
            "  - adb devicesでデバイスが表示されるか確認してください\n"
            "  - WiFi接続の場合: adb connect <IP>:5555 を実行してください"
        )

    # 指定シリアルがあれば優先
    if preferred_serial:
        for d in online:
            if d["serial"] == preferred_serial:
                return d["serial"]
        raise RuntimeError(
            f"指定デバイス '{preferred_serial}' が見つかりません。\n"
            f"  接続中のデバイス: {[d['serial'] for d in online]}"
        )

    # 複数台ある場合は警告
    if len(online) > 1:
        print_warn(f"複数デバイス検出: {[d['serial'] for d in online]}")
        print_warn(f"最初のデバイス '{online[0]['serial']}' を使用します")
        print_warn("--device で指定することもできます")

    return online[0]["serial"]


# ============================================================================
# IPアドレス自動取得
# ============================================================================

def get_pc_ip():
    """
    PCのIPアドレスを自動取得する。
    UDPソケット接続法でネットワークインターフェースのIPを推定。

    Returns:
        str: IPアドレス文字列
    """
    # 方法1: UDPソケットによる推定
    for host in ["8.8.8.8", "1.1.1.1"]:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(1)
            s.connect((host, 80))
            ip = s.getsockname()[0]
            s.close()
            if ip and ip != "0.0.0.0":
                return ip
        except (OSError, socket.error):
            continue

    # 方法2: ipconfigから取得（Windows）
    if platform.system() == "Windows":
        try:
            result = run_cmd(["ipconfig"], timeout=5)
            for line in result.stdout.splitlines():
                if "IPv4" in line:
                    match = re.search(r"(\d+\.\d+\.\d+\.\d+)", line)
                    if match:
                        return match.group(1)
        except Exception:
            pass

    return "127.0.0.1"


# ============================================================================
# Gradleビルドヘルパー
# ============================================================================

def get_gradle_env():
    """
    Gradleビルド用の環境変数を構築する。
    JAVA_HOMEとANDROID_HOMEを自動設定。

    Returns:
        dict: 環境変数辞書
    """
    env = os.environ.copy()
    env["JAVA_HOME"] = JAVA_HOME
    env["ANDROID_HOME"] = ANDROID_HOME
    return env


def get_gradlew_path():
    """
    gradlew/gradlew.batのパスを返す。

    Returns:
        str: gradlewの実行パス
    """
    if platform.system() == "Windows":
        return str(PROJECT_ROOT / "android" / "gradlew.bat")
    return str(PROJECT_ROOT / "android" / "gradlew")


# ============================================================================
# サブコマンド: setup（初期セットアップ・全自動）
# ============================================================================

def cmd_setup(args):
    """
    初期セットアップを全自動で実行する。

    手順:
    1. ADBデバイス検出（USB/WiFi自動判別）
    2. APKビルド（capture + accessory）
    3. APKインストール（-r 上書き）
    4. appops PROJECT_MEDIA 権限付与
    5. AccessibilityService有効化確認
    6. 結果サマリー表示
    """
    print_header("MirageSystem 初期セットアップ")
    total_steps = 6
    results = {}

    # --- Step 1: デバイス検出 ---
    print_step(1, total_steps, "ADBデバイス検出...")
    try:
        serial = find_device(args.device)
        conn_type = "WiFi" if ":" in serial else "USB"
        print_ok(f"デバイス: {serial} ({conn_type})")
        results["device"] = f"{serial} ({conn_type})"
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # --- Step 2: APKビルド ---
    print_step(2, total_steps, "APKビルド (capture + accessory)...")
    build_ok = _build_modules(["capture", "accessory"], release=False)
    results["build"] = "成功" if build_ok else "失敗"
    if not build_ok:
        print_error("ビルドに失敗しました。続行しますが、インストールは既存APKを使用します")

    # --- Step 3: APKインストール ---
    print_step(3, total_steps, "APKインストール...")
    install_results = {}
    for module in ["capture", "accessory"]:
        apk_path = APK_PATHS[module]["debug"]
        if apk_path.exists():
            result = adb_cmd(serial, "install", "-r", str(apk_path), timeout=120)
            if result.returncode == 0:
                print_ok(f"{module} APKインストール完了")
                install_results[module] = "成功"
            else:
                print_error(f"{module} APKインストール失敗: {result.stderr.strip()}")
                install_results[module] = "失敗"
        else:
            print_warn(f"{module} APKが見つかりません: {apk_path}")
            install_results[module] = "APK無し"
    results["install"] = install_results

    # --- Step 4: appops PROJECT_MEDIA 権限付与 ---
    print_step(4, total_steps, "PROJECT_MEDIA権限付与...")
    r = adb_shell(serial, "appops", "set", CAPTURE_PACKAGE, "PROJECT_MEDIA", "allow")
    if r.returncode == 0:
        print_ok("PROJECT_MEDIA 権限を付与しました")
        results["appops"] = "成功"
    else:
        print_warn(f"appops設定に失敗: {r.stderr.strip()}")
        results["appops"] = "失敗"

    # --- Step 5: AccessibilityService有効化確認 ---
    print_step(5, total_steps, "AccessibilityService有効化確認...")
    r = adb_shell(serial, "settings", "get", "secure", "enabled_accessibility_services")
    current_services = r.stdout.strip() if r.returncode == 0 else ""
    if ACCESSIBILITY_SERVICE in current_services:
        print_ok("AccessibilityService は有効です")
        results["accessibility"] = "有効"
    else:
        print_warn("AccessibilityServiceが無効です")
        print_info(f"現在の設定: {current_services or '(なし)'}")
        print_info("手動で有効にしてください:")
        print_info("  設定 → ユーザー補助 → MirageAccessibilityService → ON")
        results["accessibility"] = "無効（手動設定が必要）"

    # --- Step 6: 結果サマリー ---
    print_step(6, total_steps, "結果サマリー")
    print()
    print("-" * 40)
    print(f"  デバイス           : {results['device']}")
    print(f"  ビルド             : {results['build']}")
    for mod, status in results["install"].items():
        print(f"  インストール({mod:>9s}) : {status}")
    print(f"  PROJECT_MEDIA      : {results['appops']}")
    print(f"  Accessibility      : {results['accessibility']}")
    print("-" * 40)

    # 全て成功ならOK
    all_ok = (
        results["build"] == "成功"
        and all(v == "成功" for v in results["install"].values())
        and results["appops"] == "成功"
        and results["accessibility"] == "有効"
    )
    if all_ok:
        print("\n  セットアップ完了！ 'mirage_cli.py mirror' でミラーリングを開始できます")
    else:
        print("\n  一部のステップに問題があります。上記の結果を確認してください")


# ============================================================================
# サブコマンド: build（APKビルド）
# ============================================================================

def _build_modules(modules, release=False):
    """
    指定モジュールのAPKをビルドする。

    Args:
        modules: ビルド対象モジュール名のリスト ("capture", "accessory")
        release: Trueならリリースビルド
    Returns:
        bool: 全モジュールのビルドが成功したか
    """
    gradlew = get_gradlew_path()
    env = get_gradle_env()
    build_type = "Release" if release else "Debug"
    all_ok = True

    print_info(f"JAVA_HOME  = {JAVA_HOME}")
    print_info(f"ANDROID_HOME = {ANDROID_HOME}")
    print_info(f"ビルドタイプ: {build_type}")

    for module in modules:
        task = f":{module}:assemble{build_type}"
        print_info(f"ビルド中: {task}")

        result = subprocess.run(
            [gradlew, task],
            cwd=str(PROJECT_ROOT / "android"),
            env=env,
            capture_output=True,
            text=True,
            timeout=600,  # ビルドは最大10分
        )

        if result.returncode == 0:
            variant = "release" if release else "debug"
            apk_path = APK_PATHS[module][variant]
            print_ok(f"{module} ビルド成功")
            if apk_path.exists():
                size_mb = apk_path.stat().st_size / (1024 * 1024)
                print_info(f"  APK: {apk_path} ({size_mb:.1f} MB)")
        else:
            print_error(f"{module} ビルド失敗")
            # エラー出力の最後10行を表示
            stderr_lines = result.stderr.strip().splitlines()
            for line in stderr_lines[-10:]:
                print(f"    {line}")
            all_ok = False

    return all_ok


def cmd_build(args):
    """APKビルドサブコマンド"""
    print_header("APKビルド")

    if args.module == "all":
        modules = ["capture", "accessory"]
    else:
        modules = [args.module]

    ok = _build_modules(modules, release=args.release)
    if not ok:
        sys.exit(1)
    print("\n  ビルド完了！")


# ============================================================================
# サブコマンド: install（APKインストール）
# ============================================================================

def cmd_install(args):
    """APKインストールサブコマンド"""
    print_header("APKインストール")

    # デバイス検出
    try:
        serial = find_device(args.device)
        conn_type = "WiFi" if ":" in serial else "USB"
        print_ok(f"デバイス: {serial} ({conn_type})")
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    if args.module == "all":
        modules = ["capture", "accessory"]
    else:
        modules = [args.module]

    variant = "release" if args.release else "debug"
    all_ok = True

    for module in modules:
        apk_path = APK_PATHS[module][variant]
        if not apk_path.exists():
            print_error(f"APKが見つかりません: {apk_path}")
            print_info("先に 'mirage_cli.py build' を実行してください")
            all_ok = False
            continue

        print_info(f"インストール中: {module} ({variant})...")
        result = adb_cmd(serial, "install", "-r", str(apk_path), timeout=120)
        if result.returncode == 0:
            print_ok(f"{module} インストール完了")
        else:
            print_error(f"{module} インストール失敗: {result.stderr.strip()}")
            all_ok = False

    if not all_ok:
        sys.exit(1)
    print("\n  インストール完了！")


# ============================================================================
# サブコマンド: mirror（ミラーリング開始）
# ============================================================================

def cmd_mirror(args):
    """
    ミラーリングを開始する。

    手順:
    1. appops確認・付与
    2. PCのIPアドレス自動取得
    3. CaptureActivity起動（auto_mirrorインテント）
    4. UDP受信開始
    5. 受信統計をリアルタイム表示
    6. Ctrl+Cで停止
    """
    print_header("ミラーリング開始")

    # デバイス検出
    try:
        serial = find_device(args.device)
        conn_type = "WiFi" if ":" in serial else "USB"
        print_ok(f"デバイス: {serial} ({conn_type})")
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # appops確認・付与
    print_info("PROJECT_MEDIA権限を付与...")
    adb_shell(serial, "appops", "set", CAPTURE_PACKAGE, "PROJECT_MEDIA", "allow")
    print_ok("PROJECT_MEDIA 権限付与済み")

    # PCのIPアドレス
    pc_ip = args.host or get_pc_ip()
    port = args.port
    print_ok(f"PC IP: {pc_ip}")
    print_ok(f"受信ポート: {port}")

    # CaptureActivity起動
    print_info("CaptureActivity起動中...")
    result = adb_shell(
        serial,
        "am", "start",
        "-n", CAPTURE_ACTIVITY,
        "--ez", "auto_mirror", "true",
        "--es", "mirror_host", pc_ip,
        "--ei", "mirror_port", str(port),
    )
    if result.returncode != 0:
        print_error(f"CaptureActivity起動失敗: {result.stderr.strip()}")
        sys.exit(1)
    print_ok("CaptureActivity起動完了")

    # MediaProjection許可待ち
    print_info("3秒待機中 (MediaProjection処理待ち)...")
    time.sleep(3)

    # UDP受信開始
    print()
    print(f"  映像待機中... (UDP ポート {port})")
    print("  Ctrl+C で停止")
    print()

    _receive_udp_stats(port, args.timeout)


def _receive_udp_stats(port, timeout):
    """
    UDPパケットを受信し、リアルタイムで統計を表示する。

    Args:
        port: 受信UDPポート番号
        timeout: タイムアウト秒数（0=無制限）
    """
    running = True

    def on_sigint(signum, frame):
        nonlocal running
        running = False

    old_handler = signal.signal(signal.SIGINT, on_sigint)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        sock.bind(("0.0.0.0", port))
    except OSError as e:
        print_error(f"ポート {port} のバインドに失敗: {e}")
        print_info("他のプロセスがポートを使用している可能性があります")
        signal.signal(signal.SIGINT, old_handler)
        sys.exit(1)
    sock.settimeout(1.0)

    # 統計カウンタ
    total_packets = 0
    total_bytes = 0
    interval_packets = 0
    interval_bytes = 0
    start_time = time.time()
    last_report = start_time
    effective_timeout = timeout if timeout > 0 else float("inf")

    try:
        while running:
            elapsed = time.time() - start_time
            if elapsed >= effective_timeout:
                print(f"\n  タイムアウト ({timeout}秒) に到達しました")
                break

            try:
                data, addr = sock.recvfrom(RECV_BUFFER_SIZE)
            except socket.timeout:
                if total_packets == 0:
                    print(f"\r  パケット待機中... ({elapsed:.0f}s)   ", end="", flush=True)
                continue

            total_packets += 1
            total_bytes += len(data)
            interval_packets += 1
            interval_bytes += len(data)

            # 1秒ごとに統計表示
            now = time.time()
            if now - last_report >= 1.0:
                elapsed = now - start_time
                # 区間ビットレート
                dt = now - last_report
                bps = interval_bytes * 8 / dt if dt > 0 else 0
                if bps >= 1_000_000:
                    rate_str = f"{bps / 1_000_000:.2f} Mbps"
                elif bps >= 1_000:
                    rate_str = f"{bps / 1_000:.1f} kbps"
                else:
                    rate_str = f"{bps:.0f} bps"

                pps = interval_packets / dt if dt > 0 else 0
                total_mb = total_bytes / (1024 * 1024)

                print(
                    f"\r  [{elapsed:6.1f}s] "
                    f"{rate_str:>12s} | "
                    f"{pps:6.0f} pkt/s | "
                    f"総計: {total_packets:>8d} pkts, {total_mb:>7.2f} MB   ",
                    end="", flush=True,
                )

                interval_packets = 0
                interval_bytes = 0
                last_report = now

    except Exception as e:
        print(f"\n  受信エラー: {e}")
    finally:
        sock.close()
        signal.signal(signal.SIGINT, old_handler)

    # 最終統計
    elapsed = time.time() - start_time
    avg_bps = total_bytes * 8 / elapsed if elapsed > 0 else 0
    if avg_bps >= 1_000_000:
        avg_str = f"{avg_bps / 1_000_000:.2f} Mbps"
    elif avg_bps >= 1_000:
        avg_str = f"{avg_bps / 1_000:.1f} kbps"
    else:
        avg_str = f"{avg_bps:.0f} bps"

    print(f"\n\n  --- 受信統計 ---")
    print(f"  受信時間       : {elapsed:.1f} 秒")
    print(f"  総パケット数   : {total_packets:,}")
    print(f"  総受信バイト   : {total_bytes:,} B ({total_bytes / (1024*1024):.2f} MB)")
    print(f"  平均ビットレート: {avg_str}")


# ============================================================================
# サブコマンド: view（映像ビューア起動）
# ============================================================================

def cmd_view(args):
    """hybrid_video_viewer.pyを起動するラッパー"""
    print_header("映像ビューア起動")

    viewer_script = PROJECT_ROOT / "scripts" / "hybrid_video_viewer.py"
    if not viewer_script.exists():
        print_error(f"ビューアスクリプトが見つかりません: {viewer_script}")
        sys.exit(1)

    # ビューアに渡す引数を構築
    cmd = [sys.executable, str(viewer_script)]

    if args.mode == "wifi-only" or args.mode == "udp":
        cmd.append("--wifi-only")
    elif args.mode == "usb":
        cmd.append("--usb")

    if args.port:
        cmd.extend(["--port", str(args.port)])

    print_info(f"起動: {' '.join(cmd)}")
    print_info("ビューアウィンドウが開きます...")

    # ビューアを独立プロセスとして起動
    try:
        subprocess.Popen(cmd)
        print_ok("ビューア起動完了")
    except Exception as e:
        print_error(f"ビューア起動失敗: {e}")
        sys.exit(1)


# ============================================================================
# サブコマンド: tap（リモートタップ）
# ============================================================================

def cmd_tap(args):
    """リモートタップを送信する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    result = adb_shell(serial, "input", "tap", str(args.x), str(args.y))
    if result.returncode == 0:
        print_ok(f"タップ送信: ({args.x}, {args.y})")
    else:
        print_error(f"タップ失敗: {result.stderr.strip()}")
        sys.exit(1)


# ============================================================================
# サブコマンド: swipe（リモートスワイプ）
# ============================================================================

def cmd_swipe(args):
    """リモートスワイプを送信する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    cmd_args = ["input", "swipe",
                str(args.x1), str(args.y1),
                str(args.x2), str(args.y2),
                str(args.duration)]

    result = adb_shell(serial, *cmd_args)
    if result.returncode == 0:
        print_ok(f"スワイプ送信: ({args.x1},{args.y1}) → ({args.x2},{args.y2}) {args.duration}ms")
    else:
        print_error(f"スワイプ失敗: {result.stderr.strip()}")
        sys.exit(1)


# ============================================================================
# サブコマンド: text（テキスト入力）
# ============================================================================

def cmd_text(args):
    """テキスト入力を送信する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # スペースは%sに変換（ADB input textの制約）
    text = args.text.replace(" ", "%s")

    result = adb_shell(serial, "input", "text", text)
    if result.returncode == 0:
        print_ok(f"テキスト送信: \"{args.text}\"")
    else:
        print_error(f"テキスト送信失敗: {result.stderr.strip()}")
        sys.exit(1)


# ============================================================================
# サブコマンド: key（キーイベント送信）
# ============================================================================

def cmd_key(args):
    """キーイベントを送信する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # エイリアス解決
    keycode = args.keycode
    alias_used = None
    lower = keycode.lower()
    if lower in KEY_ALIASES:
        alias_used = lower
        keycode = KEY_ALIASES[lower]
    elif not keycode.startswith("KEYCODE_"):
        # 数値でない場合はKEYCODE_プレフィックスを付与
        try:
            int(keycode)
        except ValueError:
            keycode = f"KEYCODE_{keycode.upper()}"

    result = adb_shell(serial, "input", "keyevent", keycode)
    if result.returncode == 0:
        if alias_used:
            print_ok(f"キーイベント送信: {alias_used} → {keycode}")
        else:
            print_ok(f"キーイベント送信: {keycode}")
    else:
        print_error(f"キーイベント送信失敗: {result.stderr.strip()}")
        # 利用可能なエイリアスを表示
        print_info("利用可能なエイリアス:")
        aliases = sorted(KEY_ALIASES.keys())
        for i in range(0, len(aliases), 6):
            chunk = aliases[i:i + 6]
            print(f"    {', '.join(chunk)}")
        sys.exit(1)


# ============================================================================
# サブコマンド: screenshot（スクリーンショット取得）
# ============================================================================

def cmd_screenshot(args):
    """スクリーンショットを取得してPNG保存する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # 出力パス決定
    if args.output:
        output_path = Path(args.output)
    else:
        # デフォルト: screenshots/ss_SERIAL_TIMESTAMP.png
        ss_dir = PROJECT_ROOT / "screenshots"
        ss_dir.mkdir(exist_ok=True)
        # シリアル番号からファイル名に使えない文字を除去
        safe_serial = re.sub(r"[^a-zA-Z0-9_\-.]", "_", serial)
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = ss_dir / f"ss_{safe_serial}_{timestamp}.png"

    # 出力ディレクトリが存在するか確認
    output_path.parent.mkdir(parents=True, exist_ok=True)

    # Android上でスクリーンキャプチャ → PC側にpull
    remote_path = "/sdcard/mirage_screenshot.png"
    print_info("スクリーンショット取得中...")

    r = adb_shell(serial, "screencap", "-p", remote_path, timeout=15)
    if r.returncode != 0:
        print_error(f"screencap失敗: {r.stderr.strip()}")
        sys.exit(1)

    r = adb_cmd(serial, "pull", remote_path, str(output_path), timeout=15)
    if r.returncode != 0:
        print_error(f"pull失敗: {r.stderr.strip()}")
        sys.exit(1)

    # リモート一時ファイル削除
    adb_shell(serial, "rm", remote_path)

    if output_path.exists():
        size_kb = output_path.stat().st_size / 1024
        print_ok(f"保存完了: {output_path} ({size_kb:.1f} KB)")
    else:
        print_error("スクリーンショットの保存に失敗しました")
        sys.exit(1)


# ============================================================================
# サブコマンド: log（ログ表示）
# ============================================================================

def cmd_log(args):
    """Logcatを表示する"""
    try:
        serial = find_device(args.device)
    except RuntimeError as e:
        print_error(str(e))
        sys.exit(1)

    # フィルタ構築
    logcat_args = ["logcat"]

    if args.clear:
        # ログバッファクリア
        adb_cmd(serial, "logcat", "-c")
        print_ok("ログバッファをクリアしました")

    if args.tag:
        # タグフィルタ: 指定タグのみ表示、他はサイレント
        for tag in args.tag:
            logcat_args.append(f"{tag}:V")
        logcat_args.append("*:S")
    elif args.mirage:
        # Mirageアプリのログのみ表示
        logcat_args.extend([
            "MirageCapture:V",
            "MirageAccessory:V",
            "MirageAccessibility:V",
            "CaptureService:V",
            "RtpSender:V",
            "CommandReceiver:V",
            "AOA:V",
            "*:S",
        ])

    if args.level:
        level_map = {"v": "V", "d": "D", "i": "I", "w": "W", "e": "E", "f": "F"}
        level = level_map.get(args.level.lower(), args.level.upper())
        logcat_args.extend(["-v", "brief", f"*:{level}"])

    if args.lines:
        logcat_args.extend(["-t", str(args.lines)])

    print_info(f"logcat実行中... (Ctrl+C で停止)")
    print_info(f"  adb -s {serial} {' '.join(logcat_args)}")
    print()

    # logcatをリアルタイム表示（Ctrl+Cで停止）
    cmd = ["adb", "-s", serial] + logcat_args
    try:
        proc = subprocess.Popen(cmd, stdout=sys.stdout, stderr=sys.stderr)
        proc.wait()
    except KeyboardInterrupt:
        proc.terminate()
        print("\n  logcat停止")


# ============================================================================
# メインエントリポイント: argparse定義
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        prog="mirage_cli",
        description="MirageSystem 統合CLIツール — 全PC側操作を一元管理",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "使用例:\n"
            "  python mirage_cli.py setup                # 全自動セットアップ\n"
            "  python mirage_cli.py build --module capture  # captureのみビルド\n"
            "  python mirage_cli.py mirror                # ミラーリング開始\n"
            "  python mirage_cli.py tap 540 960           # タップ送信\n"
            "  python mirage_cli.py key back              # 戻るキー\n"
            "  python mirage_cli.py log --mirage          # Mirageログのみ\n"
        ),
    )

    # 共通引数
    parser.add_argument(
        "--device", "-d", type=str, default=None,
        help="ADBデバイスのシリアル番号（省略時は自動検出）",
    )

    subparsers = parser.add_subparsers(
        dest="command",
        title="サブコマンド",
        description="利用可能なコマンド一覧",
    )

    # --- setup ---
    sp_setup = subparsers.add_parser(
        "setup",
        help="初期セットアップ（全自動）",
        description="ADBデバイス検出 → APKビルド → インストール → 権限付与 → Accessibility確認",
    )
    sp_setup.set_defaults(func=cmd_setup)

    # --- build ---
    sp_build = subparsers.add_parser(
        "build",
        help="APKビルド",
        description="capture/accessoryモジュールのAPKをビルドする",
    )
    sp_build.add_argument(
        "--module", "-m", choices=["capture", "accessory", "all"], default="all",
        help="ビルド対象モジュール（デフォルト: all）",
    )
    sp_build.add_argument(
        "--release", "-r", action="store_true",
        help="リリースビルド（デフォルトはdebug）",
    )
    sp_build.set_defaults(func=cmd_build)

    # --- install ---
    sp_install = subparsers.add_parser(
        "install",
        help="APKインストール",
        description="ビルド済みAPKをデバイスにインストールする",
    )
    sp_install.add_argument(
        "--module", "-m", choices=["capture", "accessory", "all"], default="all",
        help="インストール対象モジュール（デフォルト: all）",
    )
    sp_install.add_argument(
        "--release", "-r", action="store_true",
        help="リリースAPKをインストール（デフォルトはdebug）",
    )
    sp_install.set_defaults(func=cmd_install)

    # --- mirror ---
    sp_mirror = subparsers.add_parser(
        "mirror",
        help="ミラーリング開始",
        description="CaptureActivityを起動しUDP映像パケットを受信・統計表示する",
    )
    sp_mirror.add_argument(
        "--port", "-p", type=int, default=DEFAULT_MIRROR_PORT,
        help=f"受信UDPポート番号（デフォルト: {DEFAULT_MIRROR_PORT}）",
    )
    sp_mirror.add_argument(
        "--host", type=str, default=None,
        help="PCのIPアドレス（省略時は自動取得）",
    )
    sp_mirror.add_argument(
        "--timeout", "-t", type=int, default=0,
        help="受信タイムアウト秒数（デフォルト: 0=無制限）",
    )
    sp_mirror.set_defaults(func=cmd_mirror)

    # --- view ---
    sp_view = subparsers.add_parser(
        "view",
        help="映像ビューア起動",
        description="hybrid_video_viewer.pyを起動して映像をリアルタイム表示する",
    )
    sp_view.add_argument(
        "--mode", choices=["udp", "tcp", "usb"], default="udp",
        help="受信モード（デフォルト: udp）",
    )
    sp_view.add_argument(
        "--port", "-p", type=int, default=None,
        help="受信ポート番号",
    )
    sp_view.set_defaults(func=cmd_view)

    # --- tap ---
    sp_tap = subparsers.add_parser(
        "tap",
        help="リモートタップ送信",
        description="ADB経由でタップイベントを送信する",
    )
    sp_tap.add_argument("x", type=int, help="タップX座標")
    sp_tap.add_argument("y", type=int, help="タップY座標")
    sp_tap.set_defaults(func=cmd_tap)

    # --- swipe ---
    sp_swipe = subparsers.add_parser(
        "swipe",
        help="リモートスワイプ送信",
        description="ADB経由でスワイプイベントを送信する",
    )
    sp_swipe.add_argument("x1", type=int, help="開始X座標")
    sp_swipe.add_argument("y1", type=int, help="開始Y座標")
    sp_swipe.add_argument("x2", type=int, help="終了X座標")
    sp_swipe.add_argument("y2", type=int, help="終了Y座標")
    sp_swipe.add_argument(
        "duration", type=int, nargs="?", default=300,
        help="スワイプ時間ミリ秒（デフォルト: 300）",
    )
    sp_swipe.set_defaults(func=cmd_swipe)

    # --- text ---
    sp_text = subparsers.add_parser(
        "text",
        help="テキスト入力",
        description="ADB経由でテキストを入力する",
    )
    sp_text.add_argument("text", type=str, help="入力するテキスト")
    sp_text.set_defaults(func=cmd_text)

    # --- key ---
    sp_key = subparsers.add_parser(
        "key",
        help="キーイベント送信",
        description=(
            "ADB経由でキーイベントを送信する。\n"
            "エイリアス対応: back, home, menu, enter, up, down, left, right, "
            "power, volup, voldown, play, camera 等"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sp_key.add_argument(
        "keycode", type=str,
        help="キーコード（エイリアス名 or KEYCODE_* or 数値）",
    )
    sp_key.set_defaults(func=cmd_key)

    # --- screenshot ---
    sp_ss = subparsers.add_parser(
        "screenshot",
        help="スクリーンショット取得",
        description="Android端末のスクリーンショットをPNG保存する",
    )
    sp_ss.add_argument(
        "--output", "-o", type=str, default=None,
        help="出力ファイルパス（デフォルト: screenshots/ss_SERIAL_TIMESTAMP.png）",
    )
    sp_ss.set_defaults(func=cmd_screenshot)

    # --- log ---
    sp_log = subparsers.add_parser(
        "log",
        help="Logcat表示",
        description="Android端末のlogcatをリアルタイム表示する",
    )
    sp_log.add_argument(
        "--tag", type=str, nargs="+", default=None,
        help="フィルタするタグ名（複数指定可）",
    )
    sp_log.add_argument(
        "--mirage", action="store_true",
        help="Mirage関連ログのみ表示",
    )
    sp_log.add_argument(
        "--level", "-l", type=str, default=None,
        choices=["v", "d", "i", "w", "e", "f"],
        help="最低ログレベル (v/d/i/w/e/f)",
    )
    sp_log.add_argument(
        "--lines", "-n", type=int, default=None,
        help="最新N行のみ表示（省略時はリアルタイム）",
    )
    sp_log.add_argument(
        "--clear", "-c", action="store_true",
        help="表示前にログバッファをクリア",
    )
    sp_log.set_defaults(func=cmd_log)

    # --- パース実行 ---
    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        sys.exit(0)

    # サブコマンド実行
    args.func(args)


if __name__ == "__main__":
    main()
