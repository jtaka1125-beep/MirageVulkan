# DEPRECATED: このスクリプトは旧版です。
# 最新版は auto_setup/bluetooth_adb_setup.py の BluetoothAdbSetup.auto_pair() に統合されました。
# tools/bt_pair_v4.py が最新の自動ペアリング実装です。
#!/usr/bin/env python3
"""
PC↔Android Bluetoothペアリング完全自動化 統合スクリプト
======================================================

PC側(PowerShell WinRT API) と Android側(UIAutomator自動タップ) を
同時並行で実行し、手動操作ゼロでBluetoothペアリングを完了する。

Usage:
  python scripts/bt_auto_pair.py                          # 自動検出で実行
  python scripts/bt_auto_pair.py --device A9250700956     # デバイス指定
  python scripts/bt_auto_pair.py --timeout 60             # タイムアウト60秒
  python scripts/bt_auto_pair.py --skip-android           # PC側のみ実行

依存: adb (PATHに必要), powershell.exe (Windows標準)
追加パッケージ不要（標準ライブラリのみ）
"""

import subprocess
import sys
import os
import re
import time
import argparse
import shutil
import threading
import xml.etree.ElementTree as ET

# ─── 定数 ───────────────────────────────────────────────
ADB = os.environ.get("ADB") or shutil.which("adb") or "adb"
POWERSHELL = r"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PS_SCRIPT = os.path.join(SCRIPT_DIR, "bt_auto_pair.ps1")

# ペアリングダイアログ検出のキーワード
BT_PAIR_TEXTS_JP = ["ペア設定する", "ペアリング"]
BT_PAIR_TEXTS_EN = ["Pair", "PAIR"]
BT_PAIR_BUTTON_ID = "android:id/button1"

# ポーリング間隔（秒）
POLL_INTERVAL = 1.0
UI_DUMP_TIMEOUT = 5

# ─── ANSIカラー ─────────────────────────────────────────
if sys.platform == "win32":
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)
        mode = ctypes.c_ulong()
        kernel32.GetConsoleMode(handle, ctypes.byref(mode))
        kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    except Exception:
        pass


class C:
    """ANSIカラーコード"""
    RESET = "\033[0m"
    BOLD = "\033[1m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    DIM = "\033[2m"


def ok(msg):
    print(f"  {C.GREEN}[OK]{C.RESET} {msg}")


def fail(msg):
    print(f"  {C.RED}[NG]{C.RESET} {msg}")


def info(msg):
    print(f"  {C.CYAN}[..]{C.RESET} {msg}")


def warn(msg):
    print(f"  {C.YELLOW}[!!]{C.RESET} {msg}")


def header(msg):
    print(f"\n{C.BOLD}{C.CYAN}{'=' * 60}{C.RESET}")
    print(f"{C.BOLD}  {msg}{C.RESET}")
    print(f"{C.BOLD}{C.CYAN}{'=' * 60}{C.RESET}")


# ─── ADB ヘルパー ───────────────────────────────────────
def run_adb(args, serial=None, timeout=15):
    """ADBコマンドを実行して (成功?, stdout, stderr) を返す"""
    cmd = [ADB]
    if serial:
        cmd.extend(["-s", serial])
    if isinstance(args, str):
        args = args.split()
    cmd.extend(args)
    try:
        result = subprocess.run(
            cmd, capture_output=True, text=True, timeout=timeout
        )
        return result.returncode == 0, result.stdout.strip(), result.stderr.strip()
    except subprocess.TimeoutExpired:
        return False, "", "タイムアウト"
    except Exception as e:
        return False, "", str(e)


def shell(cmd, serial=None, timeout=15):
    """adb shell コマンドのstdoutを返す"""
    _, out, _ = run_adb(["shell"] + cmd.split(), serial, timeout)
    return out


def tap(serial, x, y):
    """画面タップ"""
    shell(f"input tap {x} {y}", serial)


# ─── デバイス検出 ───────────────────────────────────────
def get_connected_devices():
    """USB接続済みのADBデバイス一覧を返す"""
    success, out, _ = run_adb(["devices", "-l"])
    devices = []
    if not success:
        return devices
    for line in out.strip().split("\n")[1:]:
        if not line.strip():
            continue
        if "\tdevice" not in line and " device " not in line:
            continue
        parts = line.split()
        serial = parts[0]
        # WiFi/BT接続デバイスはスキップ（USB接続のみ）
        if ":" in serial:
            continue
        model = "Unknown"
        for p in parts:
            if p.startswith("model:"):
                model = p.split(":", 1)[1]
                break
        devices.append({"serial": serial, "model": model})
    return devices


def get_device_bt_name(serial):
    """AndroidデバイスのBluetooth名を取得"""
    # 方法1: settings
    name = shell("settings get secure bluetooth_name", serial)
    if name and name != "null" and name.strip():
        return name.strip()
    # 方法2: プロパティ
    name = shell("getprop ro.product.model", serial)
    if name and name.strip():
        return name.strip()
    return "Android"


# ─── UIダンプ & XML解析 ─────────────────────────────────
def dump_ui(serial):
    """UIツリーをダンプしてXML文字列を返す"""
    remote_path = "/data/local/tmp/ui_dump.xml"
    success, _, _ = run_adb(
        ["shell", "uiautomator", "dump", remote_path],
        serial, timeout=UI_DUMP_TIMEOUT
    )
    if not success:
        return ""
    _, xml_str, _ = run_adb(["shell", "cat", remote_path], serial)
    return xml_str


def parse_bounds(bounds_str):
    """bounds="[x1,y1][x2,y2]" → (cx, cy) 中心座標"""
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds_str)
    if not m:
        return None
    x1, y1, x2, y2 = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    return ((x1 + x2) // 2, (y1 + y2) // 2)


def find_node_by_text(xml_str, text):
    """XMLからテキストを含むノードの中心座標を返す"""
    try:
        root = ET.fromstring(xml_str)
    except ET.ParseError:
        return None
    for node in root.iter("node"):
        node_text = node.get("text", "")
        content_desc = node.get("content-desc", "")
        if text in node_text or text in content_desc:
            bounds = node.get("bounds", "")
            pos = parse_bounds(bounds)
            if pos:
                return pos
    return None


def find_node_by_resource_id(xml_str, resource_id):
    """XMLからresource-idでノードの中心座標を返す"""
    try:
        root = ET.fromstring(xml_str)
    except ET.ParseError:
        return None
    for node in root.iter("node"):
        if node.get("resource-id", "") == resource_id:
            bounds = node.get("bounds", "")
            pos = parse_bounds(bounds)
            if pos:
                return pos
    return None


def read_pin_from_xml(xml_str):
    """XMLからPINコード（6桁数字）を読み取る"""
    matches = re.findall(r"\b(\d{6})\b", xml_str)
    return matches[0] if matches else None


# ─── Android側: ペアリングダイアログ自動承認 ────────────
def android_auto_accept(serial, device_name, timeout_sec, result_holder):
    """
    Android側でペアリングダイアログを監視し、自動タップする。
    別スレッドで実行される。
    """
    thread_name = f"Android({device_name})"
    info(f"[{thread_name}] ペアリングダイアログ監視開始（{timeout_sec}秒）")

    start = time.time()
    accepted = False
    pin_code = None

    while time.time() - start < timeout_sec:
        xml = dump_ui(serial)
        if not xml:
            time.sleep(POLL_INTERVAL)
            continue

        # ペアリングダイアログを検出
        # alertTitle に「ペア」「Pair」が含まれているか
        has_dialog = False
        if "alertTitle" in xml:
            if any(kw in xml for kw in ["ペア", "Pair", "pair", "Bluetooth", "bluetooth"]):
                has_dialog = True

        if not has_dialog:
            elapsed = int(time.time() - start)
            # 進捗表示（同一行上書き）
            if elapsed % 5 == 0 and elapsed > 0:
                info(f"[{thread_name}] {elapsed}秒経過... ダイアログ待機中")
            time.sleep(POLL_INTERVAL)
            continue

        ok(f"[{thread_name}] ペアリングダイアログを検出！")

        # PINコード読み取り
        pin_code = read_pin_from_xml(xml)
        if pin_code:
            ok(f"[{thread_name}] PIN: {C.BOLD}{pin_code}{C.RESET}")
        else:
            info(f"[{thread_name}] PINコード表示なし（確認のみ）")

        # ペアリングボタンをタップ
        btn_pos = None

        # resource-id で検索
        btn_pos = find_node_by_resource_id(xml, BT_PAIR_BUTTON_ID)

        # 日本語テキストで検索
        if not btn_pos:
            for text in BT_PAIR_TEXTS_JP:
                btn_pos = find_node_by_text(xml, text)
                if btn_pos:
                    break

        # 英語テキストで検索
        if not btn_pos:
            for text in BT_PAIR_TEXTS_EN:
                btn_pos = find_node_by_text(xml, text)
                if btn_pos:
                    break

        if btn_pos:
            tap(serial, btn_pos[0], btn_pos[1])
            ok(f"[{thread_name}] ペアリングボタンをタップ ({btn_pos[0]}, {btn_pos[1]})")
            accepted = True
        else:
            # フォールバック: 画面サイズから推定タップ
            warn(f"[{thread_name}] ボタン未検出、推定位置でタップ")
            size_output = shell("wm size", serial)
            sw, sh = 1080, 2340  # デフォルト
            size_match = re.search(r"(\d+)x(\d+)", size_output)
            if size_match:
                sw, sh = int(size_match.group(1)), int(size_match.group(2))
            # ダイアログのボタンは通常右下付近
            tap(serial, int(sw * 0.75), int(sh * 0.55))
            accepted = True

        # 承認後に少し待ってダイアログが消えたか確認
        time.sleep(1.5)
        xml_after = dump_ui(serial)
        if xml_after and "alertTitle" in xml_after and "ペア" in xml_after:
            warn(f"[{thread_name}] ダイアログがまだ表示されています。再タップ...")
            # 再度ボタンを探してタップ
            btn2 = find_node_by_resource_id(xml_after, BT_PAIR_BUTTON_ID)
            if btn2:
                tap(serial, btn2[0], btn2[1])
                time.sleep(1)

        break

    # 結果を格納
    result_holder["accepted"] = accepted
    result_holder["pin"] = pin_code

    if not accepted:
        warn(f"[{thread_name}] タイムアウト: ペアリングダイアログが現れませんでした")


# ─── PC側: PowerShellスクリプト実行 ─────────────────────
def run_pc_pairing(device_name, timeout_sec, result_holder):
    """
    PC側のPowerShellスクリプトをsubprocessで実行する。
    別スレッドで実行される。
    """
    thread_name = "PC(PowerShell)"
    info(f"[{thread_name}] BT自動ペアリングスクリプトを起動中...")

    if not os.path.exists(PS_SCRIPT):
        fail(f"[{thread_name}] スクリプトが見つかりません: {PS_SCRIPT}")
        result_holder["success"] = False
        result_holder["output"] = "スクリプトファイルが存在しません"
        return

    # PowerShell 5.1 (Windows標準) を使用
    ps_exe = POWERSHELL
    if not os.path.exists(ps_exe):
        # フォールバック: PATHから検索
        ps_exe = shutil.which("powershell") or "powershell.exe"

    cmd = [
        ps_exe,
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", PS_SCRIPT,
        "-TimeoutSeconds", str(timeout_sec),
    ]
    if device_name:
        cmd.extend(["-DeviceName", device_name])

    info(f"[{thread_name}] 実行: {' '.join(cmd)}")

    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout_sec + 30  # スクリプトのタイムアウト+余裕
        )
        output = result.stdout + result.stderr
        result_holder["output"] = output
        result_holder["returncode"] = result.returncode

        # 出力をリアルタイム表示
        for line in output.strip().split("\n"):
            if line.strip():
                print(f"  {C.DIM}[PS] {line.strip()}{C.RESET}")

        if result.returncode == 0:
            ok(f"[{thread_name}] ペアリング成功")
            result_holder["success"] = True
        else:
            fail(f"[{thread_name}] ペアリング失敗 (exit={result.returncode})")
            result_holder["success"] = False

    except subprocess.TimeoutExpired:
        fail(f"[{thread_name}] タイムアウト ({timeout_sec + 30}秒)")
        result_holder["success"] = False
        result_holder["output"] = "タイムアウト"
    except Exception as e:
        fail(f"[{thread_name}] エラー: {e}")
        result_holder["success"] = False
        result_holder["output"] = str(e)


# ─── Bluetooth設定画面を開く ────────────────────────────
def open_bt_settings(serial):
    """AndroidのBluetooth設定画面を開く"""
    info("Bluetooth設定画面を開いています...")
    success, out, err = run_adb(
        ["shell", "am", "start", "-a", "android.settings.BLUETOOTH_SETTINGS"],
        serial
    )
    if success:
        ok("Bluetooth設定画面を開きました")
    else:
        warn(f"設定画面のオープンに失敗: {err}")
    time.sleep(2)


# ─── ペアリング状態確認 ─────────────────────────────────
def verify_pairing(serial):
    """Bluetoothペアリング状態を確認"""
    info("ペアリング状態を確認中...")

    # ペアリング済みデバイス一覧を取得
    paired_output = shell(
        "dumpsys bluetooth_manager | grep -A 2 'Bonded devices'",
        serial, timeout=10
    )
    if paired_output:
        ok(f"ペアリング情報:\n      {paired_output}")
        return True

    # 代替方法: service call
    bonded = shell("service call bluetooth_manager 6", serial)
    if bonded and "00000001" in bonded:
        ok("Bluetoothマネージャ: ペアリング済みデバイスあり")
        return True

    warn("ペアリング状態を確認できませんでした")
    return False


# ─── メイン処理 ─────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="PC↔Android Bluetoothペアリング完全自動化",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
例:
  python scripts/bt_auto_pair.py                          自動検出で実行
  python scripts/bt_auto_pair.py --device A9250700956     デバイス指定
  python scripts/bt_auto_pair.py --timeout 60             タイムアウト60秒
  python scripts/bt_auto_pair.py --skip-android           PC側のみ実行
  python scripts/bt_auto_pair.py --skip-pc                Android側のみ実行
        """,
    )
    parser.add_argument("--device", "-d", default=None,
                        help="ADBデバイスシリアル番号を指定")
    parser.add_argument("--timeout", "-t", type=int, default=30,
                        help="タイムアウト秒 (デフォルト: 30)")
    parser.add_argument("--skip-android", action="store_true",
                        help="Android側の自動承認をスキップ（PC側のみ）")
    parser.add_argument("--skip-pc", action="store_true",
                        help="PC側のペアリングをスキップ（Android側のみ）")
    parser.add_argument("--open-settings", action="store_true",
                        help="Android側のBluetooth設定画面を先に開く")

    args = parser.parse_args()

    header("Mirage BT Auto-Pair (統合)")
    print(f"  ADB: {ADB}")
    print(f"  PowerShell: {POWERSHELL}")
    print(f"  タイムアウト: {args.timeout}秒")

    # ─── Step 1: デバイス検出 ─────────────────────────
    info("USBデバイスを検出中...")
    devices = get_connected_devices()
    if not devices and not args.skip_android:
        fail("USB接続されたADBデバイスが見つかりません")
        fail("USBデバッグを有効にしてUSBケーブルで接続してください")
        sys.exit(1)

    # 対象デバイスを選択
    target = None
    if args.device:
        for dev in devices:
            if dev["serial"] == args.device:
                target = dev
                break
        if not target:
            fail(f"指定デバイス '{args.device}' が見つかりません")
            sys.exit(1)
    elif devices:
        target = devices[0]
        if len(devices) > 1:
            warn(f"{len(devices)}台検出。最初のデバイスを使用: {target['model']}")

    serial = target["serial"] if target else None
    model = target["model"] if target else "Unknown"

    # ─── Step 2: Bluetooth名を取得 ───────────────────
    bt_name = ""
    if serial:
        bt_name = get_device_bt_name(serial)
        ok(f"デバイス: {model} ({serial})")
        ok(f"Bluetooth名: {bt_name}")

    # ─── Step 3: Bluetooth設定画面を開く（オプション）─
    if args.open_settings and serial:
        open_bt_settings(serial)

    # ─── Step 4: PC側とAndroid側を並行実行 ───────────
    header("ペアリング開始")

    pc_result = {"success": False, "output": "", "returncode": -1}
    android_result = {"accepted": False, "pin": None}
    threads = []

    # PC側スレッド起動
    if not args.skip_pc:
        pc_thread = threading.Thread(
            target=run_pc_pairing,
            args=(bt_name, args.timeout, pc_result),
            daemon=True,
            name="PC-Pairing"
        )
        threads.append(pc_thread)
    else:
        info("PC側ペアリングをスキップ")
        pc_result["success"] = True

    # Android側スレッド起動
    if not args.skip_android and serial:
        android_thread = threading.Thread(
            target=android_auto_accept,
            args=(serial, model, args.timeout, android_result),
            daemon=True,
            name="Android-Accept"
        )
        threads.append(android_thread)
    else:
        info("Android側自動承認をスキップ")
        android_result["accepted"] = True

    # 全スレッド開始
    for t in threads:
        t.start()

    # 全スレッドの完了を待機
    for t in threads:
        t.join(timeout=args.timeout + 60)

    # ─── Step 5: 結果確認 ────────────────────────────
    header("結果")

    pc_ok = pc_result.get("success", False)
    android_ok = android_result.get("accepted", False)

    print(f"  PC側ペアリング:       {'成功' if pc_ok else '失敗'}")
    print(f"  Android側自動承認:    {'成功' if android_ok else '失敗'}")

    # ペアリング状態確認
    if serial and (pc_ok or android_ok):
        time.sleep(2)
        verify_pairing(serial)

    if pc_ok and android_ok:
        print(f"\n  {C.GREEN}{C.BOLD}Bluetoothペアリング完了！{C.RESET}")
        sys.exit(0)
    elif pc_ok or android_ok:
        print(f"\n  {C.YELLOW}一部成功。もう片方を手動で確認してください。{C.RESET}")
        sys.exit(1)
    else:
        print(f"\n  {C.RED}ペアリングに失敗しました。{C.RESET}")
        print(f"  {C.DIM}ヒント: Android側でBluetoothが有効か確認してください{C.RESET}")
        print(f"  {C.DIM}ヒント: PC側でBluetoothアダプタが有効か確認してください{C.RESET}")
        sys.exit(1)


if __name__ == "__main__":
    main()
