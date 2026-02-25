#!/usr/bin/env python3
"""
mirage_client.py - MirageGUI TCP JSON-RPC Client
MirageGUI (MacroApiServer on port 19840) と通信するクライアント

使い方:
    from mirage_client import MirageClient, MacroRunner

    # 直接API呼び出し
    client = MirageClient()
    client.connect()
    devices = client.list_devices()
    client.tap(0, 500, 800)

    # マクロ実行
    runner = MacroRunner(client, device_index=0)
    runner.execute('''
        device.tap(500, 800)
        sleep(1)
        device.swipe(100, 500, 900, 500)
    ''')
"""

import socket
import json
import struct
import base64
import time
import threading
from typing import Optional, Any, Callable


class MirageClient:
    """MirageGUI MacroApiServer (TCP JSON-RPC) クライアント"""

    def __init__(self, host: str = "127.0.0.1", port: int = 19840):
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = None
        self._request_id = 0
        self._lock = threading.Lock()

    def connect(self) -> bool:
        """サーバーに接続"""
        if self._sock:
            return True
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(5.0)
            self._sock.connect((self.host, self.port))
            return True
        except Exception as e:
            self._sock = None
            raise ConnectionError(f"MirageGUI connection failed: {e}")

    def disconnect(self):
        """接続を閉じる"""
        if self._sock:
            try:
                self._sock.close()
            except Exception:
                pass
            self._sock = None

    def _send_request(self, method: str, params: dict = None) -> Any:
        """JSON-RPC リクエスト送信"""
        with self._lock:
            if not self._sock:
                self.connect()

            self._request_id += 1
            request = {
                "jsonrpc": "2.0",
                "id": self._request_id,
                "method": method,
                "params": params or {}
            }

            # プロトコル: 4バイト長プレフィックス + JSON
            data = json.dumps(request, ensure_ascii=False).encode("utf-8")
            header = struct.pack(">I", len(data))

            self._sock.sendall(header + data)

            # レスポンス受信
            resp_header = self._recv_exact(4)
            resp_len = struct.unpack(">I", resp_header)[0]
            resp_data = self._recv_exact(resp_len)

            response = json.loads(resp_data.decode("utf-8"))

            if "error" in response:
                raise RuntimeError(f"RPC Error: {response['error']}")

            return response.get("result")

    def _recv_exact(self, n: int) -> bytes:
        """指定バイト数を確実に受信"""
        chunks = []
        remaining = n
        while remaining > 0:
            chunk = self._sock.recv(min(remaining, 4096))
            if not chunk:
                raise ConnectionError("Connection closed")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    # ============ 接続確認 ============

    def ping(self) -> bool:
        """サーバー接続確認"""
        try:
            result = self._send_request("ping")
            return result == "pong" or result is True
        except Exception:
            return False

    # ============ デバイス管理 ============

    def list_devices(self) -> list:
        """接続デバイス一覧を取得"""
        return self._send_request("list_devices")

    def device_info(self, device_index: int) -> dict:
        """デバイス情報を取得"""
        return self._send_request("device_info", {"device_index": device_index})

    # ============ タッチ操作 ============

    def tap(self, device_index: int, x: int, y: int, duration_ms: int = 50) -> bool:
        """タップ (AOA/ADB自動選択)"""
        return self._send_request("tap", {
            "device_index": device_index,
            "x": x, "y": y,
            "duration_ms": duration_ms
        })

    def swipe(self, device_index: int, x1: int, y1: int, x2: int, y2: int,
              duration_ms: int = 300) -> bool:
        """スワイプ"""
        return self._send_request("swipe", {
            "device_index": device_index,
            "x1": x1, "y1": y1,
            "x2": x2, "y2": y2,
            "duration_ms": duration_ms
        })

    def long_press(self, device_index: int, x: int, y: int,
                   duration_ms: int = 1000) -> bool:
        """長押し"""
        return self._send_request("long_press", {
            "device_index": device_index,
            "x": x, "y": y,
            "duration_ms": duration_ms
        })

    def multi_touch(self, device_index: int, points: list) -> bool:
        """マルチタッチ (points: [{x, y, action}, ...])"""
        return self._send_request("multi_touch", {
            "device_index": device_index,
            "points": points
        })

    def pinch(self, device_index: int, cx: int, cy: int,
              start_distance: int, end_distance: int,
              duration_ms: int = 300) -> bool:
        """ピンチイン/アウト"""
        return self._send_request("pinch", {
            "device_index": device_index,
            "cx": cx, "cy": cy,
            "start_distance": start_distance,
            "end_distance": end_distance,
            "duration_ms": duration_ms
        })

    # ============ キー・テキスト入力 ============

    def key(self, device_index: int, keycode: int) -> bool:
        """キーコード送信 (ADB経由)"""
        return self._send_request("key", {
            "device_index": device_index,
            "keycode": keycode
        })

    def text(self, device_index: int, text: str) -> bool:
        """テキスト入力 (ADB経由)"""
        return self._send_request("text", {
            "device_index": device_index,
            "text": text
        })

    # ============ UI要素操作 ============

    def click_id(self, device_index: int, resource_id: str) -> bool:
        """リソースIDでクリック (ADB uiautomator)"""
        return self._send_request("click_id", {
            "device_index": device_index,
            "resource_id": resource_id
        })

    def click_text(self, device_index: int, text: str) -> bool:
        """テキストでクリック (ADB uiautomator)"""
        return self._send_request("click_text", {
            "device_index": device_index,
            "text": text
        })

    # ============ アプリ制御 ============

    def launch_app(self, device_index: int, package: str, activity: str = None) -> bool:
        """アプリを起動"""
        params = {"device_index": device_index, "package": package}
        if activity:
            params["activity"] = activity
        return self._send_request("launch_app", params)

    def force_stop(self, device_index: int, package: str) -> bool:
        """アプリを強制終了"""
        return self._send_request("force_stop", {
            "device_index": device_index,
            "package": package
        })

    # ============ スクリーンショット・録画 ============

    def screenshot(self, device_index: int) -> bytes:
        """スクリーンショット取得 (PNG bytes)"""
        result = self._send_request("screenshot", {"device_index": device_index})
        if isinstance(result, str):
            return base64.b64decode(result)
        return result

    def screenshot_base64(self, device_index: int) -> str:
        """スクリーンショット取得 (base64文字列)"""
        return self._send_request("screenshot", {"device_index": device_index})

    def screen_record(self, device_index: int, duration_sec: int = 10,
                      save_path: str = None) -> str:
        """画面録画"""
        return self._send_request("screen_record", {
            "device_index": device_index,
            "duration_sec": duration_sec,
            "save_path": save_path
        })

    # ============ テキスト検出 ============

    def screen_contains_text(self, device_index: int, text: str) -> bool:
        """画面にテキストが含まれているか確認"""
        return self._send_request("screen_contains_text", {
            "device_index": device_index,
            "text": text
        })

    def find_text(self, device_index: int, text: str) -> Optional[dict]:
        """テキストの座標を検索"""
        return self._send_request("find_text", {
            "device_index": device_index,
            "text": text
        })

    def tap_text(self, device_index: int, text: str) -> bool:
        """テキストをタップ"""
        return self._send_request("tap_text", {
            "device_index": device_index,
            "text": text
        })

    def wait_for_text(self, device_index: int, text: str,
                      timeout_sec: float = 10.0, interval_sec: float = 0.5) -> bool:
        """テキストが出現するまで待機"""
        return self._send_request("wait_for_text", {
            "device_index": device_index,
            "text": text,
            "timeout_sec": timeout_sec,
            "interval_sec": interval_sec
        })

    def tap_element(self, device_index: int, element_id: str = None,
                    element_text: str = None) -> bool:
        """要素をタップ"""
        params = {"device_index": device_index}
        if element_id:
            params["element_id"] = element_id
        if element_text:
            params["element_text"] = element_text
        return self._send_request("tap_element", params)

    # ============ コンテキストマネージャ ============

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.disconnect()
        return False


class MacroRunner:
    """マクロ実行環境 (exec用namespace提供)"""

    def __init__(self, client: MirageClient, device_index: int = 0,
                 step_delay: float = 0.3, api_bridge: Callable = None):
        self.client = client
        self.device_index = device_index
        self.step_delay = step_delay
        self.api_bridge = api_bridge
        self._cancelled = False
        self._running = False

    def cancel(self):
        """実行中のマクロをキャンセル"""
        self._cancelled = True

    def is_running(self) -> bool:
        """マクロが実行中かどうか"""
        return self._running

    def execute(self, code: str) -> dict:
        """Pythonマクロコードを実行"""
        self._cancelled = False
        self._running = True

        try:
            namespace = self._create_namespace()
            exec(code, namespace)
            return {"success": True}
        except MacroCancelled:
            return {"success": False, "cancelled": True}
        except Exception as e:
            return {"success": False, "error": str(e)}
        finally:
            self._running = False

    def _create_namespace(self) -> dict:
        """exec()用のnamespaceを生成"""
        device = _DeviceProxy(self.client, self.device_index, self)

        def checked_sleep(sec: float):
            """キャンセルチェック付きsleep"""
            if self._cancelled:
                raise MacroCancelled()
            time.sleep(sec)
            if self._cancelled:
                raise MacroCancelled()

        return {
            "device": device,
            "sleep": checked_sleep,
            "print": print,
            "time": time,
            "api_bridge": self.api_bridge,
        }


class MacroCancelled(Exception):
    """マクロがキャンセルされた"""
    pass


class _DeviceProxy:
    """マクロ内で device.xxx() を呼び出すためのプロキシ"""

    def __init__(self, client: MirageClient, device_index: int, runner: MacroRunner):
        self._client = client
        self._index = device_index
        self._runner = runner

    def _check_cancel(self):
        if self._runner._cancelled:
            raise MacroCancelled()

    def _delay(self):
        if self._runner.step_delay > 0:
            time.sleep(self._runner.step_delay)

    # タッチ操作
    def tap(self, x: int, y: int, duration_ms: int = 50):
        self._check_cancel()
        result = self._client.tap(self._index, x, y, duration_ms)
        self._delay()
        return result

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration_ms: int = 300):
        self._check_cancel()
        result = self._client.swipe(self._index, x1, y1, x2, y2, duration_ms)
        self._delay()
        return result

    def long_press(self, x: int, y: int, duration_ms: int = 1000):
        self._check_cancel()
        result = self._client.long_press(self._index, x, y, duration_ms)
        self._delay()
        return result

    def multi_touch(self, points: list):
        self._check_cancel()
        result = self._client.multi_touch(self._index, points)
        self._delay()
        return result

    def pinch(self, cx: int, cy: int, start_dist: int, end_dist: int, duration_ms: int = 300):
        self._check_cancel()
        result = self._client.pinch(self._index, cx, cy, start_dist, end_dist, duration_ms)
        self._delay()
        return result

    # キー・テキスト入力
    def key(self, keycode: int):
        self._check_cancel()
        result = self._client.key(self._index, keycode)
        self._delay()
        return result

    def text(self, text: str):
        self._check_cancel()
        result = self._client.text(self._index, text)
        self._delay()
        return result

    # アプリ制御
    def launch_app(self, package: str, activity: str = None):
        self._check_cancel()
        result = self._client.launch_app(self._index, package, activity)
        self._delay()
        return result

    def force_stop(self, package: str):
        self._check_cancel()
        result = self._client.force_stop(self._index, package)
        self._delay()
        return result

    # スクリーンショット
    def screenshot(self) -> bytes:
        self._check_cancel()
        return self._client.screenshot(self._index)

    def screenshot_base64(self) -> str:
        self._check_cancel()
        return self._client.screenshot_base64(self._index)

    # 画面録画
    def screen_record(self, duration_sec: int = 10, save_path: str = None) -> str:
        self._check_cancel()
        return self._client.screen_record(self._index, duration_sec, save_path)

    # UI要素操作
    def click_id(self, resource_id: str):
        self._check_cancel()
        result = self._client.click_id(self._index, resource_id)
        self._delay()
        return result

    def click_text(self, text: str):
        self._check_cancel()
        result = self._client.click_text(self._index, text)
        self._delay()
        return result

    # テキスト検出
    def screen_contains_text(self, text: str) -> bool:
        self._check_cancel()
        return self._client.screen_contains_text(self._index, text)

    def find_and_tap_text(self, text: str) -> bool:
        """テキストを探してタップ"""
        self._check_cancel()
        result = self._client.tap_text(self._index, text)
        self._delay()
        return result

    def wait_for_text(self, text: str, timeout_sec: float = 10.0,
                      interval_sec: float = 0.5) -> bool:
        """テキストが出現するまで待機"""
        self._check_cancel()
        return self._client.wait_for_text(self._index, text, timeout_sec, interval_sec)

    def tap_element(self, element_id: str = None, element_text: str = None):
        """要素をタップ"""
        self._check_cancel()
        result = self._client.tap_element(self._index, element_id, element_text)
        self._delay()
        return result


# ============ テスト用エントリポイント ============

if __name__ == "__main__":
    import sys

    # Windows cp932対策
    if sys.platform == "win32":
        try:
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

    print("=== MirageClient Connectivity Test ===")
    print(f"Target: 127.0.0.1:19840")

    client = MirageClient()

    # 接続テスト
    try:
        client.connect()
        print("OK: TCP connection established")
    except ConnectionError as e:
        print(f"WARN: MirageGUI not running")
        print(f"      {e}")
        sys.exit(0)

    # ping テスト
    try:
        if client.ping():
            print("OK: ping successful")
        else:
            print("WARN: ping returned false")
    except TimeoutError:
        print("WARN: ping timeout (MirageGUI may not support ping)")
    except Exception as e:
        print(f"WARN: ping failed: {e}")

    # デバイスリスト
    try:
        devices = client.list_devices()
        print(f"OK: {len(devices)} device(s) found")
        for i, dev in enumerate(devices):
            print(f"    [{i}] {dev}")
    except TimeoutError:
        print("WARN: list_devices timeout")
    except Exception as e:
        print(f"WARN: list_devices failed: {e}")

    client.disconnect()
    print("=== Test Complete ===")
