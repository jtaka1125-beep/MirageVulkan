#!/usr/bin/env python3
"""
E2Eミラーリングパイプラインテスト

映像パイプラインが通るかを確認するための軽量テストスクリプト。
ADBデバイス検出 → 権限付与 → CaptureActivity起動 → UDP受信 → RTP解析 → FFmpegデコード → PNG保存

※ scripts/hybrid_video_viewer.py (リアルタイム表示用フルGUI) とは別。
  本スクリプトは「映像が端末からPCに届くか」を確認するワンショットテスト。

使い方:
    python scripts/e2e_mirror_test.py
    python scripts/e2e_mirror_test.py --port 50000 --timeout 15
    python scripts/e2e_mirror_test.py --device SERIAL --output frame.png
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


# === 定数 ===
DEFAULT_PORT = 50000                # 映像受信UDPポート
DEFAULT_TIMEOUT = 20                # 受信タイムアウト（秒）
DEFAULT_OUTPUT = "e2e_first_frame.png"  # デコード結果の出力ファイル名
RECV_BUFFER_SIZE = 65536            # UDP受信バッファサイズ
RTP_HEADER_MIN = 12                 # RTPヘッダー最小サイズ
CAPTURE_PACKAGE = "com.mirage.capture"
CAPTURE_ACTIVITY = "com.mirage.capture/.ui.CaptureActivity"


# ============================================================================
# ADBデバイス検出
# ============================================================================
def find_device(preferred_serial=None):
    """
    ADBで接続中のデバイスを検出してシリアル番号を返す。
    USB接続・WiFi接続(IP:port形式)の両方に対応。

    Args:
        preferred_serial: 指定があればそのデバイスを優先して返す
    Returns:
        デバイスのシリアル番号文字列
    Raises:
        RuntimeError: デバイスが見つからない場合
    """
    result = subprocess.run(
        ["adb", "devices"], capture_output=True, text=True, timeout=10
    )
    if result.returncode != 0:
        raise RuntimeError(f"adb devices 実行失敗: {result.stderr.strip()}")

    devices = []
    for line in result.stdout.strip().splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            devices.append(parts[0])

    if not devices:
        raise RuntimeError(
            "ADBデバイスが見つかりません。\n"
            "  - USBケーブルを確認してください\n"
            "  - adb devicesでデバイスが表示されるか確認してください\n"
            "  - WiFi接続の場合: adb connect <IP>:5555 を実行してください"
        )

    # 指定シリアルがあればそれを優先
    if preferred_serial:
        if preferred_serial in devices:
            return preferred_serial
        raise RuntimeError(
            f"指定デバイス '{preferred_serial}' が見つかりません。\n"
            f"  接続中のデバイス: {devices}"
        )

    # 複数台ある場合は警告して最初のデバイスを使用
    if len(devices) > 1:
        print(f"  [警告] 複数デバイス検出: {devices}")
        print(f"  [警告] 最初のデバイス '{devices[0]}' を使用します")
        print(f"  [警告] --device で指定することもできます")

    return devices[0]


# ============================================================================
# PCのIPアドレス自動取得
# ============================================================================
def get_pc_ip():
    """
    PCのIPアドレスを自動取得する。
    UDPソケットを使った接続先推定法で、実際のネットワークインターフェースのIPを取得。

    Returns:
        IPアドレス文字列 (例: "192.168.0.8")
    Raises:
        RuntimeError: IP取得に失敗した場合
    """
    # 方法1: UDPソケットで外部接続を試みてローカルIPを取得
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.settimeout(1)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        if ip and ip != "0.0.0.0":
            return ip
    except Exception:
        pass

    # 方法2: Windowsの場合ipconfig、それ以外はifconfigから取得
    if platform.system() == "Windows":
        try:
            result = subprocess.run(
                ["ipconfig"], capture_output=True, text=True, timeout=5
            )
            for line in result.stdout.splitlines():
                if "IPv4" in line:
                    match = re.search(r"(\d+\.\d+\.\d+\.\d+)", line)
                    if match:
                        return match.group(1)
        except Exception:
            pass
    else:
        try:
            result = subprocess.run(
                ["hostname", "-I"], capture_output=True, text=True, timeout=5
            )
            ips = result.stdout.strip().split()
            if ips:
                return ips[0]
        except Exception:
            pass

    raise RuntimeError(
        "PCのIPアドレスを自動取得できませんでした。\n"
        "  --host で手動指定してください"
    )


# ============================================================================
# appops PROJECT_MEDIA 権限付与
# ============================================================================
def grant_projection(serial):
    """
    appopsコマンドでMediaProjection(PROJECT_MEDIA)を許可する。

    Args:
        serial: ADBデバイスのシリアル番号
    """
    cmd = [
        "adb", "-s", serial,
        "shell", "appops", "set", CAPTURE_PACKAGE, "PROJECT_MEDIA", "allow"
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        print(f"  [警告] appops設定に失敗: {result.stderr.strip()}")
        print("  [警告] 手動でMediaProjectionダイアログを承認してください")
    else:
        print("  [OK] PROJECT_MEDIA 権限を付与しました")


# ============================================================================
# CaptureActivityをauto_mirrorインテントで起動
# ============================================================================
def start_capture(serial, host, port):
    """
    ADB経由でCaptureActivityをauto_mirrorモードで起動する。

    Args:
        serial: ADBデバイスのシリアル番号
        host: ミラーリング先PCのIPアドレス
        port: ミラーリング先のUDPポート番号
    """
    cmd = [
        "adb", "-s", serial,
        "shell", "am", "start",
        "-n", CAPTURE_ACTIVITY,
        "--ez", "auto_mirror", "true",
        "--es", "mirror_host", host,
        "--ei", "mirror_port", str(port),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
    if result.returncode != 0:
        raise RuntimeError(
            f"CaptureActivity起動に失敗: {result.stderr.strip()}\n"
            f"  - APKがインストールされているか確認してください\n"
            f"  - パッケージ名: {CAPTURE_PACKAGE}"
        )
    print(f"  [OK] CaptureActivity起動 (host={host}, port={port})")


# ============================================================================
# RTPヘッダー解析
# ============================================================================
def parse_rtp_header(data):
    """
    RTPヘッダーを解析する。

    Args:
        data: 受信パケットデータ (bytes)
    Returns:
        dict: {version, pt, marker, seq, timestamp, ssrc, payload_offset}
        None: RTPパケットでない場合
    """
    if len(data) < RTP_HEADER_MIN:
        return None

    first_byte = data[0]
    version = (first_byte >> 6) & 0x03
    if version != 2:
        return None

    padding = (first_byte >> 5) & 0x01
    extension = (first_byte >> 4) & 0x01
    cc = first_byte & 0x0F

    second_byte = data[1]
    marker = (second_byte >> 7) & 0x01
    pt = second_byte & 0x7F

    seq = struct.unpack("!H", data[2:4])[0]
    timestamp = struct.unpack("!I", data[4:8])[0]
    ssrc = struct.unpack("!I", data[8:12])[0]

    # CSRCリスト分のオフセット
    payload_offset = 12 + cc * 4

    # 拡張ヘッダーがある場合はスキップ
    if extension and len(data) > payload_offset + 4:
        ext_len = struct.unpack("!H", data[payload_offset + 2:payload_offset + 4])[0]
        payload_offset += 4 + ext_len * 4

    return {
        "version": version,
        "pt": pt,
        "marker": marker,
        "seq": seq,
        "timestamp": timestamp,
        "ssrc": ssrc,
        "payload_offset": payload_offset,
    }


# ============================================================================
# H.264 NALユニット抽出 (RTPペイロードから)
# ============================================================================
def extract_h264_nals(packets):
    """
    受信RTPパケット群からH.264 NALユニットを抽出する。
    シングルNAL、FU-A断片化の両方に対応。

    Args:
        packets: (data, rtp_info) のリスト
    Returns:
        H.264 Annex-Bストリーム (bytes)
    """
    h264_stream = bytearray()
    fua_buffer = bytearray()
    fua_nal_header = 0

    for data, rtp in packets:
        offset = rtp["payload_offset"]
        if offset >= len(data):
            continue

        payload = data[offset:]
        if len(payload) < 1:
            continue

        nal_type = payload[0] & 0x1F
        nri = payload[0] & 0x60

        if nal_type <= 23:
            # シングルNALユニット
            h264_stream.extend(b'\x00\x00\x00\x01')
            h264_stream.extend(payload)

        elif nal_type == 28:
            # FU-A 断片化ユニット
            if len(payload) < 2:
                continue
            fu_indicator = payload[0]
            fu_header = payload[1]
            start = (fu_header >> 7) & 0x01
            end = (fu_header >> 6) & 0x01
            frag_nal_type = fu_header & 0x1F

            if start:
                fua_nal_header = (fu_indicator & 0xE0) | frag_nal_type
                fua_buffer = bytearray([fua_nal_header])
                fua_buffer.extend(payload[2:])
            elif fua_buffer:
                fua_buffer.extend(payload[2:])

            if end and fua_buffer:
                h264_stream.extend(b'\x00\x00\x00\x01')
                h264_stream.extend(fua_buffer)
                fua_buffer = bytearray()

    return bytes(h264_stream)


# ============================================================================
# UDP受信 → デコード → PNG保存
# ============================================================================
def receive_and_decode(port, timeout, output_png):
    """
    UDPでRTP/H.264パケットを受信し、FFmpegでデコードして最初のフレームをPNG保存する。

    Args:
        port: 受信UDPポート番号
        timeout: 受信タイムアウト（秒）
        output_png: 出力PNGファイルパス
    Returns:
        dict: テスト結果サマリー
    """
    running = True

    def on_sigint(signum, frame):
        nonlocal running
        running = False

    old_handler = signal.signal(signal.SIGINT, on_sigint)

    # UDPソケット作成
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(1.0)

    print(f"  UDP受信開始: ポート {port}, タイムアウト {timeout}秒")

    # 統計情報
    stats = {
        "total_packets": 0,
        "total_bytes": 0,
        "rtp_packets": 0,
        "non_rtp_packets": 0,
        "first_rtp_seq": None,
        "last_rtp_seq": None,
        "rtp_seq_gaps": 0,
        "start_time": None,
        "end_time": None,
        "decode_ok": False,
        "output_file": None,
    }

    rtp_packet_list = []
    prev_seq = None
    start_time = time.time()

    try:
        while running and (time.time() - start_time) < timeout:
            try:
                data, addr = sock.recvfrom(RECV_BUFFER_SIZE)
            except socket.timeout:
                elapsed = time.time() - start_time
                if stats["total_packets"] == 0:
                    print(f"\r  パケット待機中... ({elapsed:.0f}s / {timeout}s)   ",
                          end="", flush=True)
                continue

            if stats["start_time"] is None:
                stats["start_time"] = time.time()

            stats["total_packets"] += 1
            stats["total_bytes"] += len(data)

            # RTPヘッダー解析
            rtp = parse_rtp_header(data)
            if rtp:
                stats["rtp_packets"] += 1
                rtp_packet_list.append((data, rtp))

                if stats["first_rtp_seq"] is None:
                    stats["first_rtp_seq"] = rtp["seq"]
                stats["last_rtp_seq"] = rtp["seq"]

                # シーケンスギャップ検出
                if prev_seq is not None:
                    expected = (prev_seq + 1) & 0xFFFF
                    if rtp["seq"] != expected:
                        stats["rtp_seq_gaps"] += 1
                prev_seq = rtp["seq"]

                # 最初の数パケットは詳細表示
                if stats["rtp_packets"] <= 5:
                    print(f"  [RTP#{stats['rtp_packets']}] "
                          f"seq={rtp['seq']} ts={rtp['timestamp']} "
                          f"pt={rtp['pt']} marker={rtp['marker']} "
                          f"size={len(data)}")
            else:
                stats["non_rtp_packets"] += 1
                if stats["non_rtp_packets"] <= 3:
                    print(f"  [非RTP] size={len(data)} head={data[:8].hex()}")

            # ビットレート表示（1秒ごと）
            elapsed = time.time() - start_time
            if stats["total_packets"] % 100 == 0:
                bps = stats["total_bytes"] * 8 / elapsed if elapsed > 0 else 0
                if bps >= 1_000_000:
                    rate_str = f"{bps / 1_000_000:.2f} Mbps"
                elif bps >= 1_000:
                    rate_str = f"{bps / 1_000:.1f} kbps"
                else:
                    rate_str = f"{bps:.0f} bps"
                print(f"\r  [{elapsed:.1f}s] {stats['total_packets']} pkts, "
                      f"{rate_str}   ", end="", flush=True)

            # マーカービット=1のRTPパケットを受信したら最低1フレーム受信完了
            # ある程度パケットが溜まったらデコード試行
            if rtp and rtp["marker"] == 1 and stats["rtp_packets"] >= 10:
                print(f"\n  [INFO] マーカービット検出 (seq={rtp['seq']}) — "
                      f"デコード試行開始")
                break

    finally:
        sock.close()
        signal.signal(signal.SIGINT, old_handler)

    stats["end_time"] = time.time()
    print()  # 改行

    if not rtp_packet_list:
        print("  [エラー] RTPパケットを1つも受信できませんでした")
        return stats

    # H.264ストリーム抽出
    print(f"  H.264 NAL抽出中... ({len(rtp_packet_list)} RTPパケットから)")
    h264_data = extract_h264_nals(rtp_packet_list)

    if not h264_data:
        print("  [エラー] H.264データを抽出できませんでした")
        return stats

    print(f"  H.264ストリーム: {len(h264_data)} バイト")

    # FFmpegでデコード → PNG保存
    print(f"  FFmpegでデコード中...")
    try:
        ffmpeg_cmd = [
            "ffmpeg",
            "-y",                       # 上書き許可
            "-f", "h264",               # 入力フォーマット: raw H.264
            "-i", "pipe:0",             # 標準入力から読み取り
            "-frames:v", "1",           # 最初の1フレームのみ
            "-pix_fmt", "rgb24",        # ピクセルフォーマット
            output_png
        ]
        proc = subprocess.run(
            ffmpeg_cmd,
            input=h264_data,
            capture_output=True,
            timeout=15,
        )
        if proc.returncode == 0 and os.path.exists(output_png):
            file_size = os.path.getsize(output_png)
            stats["decode_ok"] = True
            stats["output_file"] = output_png
            print(f"  [OK] デコード成功: {output_png} ({file_size} バイト)")
        else:
            print(f"  [エラー] FFmpegデコード失敗 (rc={proc.returncode})")
            if proc.stderr:
                stderr_text = proc.stderr.decode("utf-8", errors="replace")
                # 最後の5行だけ表示
                for line in stderr_text.strip().splitlines()[-5:]:
                    print(f"    {line}")
    except FileNotFoundError:
        print("  [エラー] ffmpegが見つかりません。PATHにffmpegを追加してください")
    except subprocess.TimeoutExpired:
        print("  [エラー] FFmpegデコードがタイムアウトしました")

    return stats


# ============================================================================
# 結果サマリー表示
# ============================================================================
def print_summary(stats):
    """テスト結果のサマリーを表示する"""
    elapsed = 0
    if stats["start_time"] and stats["end_time"]:
        elapsed = stats["end_time"] - stats["start_time"]

    avg_bps = (stats["total_bytes"] * 8 / elapsed) if elapsed > 0 else 0
    if avg_bps >= 1_000_000:
        rate_str = f"{avg_bps / 1_000_000:.2f} Mbps"
    elif avg_bps >= 1_000:
        rate_str = f"{avg_bps / 1_000:.1f} kbps"
    else:
        rate_str = f"{avg_bps:.0f} bps"

    print()
    print("=" * 60)
    print(" E2Eミラーリングテスト結果")
    print("=" * 60)
    print(f"  総パケット数     : {stats['total_packets']}")
    print(f"  総受信バイト     : {stats['total_bytes']:,} B")
    print(f"  受信時間         : {elapsed:.1f} 秒")
    print(f"  平均ビットレート : {rate_str}")
    print(f"  RTPパケット      : {stats['rtp_packets']}")
    print(f"  非RTPパケット    : {stats['non_rtp_packets']}")
    if stats["first_rtp_seq"] is not None:
        print(f"  RTP SEQ範囲      : {stats['first_rtp_seq']} → {stats['last_rtp_seq']}")
        print(f"  RTP SEQギャップ  : {stats['rtp_seq_gaps']}")
    print(f"  デコード         : {'成功' if stats['decode_ok'] else '失敗'}")
    if stats["output_file"]:
        print(f"  出力ファイル     : {stats['output_file']}")

    # 合否判定
    print()
    if stats["rtp_packets"] > 0 and stats["decode_ok"]:
        print("  >>> テスト結果: PASS — 映像パイプラインは正常に動作しています <<<")
    elif stats["rtp_packets"] > 0:
        print("  >>> テスト結果: PARTIAL — RTP受信OK / デコード失敗 <<<")
        print("  ffmpegがインストールされているか確認してください")
    else:
        print("  >>> テスト結果: FAIL — 映像パケットを受信できませんでした <<<")
        print("  以下を確認してください:")
        print("    - Android端末でMediaProjectionが許可されているか")
        print("    - ファイアウォールでUDPポートが開いているか")
        print("    - PCとAndroid端末が同じネットワーク上にあるか")

    print("=" * 60)


# ============================================================================
# メインエントリポイント
# ============================================================================
def main():
    parser = argparse.ArgumentParser(
        description="E2Eミラーリングパイプラインテスト — ADB→CaptureActivity→UDP→RTP→H.264→PNG"
    )
    parser.add_argument(
        "--port", type=int, default=DEFAULT_PORT,
        help=f"受信UDPポート番号 (デフォルト: {DEFAULT_PORT})"
    )
    parser.add_argument(
        "--timeout", type=float, default=DEFAULT_TIMEOUT,
        help=f"受信タイムアウト秒数 (デフォルト: {DEFAULT_TIMEOUT})"
    )
    parser.add_argument(
        "--output", type=str, default=DEFAULT_OUTPUT,
        help=f"デコード結果PNGファイルパス (デフォルト: {DEFAULT_OUTPUT})"
    )
    parser.add_argument(
        "--device", type=str, default=None,
        help="ADBデバイスのシリアル番号 (省略時は自動検出)"
    )
    parser.add_argument(
        "--host", type=str, default=None,
        help="PCのIPアドレス (省略時は自動検出)"
    )
    parser.add_argument(
        "--skip-launch", action="store_true",
        help="CaptureActivity起動をスキップ (既に起動済みの場合)"
    )
    args = parser.parse_args()

    print("=" * 60)
    print(" MirageComplete E2Eミラーリングテスト")
    print("=" * 60)
    print()

    # Step 1: デバイス検出
    print("[1/5] ADBデバイス検出...")
    try:
        serial = find_device(args.device)
        # USB or WiFi判定
        conn_type = "WiFi" if ":" in serial else "USB"
        print(f"  [OK] デバイス: {serial} ({conn_type})")
    except RuntimeError as e:
        print(f"  [エラー] {e}")
        sys.exit(1)

    # Step 2: PCのIPアドレス取得
    print()
    print("[2/5] PCのIPアドレス取得...")
    try:
        pc_ip = args.host or get_pc_ip()
        print(f"  [OK] PC IP: {pc_ip}")
    except RuntimeError as e:
        print(f"  [エラー] {e}")
        sys.exit(1)

    # Step 3: 権限付与
    print()
    print("[3/5] PROJECT_MEDIA権限付与...")
    grant_projection(serial)

    # Step 4: CaptureActivity起動
    print()
    if args.skip_launch:
        print("[4/5] CaptureActivity起動をスキップ (--skip-launch)")
    else:
        print("[4/5] CaptureActivity起動...")
        try:
            start_capture(serial, pc_ip, args.port)
        except RuntimeError as e:
            print(f"  [エラー] {e}")
            sys.exit(1)
        # MediaProjectionダイアログの処理を待つ
        print("  3秒待機中 (MediaProjectionダイアログ処理待ち)...")
        time.sleep(3)

    # Step 5: UDP受信 → デコード → PNG保存
    print()
    print("[5/5] UDP受信 → RTP解析 → H.264デコード...")
    stats = receive_and_decode(args.port, args.timeout, args.output)

    # 結果サマリー
    print_summary(stats)

    # 終了コード
    if stats["decode_ok"]:
        sys.exit(0)
    elif stats["rtp_packets"] > 0:
        sys.exit(2)  # 部分成功
    else:
        sys.exit(1)  # 失敗


if __name__ == "__main__":
    main()
