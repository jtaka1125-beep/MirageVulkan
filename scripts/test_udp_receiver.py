#!/usr/bin/env python3
"""
UDP映像受信テストスクリプト

H.264パケットをUDP経由で受信し、統計情報をリアルタイム表示する。
RTPヘッダー解析、VID0マジック検出に対応。
"""

import argparse
import socket
import struct
import sys
import signal
import time


# === 定数 ===
VID0_MAGIC = 0x56494430          # "VID0" リトルエンディアン
RTP_HEADER_MIN_SIZE = 12         # RTPヘッダーの最小サイズ（バイト）
HEXDUMP_MAX_BYTES = 64           # hexdump表示の最大バイト数
STATS_INTERVAL = 1.0             # 統計表示間隔（秒）
RECV_BUFFER_SIZE = 65536         # UDP受信バッファサイズ


class ReceiverStats:
    """受信統計を管理するクラス"""

    def __init__(self):
        self.total_packets = 0
        self.total_bytes = 0
        self.vid0_packets = 0
        self.rtp_packets = 0
        self.unknown_packets = 0
        self.first_packet_data = None  # 先頭パケットを保存
        self.start_time = None

        # 区間統計（1秒ごとのビットレート計算用）
        self._interval_bytes = 0
        self._interval_packets = 0
        self._interval_start = None

    def record_packet(self, data: bytes, pkt_type: str):
        """パケット受信を記録する"""
        now = time.time()

        if self.start_time is None:
            self.start_time = now
            self._interval_start = now

        self.total_packets += 1
        self.total_bytes += len(data)
        self._interval_bytes += len(data)
        self._interval_packets += 1

        if pkt_type == "vid0":
            self.vid0_packets += 1
        elif pkt_type == "rtp":
            self.rtp_packets += 1
        else:
            self.unknown_packets += 1

        # 先頭パケットを保存
        if self.first_packet_data is None:
            self.first_packet_data = data

    def get_interval_stats(self):
        """区間統計を取得しリセットする。(ビットレートbps, パケット数)を返す"""
        now = time.time()
        elapsed = now - self._interval_start if self._interval_start else 0

        if elapsed > 0:
            bitrate = (self._interval_bytes * 8) / elapsed
            pkt_count = self._interval_packets
        else:
            bitrate = 0
            pkt_count = 0

        # リセット
        self._interval_bytes = 0
        self._interval_packets = 0
        self._interval_start = now

        return bitrate, pkt_count

    def get_elapsed(self):
        """経過時間（秒）を返す"""
        if self.start_time is None:
            return 0
        return time.time() - self.start_time


def format_bitrate(bps: float) -> str:
    """ビットレートを人間が読みやすい形式に変換する"""
    if bps >= 1_000_000:
        return f"{bps / 1_000_000:.2f} Mbps"
    elif bps >= 1_000:
        return f"{bps / 1_000:.1f} kbps"
    else:
        return f"{bps:.0f} bps"


def format_bytes(b: int) -> str:
    """バイト数を人間が読みやすい形式に変換する"""
    if b >= 1_048_576:
        return f"{b / 1_048_576:.2f} MB"
    elif b >= 1024:
        return f"{b / 1024:.1f} KB"
    else:
        return f"{b} B"


def hexdump(data: bytes, max_bytes: int = HEXDUMP_MAX_BYTES) -> str:
    """バイト列をhexdump形式の文字列に変換する"""
    lines = []
    truncated = data[:max_bytes]
    for offset in range(0, len(truncated), 16):
        chunk = truncated[offset:offset + 16]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 0x20 <= b < 0x7f else "." for b in chunk)
        lines.append(f"  {offset:04x}: {hex_part:<48s}  {ascii_part}")
    if len(data) > max_bytes:
        lines.append(f"  ... ({len(data) - max_bytes} バイト省略)")
    return "\n".join(lines)


def parse_rtp_header(data: bytes):
    """
    RTPヘッダーを解析する。
    成功時: (sequence_number, timestamp, payload_type) を返す
    失敗時: None を返す
    """
    if len(data) < RTP_HEADER_MIN_SIZE:
        return None

    # RTPヘッダー先頭バイト: V=2, P, X, CC
    first_byte = data[0]
    version = (first_byte >> 6) & 0x03
    if version != 2:
        return None

    # 2バイト目: M, PT
    second_byte = data[1]
    payload_type = second_byte & 0x7F
    marker = (second_byte >> 7) & 0x01

    # シーケンス番号（2バイト、ビッグエンディアン）
    seq_num = struct.unpack("!H", data[2:4])[0]

    # タイムスタンプ（4バイト、ビッグエンディアン）
    timestamp = struct.unpack("!I", data[4:8])[0]

    return seq_num, timestamp, payload_type, marker


def detect_vid0_magic(data: bytes) -> bool:
    """VID0マジック（0x56494430）がパケット先頭にあるか判定する"""
    if len(data) < 4:
        return False
    magic = struct.unpack(">I", data[0:4])[0]
    return magic == VID0_MAGIC


def classify_packet(data: bytes) -> str:
    """パケットの種類を判定する: 'vid0', 'rtp', 'unknown'"""
    if detect_vid0_magic(data):
        return "vid0"
    if parse_rtp_header(data) is not None:
        return "rtp"
    return "unknown"


def print_packet_info(data: bytes, pkt_type: str, pkt_num: int):
    """パケットの詳細情報を表示する（最初の数パケットのみ）"""
    if pkt_num > 5:
        return

    size = len(data)
    if pkt_type == "vid0":
        # VID0ヘッダー: 4バイトマジック + 4バイト長
        payload_len = struct.unpack(">I", data[4:8])[0] if size >= 8 else 0
        print(f"  [PKT#{pkt_num}] VID0  size={size}  payload_len={payload_len}")
    elif pkt_type == "rtp":
        rtp = parse_rtp_header(data)
        if rtp:
            seq, ts, pt, marker = rtp
            print(f"  [PKT#{pkt_num}] RTP   size={size}  seq={seq}  ts={ts}  "
                  f"pt={pt}  marker={marker}")
    else:
        print(f"  [PKT#{pkt_num}] ???   size={size}  head={data[:4].hex()}")


def print_final_stats(stats: ReceiverStats):
    """最終統計を表示する"""
    elapsed = stats.get_elapsed()
    avg_bitrate = (stats.total_bytes * 8 / elapsed) if elapsed > 0 else 0

    print("\n" + "=" * 60)
    print("受信統計サマリー")
    print("=" * 60)
    print(f"  総パケット数   : {stats.total_packets}")
    print(f"  総受信バイト   : {format_bytes(stats.total_bytes)}")
    print(f"  経過時間       : {elapsed:.1f} 秒")
    print(f"  平均ビットレート: {format_bitrate(avg_bitrate)}")
    print(f"  VID0パケット   : {stats.vid0_packets}")
    print(f"  RTPパケット    : {stats.rtp_packets}")
    print(f"  不明パケット   : {stats.unknown_packets}")

    # 先頭パケットのhexdump
    if stats.first_packet_data:
        print(f"\n先頭パケット hexdump ({len(stats.first_packet_data)} バイト):")
        print(hexdump(stats.first_packet_data))

    print("=" * 60)


def run_receiver(port: int, timeout: float):
    """メインの受信ループ"""
    stats = ReceiverStats()
    running = True

    def on_sigint(signum, frame):
        nonlocal running
        running = False
        print("\n\n停止シグナルを受信しました...")

    signal.signal(signal.SIGINT, on_sigint)

    # UDPソケットを作成
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", port))
    sock.settimeout(1.0)  # 1秒ごとにタイムアウトして統計表示

    print(f"UDP受信テスト開始: ポート {port}")
    print(f"タイムアウト: {timeout:.0f} 秒")
    print(f"停止: Ctrl+C")
    print("-" * 60)

    last_stats_time = time.time()
    start_time = time.time()

    try:
        while running:
            # タイムアウト判定
            if time.time() - start_time >= timeout:
                print(f"\n{timeout:.0f}秒タイムアウトにより自動終了します。")
                break

            # パケット受信
            try:
                data, addr = sock.recvfrom(RECV_BUFFER_SIZE)
            except socket.timeout:
                # 受信タイムアウト → 統計表示のみ
                now = time.time()
                if now - last_stats_time >= STATS_INTERVAL and stats.total_packets > 0:
                    bitrate, pkt_count = stats.get_interval_stats()
                    elapsed = now - start_time
                    remaining = timeout - elapsed
                    print(f"\r  [{elapsed:5.1f}s] 累計: {stats.total_packets} pkts, "
                          f"{format_bytes(stats.total_bytes)} | "
                          f"現在: {format_bitrate(bitrate)}, {pkt_count} pkts/s | "
                          f"残り {remaining:.0f}s   ", end="", flush=True)
                    last_stats_time = now
                elif stats.total_packets == 0:
                    elapsed = now - start_time
                    remaining = timeout - elapsed
                    print(f"\r  パケット待機中... ({elapsed:.0f}s / {timeout:.0f}s)   ",
                          end="", flush=True)
                continue

            # パケット分類と記録
            pkt_type = classify_packet(data)
            stats.record_packet(data, pkt_type)

            # 最初の数パケットは詳細表示
            print_packet_info(data, pkt_type, stats.total_packets)

            # 統計表示（1秒ごと）
            now = time.time()
            if now - last_stats_time >= STATS_INTERVAL:
                bitrate, pkt_count = stats.get_interval_stats()
                elapsed = now - start_time
                remaining = timeout - elapsed
                print(f"\r  [{elapsed:5.1f}s] 累計: {stats.total_packets} pkts, "
                      f"{format_bytes(stats.total_bytes)} | "
                      f"現在: {format_bitrate(bitrate)}, {pkt_count} pkts/s | "
                      f"残り {remaining:.0f}s   ", end="", flush=True)
                last_stats_time = now

    finally:
        sock.close()
        print_final_stats(stats)


def main():
    parser = argparse.ArgumentParser(
        description="UDP H.264映像受信テスト — パケット統計とRTP解析を行う"
    )
    parser.add_argument(
        "--port", type=int, default=50000,
        help="受信UDPポート番号 (デフォルト: 50000)"
    )
    parser.add_argument(
        "--timeout", type=float, default=30,
        help="自動終了までのタイムアウト秒数 (デフォルト: 30)"
    )
    args = parser.parse_args()

    print("=" * 60)
    print("MirageComplete UDP映像受信テスト")
    print("=" * 60)

    run_receiver(port=args.port, timeout=args.timeout)


if __name__ == "__main__":
    main()
