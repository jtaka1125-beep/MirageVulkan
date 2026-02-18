"""MirageClient - TCP JSON-RPC client for MacroApiServer (port 19840)"""
import json
import socket
import time
import subprocess
import re
import xml.etree.ElementTree as ET
import os
import tempfile
from typing import Optional, Any


class MirageClient:
    """Connect to MirageGUI's MacroApiServer via TCP JSON-RPC."""

    def __init__(self, host: str = "127.0.0.1", port: int = 19840, timeout: float = 10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
        self._id = 0
        self._buf = b""

    def connect(self) -> bool:
        try:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self._sock.settimeout(self.timeout)
            self._sock.connect((self.host, self.port))
            return True
        except Exception as e:
            self._sock = None
            raise ConnectionError(f"MacroApiServer connect failed: {e}")

    def disconnect(self):
        if self._sock:
            try: self._sock.close()
            except Exception: pass
            self._sock = None

    @property
    def connected(self) -> bool:
        return self._sock is not None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.disconnect()

    def _call(self, method: str, params: dict = None) -> Any:
        if not self._sock:
            raise ConnectionError("Not connected")
        self._id += 1
        req = {"id": self._id, "method": method}
        if params:
            req["params"] = params
        line = json.dumps(req, ensure_ascii=False) + "\n"
        self._sock.sendall(line.encode("utf-8"))
        response = self._read_line()
        data = json.loads(response)
        if "error" in data:
            err = data["error"]
            raise RuntimeError(f"RPC error {err.get('code', '?')}: {err.get('message', 'unknown')}")
        return data.get("result")

    def _read_line(self) -> str:
        while b"\n" not in self._buf:
            chunk = self._sock.recv(4096)
            if not chunk:
                raise ConnectionError("Server disconnected")
            self._buf += chunk
        line, self._buf = self._buf.split(b"\n", 1)
        return line.decode("utf-8")

    # ---- high-level API ----
    def ping(self) -> dict: return self._call("ping")
    def list_devices(self) -> list: return self._call("list_devices")
    def device_info(self, device_id: str) -> dict: return self._call("device_info", {"device_id": device_id})
    def tap(self, device_id: str, x: int, y: int) -> dict: return self._call("tap", {"device_id": device_id, "x": x, "y": y})
    def swipe(self, device_id: str, x1: int, y1: int, x2: int, y2: int, duration: int = 300) -> dict:
        return self._call("swipe", {"device_id": device_id, "x1": x1, "y1": y1, "x2": x2, "y2": y2, "duration": duration})
    def long_press(self, device_id: str, x: int, y: int, duration: int = 1000) -> dict:
        return self._call("long_press", {"device_id": device_id, "x": x, "y": y, "duration": duration})
    def key(self, device_id: str, keycode: int) -> dict: return self._call("key", {"device_id": device_id, "keycode": keycode})
    def text(self, device_id: str, text: str) -> dict: return self._call("text", {"device_id": device_id, "text": text})
    def click_id(self, device_id: str, resource_id: str) -> dict: return self._call("click_id", {"device_id": device_id, "resource_id": resource_id})
    def click_text(self, device_id: str, text: str) -> dict: return self._call("click_text", {"device_id": device_id, "text": text})
    def launch_app(self, device_id: str, package: str) -> dict: return self._call("launch_app", {"device_id": device_id, "package": package})
    def force_stop(self, device_id: str, package: str) -> dict: return self._call("force_stop", {"device_id": device_id, "package": package})
    def screenshot(self, device_id: str) -> dict: return self._call("screenshot", {"device_id": device_id})

    # ---- OCR API (requires USE_OCR=ON in MirageVulkan) ----
    def ocr_analyze(self, device_id: str) -> dict: return self._call("ocr_analyze", {"device_id": device_id})
    def ocr_find_text(self, device_id: str, query: str) -> dict: return self._call("ocr_find_text", {"device_id": device_id, "query": query})
    def ocr_has_text(self, device_id: str, query: str) -> dict: return self._call("ocr_has_text", {"device_id": device_id, "query": query})
    def ocr_tap_text(self, device_id: str, query: str) -> dict: return self._call("ocr_tap_text", {"device_id": device_id, "query": query})


# ===========================================================================
# Screen Analysis via ADB uiautomator (used by MacroRunner)
# ===========================================================================

def _parse_bounds(bounds_str):
    m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', bounds_str or '')
    return tuple(int(v) for v in m.groups()) if m else None


def dump_ui_hierarchy(serial: str) -> list:
    """Run uiautomator dump and parse elements. Returns list of dicts."""
    try:
        subprocess.run(['adb', '-s', serial, 'shell', 'uiautomator', 'dump', '/sdcard/mirage_ui.xml'],
                       capture_output=True, timeout=15)
        tmp = os.path.join(tempfile.gettempdir(), 'mirage_ui_client.xml')
        subprocess.run(['adb', '-s', serial, 'pull', '/sdcard/mirage_ui.xml', tmp],
                       capture_output=True, timeout=10)
        subprocess.run(['adb', '-s', serial, 'shell', 'rm', '/sdcard/mirage_ui.xml'],
                       capture_output=True, timeout=5)
        if not os.path.exists(tmp):
            return []
        with open(tmp, 'r', encoding='utf-8') as f:
            xml_text = f.read()
        root = ET.fromstring(xml_text)
        elements = []
        for node in root.iter('node'):
            bounds = _parse_bounds(node.get('bounds', ''))
            if not bounds: continue
            x1, y1, x2, y2 = bounds
            if (x2 - x1) < 2 or (y2 - y1) < 2: continue
            elements.append({
                'text': node.get('text', '') or '',
                'resource_id': node.get('resource-id', '') or '',
                'content_desc': node.get('content-desc', '') or '',
                'center_x': (x1 + x2) // 2, 'center_y': (y1 + y2) // 2,
                'clickable': node.get('clickable') == 'true',
            })
        return elements
    except Exception:
        return []


def find_text_on_screen(serial: str, text: str) -> list:
    """Find elements containing text. Returns list of matches."""
    text_lower = text.lower()
    return [e for e in dump_ui_hierarchy(serial)
            if text_lower in (e.get('text', '') or '').lower()
            or text_lower in (e.get('content_desc', '') or '').lower()]


def find_element_by_id(serial: str, resource_id: str) -> Optional[dict]:
    """Find element by resource-id. Returns first match or None."""
    for e in dump_ui_hierarchy(serial):
        if resource_id in (e.get('resource_id', '') or ''):
            return e
    return None


# ===========================================================================
# MacroRunner
# ===========================================================================

class MacroRunner:
    """Execute generated Python macro code against a device."""

    def __init__(self, client: MirageClient, device_id: str, step_delay: float = 0.3):
        self.client = client
        self.device_id = device_id
        self.step_delay = step_delay
        self.log_lines: list[str] = []
        self._cancelled = False

    def cancel(self):
        self._cancelled = True

    def execute(self, code: str) -> dict:
        self._cancelled = False
        self.log_lines = []
        device = _DeviceProxy(self)
        namespace = {
            "device": device,
            "time": __import__("time"),
            "logging": __import__("logging"),
            "log": _LogProxy(self),
        }
        try:
            exec(compile(code, "<macro>", "exec"), namespace)
            return {"status": "ok", "log": self.log_lines, "steps": device.step_count}
        except _MacroCancelled:
            return {"status": "cancelled", "log": self.log_lines, "steps": device.step_count}
        except Exception as e:
            self.log_lines.append(f"ERROR: {e}")
            return {"status": "error", "log": self.log_lines, "steps": device.step_count, "error": str(e)}


class _MacroCancelled(Exception):
    pass


class _DeviceProxy:
    """Proxy object injected as `device` into macro namespace."""

    def __init__(self, runner: MacroRunner):
        self._r = runner
        self.step_count = 0

    def _check(self):
        if self._r._cancelled: raise _MacroCancelled()

    def _step(self):
        self.step_count += 1
        self._check()
        if self._r.step_delay > 0:
            __import__("time").sleep(self._r.step_delay)

    def tap(self, x: int, y: int):
        self._step()
        self._r.log_lines.append(f"tap({x}, {y})")
        return self._r.client.tap(self._r.device_id, x, y)

    def swipe(self, x1: int, y1: int, x2: int, y2: int, duration: int = 300):
        self._step()
        self._r.log_lines.append(f"swipe({x1},{y1}->{x2},{y2}, {duration}ms)")
        return self._r.client.swipe(self._r.device_id, x1, y1, x2, y2, duration)

    def long_press(self, x: int, y: int, duration: int = 1000):
        self._step()
        self._r.log_lines.append(f"long_press({x}, {y}, {duration}ms)")
        return self._r.client.long_press(self._r.device_id, x, y, duration)

    def key(self, keycode: int):
        self._step()
        self._r.log_lines.append(f"key({keycode})")
        return self._r.client.key(self._r.device_id, keycode)

    def text(self, t: str):
        self._step()
        self._r.log_lines.append(f"text('{t}')")
        return self._r.client.text(self._r.device_id, t)

    def launch_app(self, package: str):
        self._step()
        self._r.log_lines.append(f"launch_app({package})")
        return self._r.client.launch_app(self._r.device_id, package)

    def force_stop(self, package: str):
        self._step()
        self._r.log_lines.append(f"force_stop({package})")
        return self._r.client.force_stop(self._r.device_id, package)

    def screenshot(self, filename: str = "screenshot.png"):
        self._step()
        self._r.log_lines.append(f"screenshot({filename})")
        return self._r.client.screenshot(self._r.device_id)

    def screen_contains_text(self, text: str) -> bool:
        """Real implementation via uiautomator dump."""
        self._check()
        matches = find_text_on_screen(self._r.device_id, text)
        found = len(matches) > 0
        self._r.log_lines.append(f"screen_contains_text('{text}') -> {found}")
        return found

    def find_and_tap_text(self, text: str) -> bool:
        """Find text on screen and tap its center."""
        self._step()
        matches = find_text_on_screen(self._r.device_id, text)
        if matches:
            cx, cy = matches[0]['center_x'], matches[0]['center_y']
            self._r.log_lines.append(f"find_and_tap_text('{text}') -> tap({cx},{cy})")
            self._r.client.tap(self._r.device_id, cx, cy)
            return True
        self._r.log_lines.append(f"find_and_tap_text('{text}') -> not found")
        return False

    def wait_for_text(self, text: str, timeout_sec: int = 10, interval: float = 1.0) -> bool:
        """Poll screen until text appears."""
        self._step()
        deadline = __import__("time").time() + timeout_sec
        while __import__("time").time() < deadline:
            self._check()
            matches = find_text_on_screen(self._r.device_id, text)
            if matches:
                self._r.log_lines.append(f"wait_for_text('{text}') -> found")
                return True
            __import__("time").sleep(interval)
        self._r.log_lines.append(f"wait_for_text('{text}', {timeout_sec}s) -> timeout")
        return False

    def tap_element(self, resource_id: str) -> bool:
        """Find element by resource-id and tap."""
        self._step()
        elem = find_element_by_id(self._r.device_id, resource_id)
        if elem:
            cx, cy = elem['center_x'], elem['center_y']
            self._r.log_lines.append(f"tap_element('{resource_id}') -> tap({cx},{cy})")
            self._r.client.tap(self._r.device_id, cx, cy)
            return True
        # Fallback: try AOA click_id if available
        try:
            self._r.client.click_id(self._r.device_id, resource_id)
            self._r.log_lines.append(f"tap_element('{resource_id}') -> click_id (AOA)")
            return True
        except Exception:
            self._r.log_lines.append(f"tap_element('{resource_id}') -> not found")
            return False

    def screen_record(self, duration: int = 10):
        self._step()
        self._r.log_lines.append(f"screen_record({duration}s) -> stub")

    # ---- OCR methods (H264 frame-based, no ADB screenshot needed) ----
    def ocr_analyze(self) -> dict:
        """OCR full text extraction from live H264 frame."""
        self._step()
        result = self._r.client.ocr_analyze(self._r.device_id)
        self._r.log_lines.append(f"ocr_analyze() -> {result.get('word_count', 0)} words")
        return result

    def ocr_find_text(self, query: str) -> list:
        """Find text via OCR. Returns list of matches with bounding boxes."""
        self._step()
        result = self._r.client.ocr_find_text(self._r.device_id, query)
        matches = result.get('matches', [])
        self._r.log_lines.append(f"ocr_find_text('{query}') -> {len(matches)} matches")
        return matches

    def ocr_has_text(self, query: str) -> bool:
        """Check if text exists on screen via OCR."""
        self._check()
        result = self._r.client.ocr_has_text(self._r.device_id, query)
        found = result.get('found', False)
        self._r.log_lines.append(f"ocr_has_text('{query}') -> {found}")
        return found

    def ocr_tap_text(self, query: str) -> bool:
        """Find text via OCR and tap its center."""
        self._step()
        result = self._r.client.ocr_tap_text(self._r.device_id, query)
        found = result.get('found', False)
        if found:
            self._r.log_lines.append(f"ocr_tap_text('{query}') -> tap({result.get('x')},{result.get('y')})")
        else:
            self._r.log_lines.append(f"ocr_tap_text('{query}') -> not found")
        return found

    def ocr_wait_for_text(self, query: str, timeout_sec: int = 10, interval: float = 1.0) -> bool:
        """Poll OCR until text appears on screen."""
        self._step()
        import time as _time
        deadline = _time.time() + timeout_sec
        while _time.time() < deadline:
            self._check()
            if self.ocr_has_text(query):
                self._r.log_lines.append(f"ocr_wait_for_text('{query}') -> found")
                return True
            _time.sleep(interval)
        self._r.log_lines.append(f"ocr_wait_for_text('{query}', {timeout_sec}s) -> timeout")
        return False


class _LogProxy:
    def __init__(self, runner: MacroRunner):
        self._r = runner
    def info(self, msg: str): self._r.log_lines.append(f"[LOG] {msg}")
    def warning(self, msg: str): self._r.log_lines.append(f"[WARN] {msg}")
    def error(self, msg: str): self._r.log_lines.append(f"[ERROR] {msg}")
