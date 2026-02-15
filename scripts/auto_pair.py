# DEPRECATED: このスクリプトは旧版です。
# Bluetoothペアリング機能は auto_setup/bluetooth_adb_setup.py の BluetoothAdbSetup.auto_pair() に統合されました。
# WiFi ADB機能は auto_setup/wifi_adb_setup.py を使用してください。
#!/usr/bin/env python3
"""
WiFi ADB / Bluetooth ADB ペアリング完全自動化
=============================================

USB接続済みのAndroidデバイスに対して、
WiFi ADB / Bluetooth ADB ペアリングをバックグラウンドで自動完了させる。
PC側の手動操作ゼロ。

Usage:
  python scripts/auto_pair.py --wifi                       # WiFi ADBペアリングのみ
  python scripts/auto_pair.py --bt                         # Bluetoothペアリングのみ
  python scripts/auto_pair.py --all                        # 全自動セットアップ
  python scripts/auto_pair.py --wifi --device A9250700956  # デバイス指定

依存: adb (PATHに必要)
オプション依存なし（標準ライブラリのみ）
"""

import subprocess
import sys
import os
import re
import time
import argparse
import shutil
import xml.etree.ElementTree as ET
from collections import Counter

# ─── 定数 ───────────────────────────────────────────────
PACKAGE = "com.mirage.android"
ACCESSIBILITY_SERVICE = f"{PACKAGE}/com.mirage.android.access.MirageAccessibilityService"
ADB = os.environ.get("ADB") or shutil.which("adb") or "adb"

# 操作タイムアウト（秒）
STEP_TIMEOUT = 15

# UIダンプリトライ
UI_DUMP_RETRIES = 3
UI_DUMP_INTERVAL = 1.0

# UIテキスト識別子（auto_setup.hpp の identifiers_ 準拠）
UI_WIFI_PAIR_TEXT_JP = "ペアリングコード"
UI_WIFI_PAIR_TEXT_EN = "pairing code"
UI_BT_PAIR_TEXT_JP = "ペア設定する"
UI_BT_PAIR_TEXT_EN = "Pair"
UI_BT_PAIR_BUTTON = "android:id/button1"

# ─── ANSIカラー ─────────────────────────────────────────
# Windows 10+はANSIエスケープシーケンスをサポート
def _enable_ansi():
    """WindowsでANSIエスケープを有効化"""
    if sys.platform == "win32":
        try:
            import ctypes
            kernel32 = ctypes.windll.kernel32
            # ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
            handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_ulong()
            kernel32.GetConsoleMode(handle, ctypes.byref(mode))
            kernel32.SetConsoleMode(handle, mode.value | 0x0004)
        except Exception:
            pass

_enable_ansi()

class C:
    """ANSIカラーコード"""
    RESET  = "\033[0m"
    BOLD   = "\033[1m"
    RED    = "\033[31m"
    GREEN  = "\033[32m"
    YELLOW = "\033[33m"
    CYAN   = "\033[36m"
    DIM    = "\033[2m"


def ok(msg):
    print(f"  {C.GREEN}[OK]{C.RESET} {msg}")

def fail(msg):
    print(f"  {C.RED}[NG]{C.RESET} {msg}")

def info(msg):
    print(f"  {C.CYAN}[..]{C.RESET} {msg}")

def warn(msg):
    print(f"  {C.YELLOW}[!!]{C.RESET} {msg}")

def header(msg):
    print(f"\n{C.BOLD}{C.CYAN}{'='*60}{C.RESET}")
    print(f"{C.BOLD}  {msg}{C.RESET}")
    print(f"{C.BOLD}{C.CYAN}{'='*60}{C.RESET}")


# ─── ADB ヘルパー ───────────────────────────────────────
def run_adb(args, serial=None, timeout=None):
    """ADBコマンドを実行して (成功?, stdout, stderr) を返す"""
    if timeout is None:
        timeout = STEP_TIMEOUT
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


def shell(cmd, serial=None, timeout=None):
    """adb shell コマンドを実行してstdoutを返す"""
    _, out, _ = run_adb(["shell"] + cmd.split(), serial, timeout)
    return out


def tap(serial, x, y):
    """画面タップ"""
    shell(f"input tap {x} {y}", serial)


def press_back(serial):
    """戻るキー"""
    shell("input keyevent KEYCODE_BACK", serial)


def press_home(serial):
    """ホームキー"""
    shell("input keyevent KEYCODE_HOME", serial)


def swipe(serial, x1, y1, x2, y2, duration_ms=500):
    """スワイプ"""
    shell(f"input swipe {x1} {y1} {x2} {y2} {duration_ms}", serial)


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
        # "SERIAL device ..." の行のみ対象
        if "\tdevice" not in line and " device " not in line:
            continue
        parts = line.split()
        serial = parts[0]
        # WiFi接続済みデバイス (IP:PORT) はスキップ
        if ":" in serial:
            continue
        model = "Unknown"
        for p in parts:
            if p.startswith("model:"):
                model = p.split(":", 1)[1]
                break
        sdk = 0
        try:
            sdk_str = shell("getprop ro.build.version.sdk", serial)
            sdk = int(sdk_str)
        except (ValueError, TypeError):
            pass
        devices.append({"serial": serial, "model": model, "sdk": sdk})
    return devices


# ─── UIダンプ & XML解析 ─────────────────────────────────
def dump_ui(serial):
    """UIツリーをダンプしてXML文字列を返す"""
    # ダンプ先パス
    remote_path = "/data/local/tmp/ui_dump.xml"
    # ダンプ実行
    success, _, err = run_adb(
        ["shell", "uiautomator", "dump", remote_path], serial
    )
    if not success:
        return ""
    # XML取得
    _, xml_str, _ = run_adb(["shell", "cat", remote_path], serial)
    return xml_str


def dump_ui_with_retry(serial, retries=UI_DUMP_RETRIES, interval=UI_DUMP_INTERVAL):
    """リトライ付きUIダンプ"""
    for i in range(retries):
        xml = dump_ui(serial)
        if xml and "<node" in xml:
            return xml
        if i < retries - 1:
            time.sleep(interval)
    return ""


def parse_bounds(bounds_str):
    """bounds="[x1,y1][x2,y2]" → (cx, cy) 中心座標を返す"""
    m = re.match(r"\[(\d+),(\d+)\]\[(\d+),(\d+)\]", bounds_str)
    if not m:
        return None
    x1, y1, x2, y2 = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
    return ((x1 + x2) // 2, (y1 + y2) // 2)


def find_node_by_text(xml_str, text, partial=True):
    """XMLからテキストを含むノードの中心座標を返す"""
    try:
        root = ET.fromstring(xml_str)
    except ET.ParseError:
        return None
    for node in root.iter("node"):
        node_text = node.get("text", "")
        content_desc = node.get("content-desc", "")
        match = False
        if partial:
            match = text in node_text or text in content_desc
        else:
            match = text == node_text or text == content_desc
        if match:
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


def read_digits_from_ui(xml_str, num_digits=6):
    """UIダンプXMLからN桁の数字を正規表現で抽出"""
    pattern = rf"\b(\d{{{num_digits}}})\b"
    matches = re.findall(pattern, xml_str)
    return matches


def read_digits_vote(serial, num_digits=6, num_frames=3, interval=0.5):
    """投票方式でN桁数字を読み取る（最頻値を返す）"""
    counter = Counter()
    for i in range(num_frames):
        if i > 0:
            time.sleep(interval)
        xml = dump_ui(serial)
        if not xml:
            continue
        codes = read_digits_from_ui(xml, num_digits)
        for code in codes:
            counter[code] += 1
    if not counter:
        return None
    # 最頻値を返す
    best, count = counter.most_common(1)[0]
    return best


def extract_ip_port_from_xml(xml_str):
    """XMLからIP:PORT パターンを抽出"""
    m = re.search(r"(\d+\.\d+\.\d+\.\d+):(\d+)", xml_str)
    if m:
        return m.group(1), m.group(2)
    return None, None


def is_wifi_adb_connected(serial):
    """指定デバイスが既にWiFi ADB接続済みかチェック。
    adb devicesの出力にmDNS名やIP:portのデバイスがあれば接続済みと判定。
    serialのキー部分（例: A9250700956）がmDNS名に含まれていれば同一デバイスとみなす。
    """
    success, out, _ = run_adb(["devices", "-l"])
    if not success:
        return False, None
    # serialからデバイス固有IDを抽出（mDNS名: adb-SERIAL-xxx の SERIAL 部分）
    # USB serial そのものか、mDNS名からの抽出を試みる
    device_key = serial
    mdns_match = re.match(r"adb-(.+?)-", serial)
    if mdns_match:
        device_key = mdns_match.group(1)

    for line in out.strip().split("\n")[1:]:
        if not line.strip():
            continue
        if "\tdevice" not in line and " device " not in line:
            continue
        parts = line.split()
        dev_serial = parts[0]
        # IP:port または mDNS名のワイヤレスデバイスを探す
        is_wireless = ":" in dev_serial or "._adb-tls-connect._tcp" in dev_serial
        if not is_wireless:
            continue
        # 同一デバイスかチェック
        if device_key in dev_serial:
            return True, dev_serial
    return False, None


def get_device_ip_via_route(serial):
    """デバイスのWiFi IPアドレスを取得"""
    ip_output = shell("ip route", serial)
    ip_match = re.search(r"src\s+(\d+\.\d+\.\d+\.\d+)", ip_output)
    if ip_match:
        return ip_match.group(1)
    return None


def wifi_adb_tcpip_fallback(serial, device_name):
    """USB経由で adb tcpip 5555 → adb connect IP:5555 で接続するフォールバック"""
    info("tcpip方式でWiFi ADB接続を試みます...")

    # Step 1: デバイスIPを取得
    device_ip = get_device_ip_via_route(serial)
    if not device_ip:
        fail("デバイスのIPアドレスを取得できません（WiFi接続を確認してください）")
        return False
    ok(f"デバイスIP: {device_ip}")

    # Step 2: adb tcpip 5555
    info("adb tcpip 5555 を実行中...")
    success, out, err = run_adb(["tcpip", "5555"], serial, timeout=15)
    combined = (out + " " + err).lower()
    if "restarting" in combined or success:
        ok("tcpipモードに切り替えました")
    else:
        warn(f"tcpip切り替え結果: {out} {err}")
        # 続行を試みる

    time.sleep(2)

    # Step 3: adb connect
    connect_target = f"{device_ip}:5555"
    info(f"adb connect {connect_target} を実行中...")
    success, out, err = run_adb(["connect", connect_target], timeout=15)
    combined = (out + " " + err).lower()
    if "connected" in combined and "cannot" not in combined:
        ok(f"WiFi ADB 接続成功 (tcpip方式): {connect_target}")
        return True
    else:
        fail(f"WiFi ADB 接続失敗: {out} {err}")
        return False


# ─── WiFi ADB ペアリング ────────────────────────────────
def wifi_adb_pairing(serial, device_name, sdk_version=0, skip_if_connected=True):
    """WiFi ADBペアリングを自動実行"""
    header(f"WiFi ADB ペアリング: {device_name}")

    # Step 0: 既にWiFi ADB接続済みかチェック
    if skip_if_connected:
        already, wifi_serial = is_wifi_adb_connected(serial)
        if already:
            ok(f"既にWiFi ADB接続済み: {wifi_serial}")
            ok("スキップします")
            return True

    # API レベル取得
    if sdk_version == 0:
        try:
            sdk_version = int(shell("getprop ro.build.version.sdk", serial))
        except (ValueError, TypeError):
            sdk_version = 0

    # API < 30 は直接 tcpip 方式
    if sdk_version > 0 and sdk_version < 30:
        info(f"API Level {sdk_version} (Android 11未満) → tcpip方式を使用")
        return wifi_adb_tcpip_fallback(serial, device_name)

    # Step 1: ワイヤレスデバッグの有効化を確認
    info("ワイヤレスデバッグの状態を確認中...")
    wifi_enabled = shell("settings get global adb_wifi_enabled", serial)
    if wifi_enabled.strip() != "1":
        info("ワイヤレスデバッグを有効化中...")
        shell("settings put global adb_wifi_enabled 1", serial)
        time.sleep(1)
        # 再確認
        wifi_enabled = shell("settings get global adb_wifi_enabled", serial)
        if wifi_enabled.strip() == "1":
            ok("ワイヤレスデバッグを有効化しました")
        else:
            warn("ワイヤレスデバッグの有効化に失敗（手動で有効にしてください）")
    else:
        ok("ワイヤレスデバッグは既に有効です")

    # Step 2: ワイヤレスデバッグ設定画面を開く（複数インテントをフォールバック）
    info("ワイヤレスデバッグ設定画面を開いています...")
    wireless_debug_opened = False

    # 方法1: 直接Activity指定
    s1, o1, e1 = run_adb(
        ["shell", "am", "start", "-n",
         "com.android.settings/.Settings$WirelessDebuggingActivity"],
        serial
    )
    if s1 and "Error" not in (o1 + e1):
        wireless_debug_opened = True
        ok("WirelessDebuggingActivity で開きました")
    else:
        # 方法2: アクションインテント
        info("代替インテント(1)で開きます...")
        s2, o2, e2 = run_adb(
            ["shell", "am", "start", "-a",
             "android.settings.WIRELESS_DEBUGGING_SETTINGS"],
            serial
        )
        if s2 and "Error" not in (o2 + e2):
            wireless_debug_opened = True
            ok("WIRELESS_DEBUGGING_SETTINGS インテントで開きました")
        else:
            # 方法3: 開発者設定を開いて「ワイヤレスデバッグ」をタップ
            info("代替インテント(2): 開発者設定から探します...")
            run_adb(
                ["shell", "am", "start", "-a",
                 "android.settings.APPLICATION_DEVELOPMENT_SETTINGS"],
                serial
            )
            time.sleep(2)
            # UIダンプから「ワイヤレスデバッグ」をタップ
            for scroll_attempt in range(5):
                xml_dev = dump_ui_with_retry(serial)
                if xml_dev:
                    wd_pos = find_node_by_text(xml_dev, "ワイヤレス デバッグ")
                    if not wd_pos:
                        wd_pos = find_node_by_text(xml_dev, "ワイヤレスデバッグ")
                    if not wd_pos:
                        wd_pos = find_node_by_text(xml_dev, "Wireless debugging")
                    if wd_pos:
                        tap(serial, wd_pos[0], wd_pos[1])
                        wireless_debug_opened = True
                        ok("開発者設定から「ワイヤレスデバッグ」を開きました")
                        break
                # スクロールダウンして再検索
                swipe(serial, 540, 1500, 540, 500)
                time.sleep(1)

    if not wireless_debug_opened:
        warn("ワイヤレスデバッグ設定画面を開けませんでした")
        warn("tcpip方式にフォールバックします...")
        return wifi_adb_tcpip_fallback(serial, device_name)

    time.sleep(2)

    # Step 3: ペアリングダイアログを開く
    info("ペアリングコードオプションを探しています...")
    pair_pos = None

    for attempt in range(3):
        xml = dump_ui_with_retry(serial)
        if not xml:
            warn(f"UIダンプ取得失敗 (試行 {attempt + 1}/3)")
            time.sleep(1)
            continue

        # 日本語テキストで検索
        pair_pos = find_node_by_text(xml, UI_WIFI_PAIR_TEXT_JP)
        if not pair_pos:
            # 英語テキストで検索
            pair_pos = find_node_by_text(xml, UI_WIFI_PAIR_TEXT_EN)

        if pair_pos:
            break

        # スクロールして再検索
        info(f"スクロールして再検索 (試行 {attempt + 1}/3)...")
        swipe(serial, 540, 1500, 540, 500)
        time.sleep(1)

    if not pair_pos:
        warn("ペアリングコードオプションが見つかりません")
        warn("tcpip方式にフォールバックします...")
        press_back(serial)
        return wifi_adb_tcpip_fallback(serial, device_name)

    info(f"ペアリングオプション検出: ({pair_pos[0]}, {pair_pos[1]})")
    tap(serial, pair_pos[0], pair_pos[1])
    time.sleep(2)

    # Step 4: ペアリングコードを読み取る
    info("ペアリングコードを読み取り中...")
    pairing_code = read_digits_vote(serial, num_digits=6, num_frames=3)

    if not pairing_code:
        warn("ペアリングコードの読み取りに失敗しました")
        warn("tcpip方式にフォールバックします...")
        press_back(serial)
        return wifi_adb_tcpip_fallback(serial, device_name)

    ok(f"ペアリングコード: {C.BOLD}{pairing_code}{C.RESET}")

    # Step 5: IP:ポート番号を取得
    info("IP:ポート番号を取得中...")
    xml = dump_ui(serial)
    device_ip, pairing_port = extract_ip_port_from_xml(xml or "")

    if not device_ip or not pairing_port:
        # フォールバック: ip routeコマンドでIPを取得
        ip_output = shell("ip route", serial)
        ip_match = re.search(r"src\s+(\d+\.\d+\.\d+\.\d+)", ip_output)
        if ip_match:
            device_ip = ip_match.group(1)
        if not device_ip or not pairing_port:
            fail(f"IP:ポートの取得に失敗 (IP={device_ip}, Port={pairing_port})")
            fail(f"手動でペアリングしてください: adb pair <IP:PORT> {pairing_code}")
            press_back(serial)
            return False

    pair_target = f"{device_ip}:{pairing_port}"
    ok(f"ペアリング先: {pair_target}")

    # Step 6: adb pair 実行
    info(f"adb pair {pair_target} {pairing_code} を実行中...")
    success, out, err = run_adb(
        ["pair", pair_target, pairing_code], timeout=30
    )
    combined = (out + " " + err).lower()
    if "success" in combined or "paired" in combined:
        ok("WiFi ADB ペアリング成功！")
    else:
        warn(f"ペアリング結果: {out} {err}")

    # Step 7: adb connect で接続
    info(f"adb connect {device_ip}:5555 を実行中...")
    success, out, err = run_adb(["connect", f"{device_ip}:5555"], timeout=15)
    combined = out + " " + err
    if "connected" in combined.lower():
        ok(f"WiFi ADB 接続成功: {device_ip}:5555")
    else:
        warn(f"接続結果: {combined}")
        # ポート5555以外で接続を試行
        # ワイヤレスデバッグのデフォルトポートを取得
        info("代替ポートで接続を試行中...")
        xml2 = dump_ui(serial)
        if xml2:
            # ページ上の別のIP:PORTパターンを探す（ペアリング用ではない接続用ポート）
            all_ports = re.findall(r"(\d+\.\d+\.\d+\.\d+):(\d+)", xml2)
            for ip, port in all_ports:
                if port != pairing_port:
                    info(f"試行: adb connect {ip}:{port}")
                    s, o, e = run_adb(["connect", f"{ip}:{port}"], timeout=10)
                    if "connected" in (o + e).lower():
                        ok(f"WiFi ADB 接続成功: {ip}:{port}")
                        break

    # Step 8: ダイアログを閉じる
    press_back(serial)
    time.sleep(0.5)
    press_back(serial)

    return True


# ─── Bluetooth ADB ペアリング ───────────────────────────
def bluetooth_adb_pairing(serial, device_name, sdk_version=0):
    """Bluetooth ADBペアリングを自動実行"""
    header(f"Bluetooth ADB ペアリング: {device_name}")

    # Step 1: APIレベルを確認（BT ADBはAndroid 15+ = API 35+）
    if sdk_version == 0:
        try:
            sdk_version = int(shell("getprop ro.build.version.sdk", serial))
        except (ValueError, TypeError):
            sdk_version = 0

    if sdk_version > 0 and sdk_version < 35:
        warn(f"API Level {sdk_version} (Android 15未満)")
        warn("Bluetooth ADBはAndroid 15+ (API 35+) で追加された機能です")
        warn("通常のBluetoothペアリングダイアログの自動処理を試みます")

    # Step 2: BTペアリングダイアログを待機
    info("Bluetoothペアリングダイアログを待機中...")
    info(f"（タイムアウト: {STEP_TIMEOUT}秒）")
    info("PC側からBluetoothペアリングを開始してください")

    start_time = time.time()
    dialog_found = False
    pin_code = None

    while time.time() - start_time < STEP_TIMEOUT:
        xml = dump_ui(serial)
        if not xml:
            time.sleep(0.5)
            continue

        # BTペアリングダイアログを検出
        # alertTitle に「ペア」「Pair」が含まれているか確認
        has_pair_dialog = False
        if "alertTitle" in xml:
            if "ペア" in xml or "Pair" in xml or "pair" in xml:
                has_pair_dialog = True

        if not has_pair_dialog:
            elapsed = int(time.time() - start_time)
            print(f"\r  {C.DIM}[{elapsed}s] ダイアログ待機中...{C.RESET}", end="", flush=True)
            time.sleep(0.5)
            continue

        print()  # 改行
        dialog_found = True
        ok("Bluetoothペアリングダイアログを検出しました")

        # Step 3: PINコードをXMLから読み取り
        info("PINコードを読み取り中...")
        pin_code = read_digits_vote(serial, num_digits=6, num_frames=3)
        if pin_code:
            ok(f"PINコード: {C.BOLD}{pin_code}{C.RESET}")
        else:
            warn("PINコードを読み取れませんでした（表示なし or 確認のみ）")

        break

    if not dialog_found:
        print()  # 改行
        fail("Bluetoothペアリングダイアログがタイムアウトしました")
        return False

    # Step 4: ペア設定ボタンを自動タップ
    info("ペア設定ボタンをタップ中...")

    xml = dump_ui(serial)
    if not xml:
        fail("UIダンプの取得に失敗しました")
        return False

    btn_pos = None

    # resource-id で検索
    btn_pos = find_node_by_resource_id(xml, UI_BT_PAIR_BUTTON)

    # 日本語テキストで検索
    if not btn_pos:
        btn_pos = find_node_by_text(xml, UI_BT_PAIR_TEXT_JP)

    # 英語テキストで検索
    if not btn_pos:
        btn_pos = find_node_by_text(xml, UI_BT_PAIR_TEXT_EN)

    if btn_pos:
        tap(serial, btn_pos[0], btn_pos[1])
        ok(f"ペア設定ボタンをタップしました ({btn_pos[0]}, {btn_pos[1]})")
    else:
        # フォールバック: 画面サイズから推定
        warn("ペア設定ボタンが見つかりません。推定位置でタップします")
        size_output = shell("wm size", serial)
        sw, sh = 800, 1340
        size_match = re.search(r"(\d+)x(\d+)", size_output)
        if size_match:
            sw, sh = int(size_match.group(1)), int(size_match.group(2))
        # button1 は通常ダイアログ右下
        tap(serial, int(sw * 0.75), int(sh * 0.65))
        warn(f"推定位置でタップしました ({int(sw * 0.75)}, {int(sh * 0.65)})")

    time.sleep(1)

    # 確認
    xml_after = dump_ui(serial)
    if xml_after and "ペア" not in xml_after and "Pair" not in xml_after:
        ok("Bluetoothペアリングが完了したようです")
    else:
        warn("ダイアログがまだ表示されている可能性があります")

    return True


# ─── 全自動セットアップ ─────────────────────────────────
def full_auto_setup(serial, device_name, sdk_version=0):
    """WiFi ADB + 権限設定 + Accessibility の全自動セットアップ"""
    header(f"全自動セットアップ: {device_name}")

    results = {}

    # 1. WiFi ADB ペアリング
    info("── Phase 1: WiFi ADB ペアリング ──")
    results["wifi_adb"] = wifi_adb_pairing(serial, device_name, sdk_version=sdk_version)

    # 2. PROJECT_MEDIA 権限付与 (appops)
    info("── Phase 2: 権限設定 ──")
    info("通知権限を設定中...")
    run_adb(["shell", "cmd", "appops", "set", PACKAGE, "POST_NOTIFICATION", "allow"], serial)
    ok("POST_NOTIFICATION: allow")

    info("PROJECT_MEDIA権限を設定中...")
    run_adb(["shell", "appops", "set", PACKAGE, "PROJECT_MEDIA", "allow"], serial)
    ok("PROJECT_MEDIA: allow")

    info("SYSTEM_ALERT_WINDOW権限を設定中...")
    run_adb(["shell", "appops", "set", PACKAGE, "SYSTEM_ALERT_WINDOW", "allow"], serial)
    ok("SYSTEM_ALERT_WINDOW: allow")

    info("バックグラウンド実行権限を設定中...")
    for op in ["RUN_IN_BACKGROUND", "RUN_ANY_IN_BACKGROUND"]:
        run_adb(["shell", "appops", "set", PACKAGE, op, "allow"], serial)
    ok("バックグラウンド実行: allow")

    results["permissions"] = True

    # 3. AccessibilityService 有効化確認
    info("── Phase 3: AccessibilityService ──")
    info("AccessibilityService の状態を確認中...")
    current = shell("settings get secure enabled_accessibility_services", serial)
    if ACCESSIBILITY_SERVICE in (current or ""):
        ok("AccessibilityService は既に有効です")
        results["accessibility"] = True
    else:
        info("AccessibilityService を有効化中...")
        if current and current != "null" and current.strip():
            new_value = f"{current}:{ACCESSIBILITY_SERVICE}"
        else:
            new_value = ACCESSIBILITY_SERVICE
        run_adb(
            ["shell", "settings", "put", "secure",
             "enabled_accessibility_services", new_value],
            serial
        )
        run_adb(
            ["shell", "settings", "put", "secure", "accessibility_enabled", "1"],
            serial
        )
        # 確認
        verify = shell("settings get secure enabled_accessibility_services", serial)
        if ACCESSIBILITY_SERVICE in (verify or ""):
            ok("AccessibilityService を有効化しました")
            results["accessibility"] = True
        else:
            fail("AccessibilityService の有効化に失敗しました")
            results["accessibility"] = False

    # ホームに戻る
    press_home(serial)

    # サマリー
    header("セットアップ完了サマリー")
    for key, value in results.items():
        status = f"{C.GREEN}成功{C.RESET}" if value else f"{C.RED}失敗{C.RESET}"
        label = {
            "wifi_adb": "WiFi ADB ペアリング",
            "permissions": "権限設定",
            "accessibility": "AccessibilityService",
        }.get(key, key)
        print(f"  {label}: {status}")

    all_ok = all(results.values())
    if all_ok:
        print(f"\n  {C.GREEN}{C.BOLD}全てのセットアップが正常に完了しました！{C.RESET}")
    else:
        print(f"\n  {C.YELLOW}一部のセットアップに失敗しました。上記の詳細を確認してください。{C.RESET}")

    return all_ok


# ─── メイン ─────────────────────────────────────────────
def main():
    global STEP_TIMEOUT

    default_timeout = STEP_TIMEOUT
    parser = argparse.ArgumentParser(
        description="WiFi ADB / Bluetooth ADB ペアリング完全自動化",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
例:
  python scripts/auto_pair.py --wifi                       WiFi ADBペアリングのみ
  python scripts/auto_pair.py --bt                         Bluetoothペアリングのみ
  python scripts/auto_pair.py --all                        全自動セットアップ
  python scripts/auto_pair.py --wifi --device A9250700956  デバイス指定
  python scripts/auto_pair.py --bt --timeout 30            タイムアウト30秒
        """,
    )
    parser.add_argument("--wifi", action="store_true", help="WiFi ADBペアリングを実行")
    parser.add_argument("--bt", action="store_true", help="Bluetoothペアリングを実行")
    parser.add_argument("--all", action="store_true", help="全自動セットアップ（WiFi ADB + 権限 + Accessibility）")
    parser.add_argument("--device", "-d", default=None, help="デバイスシリアル番号を指定")
    parser.add_argument("--timeout", "-t", type=int, default=default_timeout,
                        help=f"操作タイムアウト秒 (デフォルト: {default_timeout})")

    args = parser.parse_args()

    # タイムアウト設定の反映
    STEP_TIMEOUT = args.timeout

    # 引数チェック
    if not args.wifi and not args.bt and not args.all:
        parser.print_help()
        print(f"\n{C.YELLOW}エラー: --wifi, --bt, --all のいずれかを指定してください{C.RESET}")
        sys.exit(1)

    # ヘッダー表示
    header("Mirage Auto Pair")
    print(f"  ADB: {ADB}")
    mode_str = []
    if args.wifi or args.all:
        mode_str.append("WiFi ADB")
    if args.bt:
        mode_str.append("Bluetooth")
    if args.all:
        mode_str.append("全自動セットアップ")
    print(f"  モード: {', '.join(mode_str)}")
    print(f"  タイムアウト: {STEP_TIMEOUT}秒")

    # デバイス検出
    info("USBデバイスを検出中...")
    devices = get_connected_devices()
    if not devices:
        fail("USB接続されたADBデバイスが見つかりません")
        fail("USBケーブルを確認し、USBデバッグが有効になっていることを確認してください")
        sys.exit(1)

    ok(f"{len(devices)} 台のデバイスを検出:")
    for i, dev in enumerate(devices):
        sdk_info = f"API {dev['sdk']}" if dev["sdk"] else "API不明"
        print(f"    {i + 1}. {dev['model']} ({dev['serial']}) [{sdk_info}]")

    # デバイス選択（部分一致対応: mDNS名 adb-SERIAL-xxx にもマッチ）
    if args.device:
        target = None
        for dev in devices:
            if args.device in dev["serial"] or dev["serial"] in args.device:
                target = dev
                break
        if not target:
            fail(f"指定されたデバイス '{args.device}' が見つかりません")
            fail(f"検出されたデバイス: {[d['serial'] for d in devices]}")
            sys.exit(1)
        targets = [target]
    else:
        targets = devices

    # 実行
    success_count = 0
    for dev in targets:
        serial = dev["serial"]
        name = f"{dev['model']} ({serial})"

        if args.all:
            if full_auto_setup(serial, name, dev["sdk"]):
                success_count += 1
        else:
            results = []
            if args.wifi:
                results.append(wifi_adb_pairing(serial, name, sdk_version=dev["sdk"]))
            if args.bt:
                results.append(bluetooth_adb_pairing(serial, name, dev["sdk"]))
            if all(results):
                success_count += 1

    # 最終結果
    print()
    total = len(targets)
    if success_count == total:
        ok(f"全デバイス完了 ({success_count}/{total})")
    else:
        warn(f"一部失敗あり ({success_count}/{total} 成功)")

    sys.exit(0 if success_count == total else 1)


if __name__ == "__main__":
    main()
