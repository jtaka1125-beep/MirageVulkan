"""MacroEditorAPI - pywebview JS bridge"""
import json
import subprocess
import base64
import tempfile
import os
import re
import threading
import xml.etree.ElementTree as ET
from pathlib import Path
from backend.mirage_client import MirageClient, MacroRunner

MACROS_DIR = Path(__file__).parent.parent / 'macros'
MACROS_DIR.mkdir(exist_ok=True)

_runner: MacroRunner = None
_run_lock = threading.Lock()


def _run_adb(serial, *args, timeout=10):
    cmd = ['adb', '-s', serial] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.stdout.strip()


def _parse_bounds(bounds_str):
    m = re.match(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]', bounds_str or '')
    if m:
        return tuple(int(v) for v in m.groups())
    return None


class MacroEditorAPI:
    """Exposed to JavaScript via window.pywebview.api"""

    def get_devices(self):
        try:
            client = MirageClient(timeout=2.0)
            client.connect()
            result = client.list_devices()
            client.disconnect()
            if isinstance(result, list):
                return [{'serial': d.get('hardware_id', d.get('preferred_adb_id', '')),
                         'model': d.get('model', 'unknown'),
                         'source': 'mirage'} for d in result]
        except Exception:
            pass
        try:
            result = subprocess.run(['adb', 'devices', '-l'], capture_output=True, text=True, timeout=5)
            devices = []
            for line in result.stdout.strip().split('\n')[1:]:
                if '\tdevice' in line:
                    parts = line.split()
                    serial = parts[0]
                    model = next((p.split(':')[1] for p in parts if p.startswith('model:')), 'unknown')
                    devices.append({'serial': serial, 'model': model, 'source': 'adb'})
            return devices
        except Exception as e:
            return {'error': str(e)}

    def capture_screen(self, serial):
        try:
            client = MirageClient(timeout=15.0)
            client.connect()
            result = client.screenshot(serial)
            client.disconnect()
            if result and result.get('path'):
                path = result['path']
                if os.path.exists(path):
                    with open(path, 'rb') as f:
                        b64 = base64.b64encode(f.read()).decode('ascii')
                    try:
                        c2 = MirageClient(timeout=3.0)
                        c2.connect()
                        info = c2.device_info(serial)
                        c2.disconnect()
                        w = info.get('screen_width', 1080)
                        h = info.get('screen_height', 1920)
                    except Exception:
                        w, h = 1080, 1920
                    return {'base64': b64, 'width': w, 'height': h}
        except Exception:
            pass
        try:
            subprocess.run(['adb', '-s', serial, 'shell', 'screencap', '-p', '/sdcard/mirage_cap.png'],
                           capture_output=True, timeout=10)
            tmp = os.path.join(tempfile.gettempdir(), 'mirage_screen.png')
            subprocess.run(['adb', '-s', serial, 'pull', '/sdcard/mirage_cap.png', tmp],
                           capture_output=True, timeout=10)
            subprocess.run(['adb', '-s', serial, 'shell', 'rm', '/sdcard/mirage_cap.png'],
                           capture_output=True, timeout=5)
            if not os.path.exists(tmp):
                return {'error': 'Screenshot file not found'}
            with open(tmp, 'rb') as f:
                b64 = base64.b64encode(f.read()).decode('ascii')
            result = subprocess.run(['adb', '-s', serial, 'shell', 'wm', 'size'],
                                    capture_output=True, text=True, timeout=5)
            width, height = 1080, 1920
            for line in result.stdout.strip().split('\n'):
                if 'Physical size' in line or 'Override size' in line:
                    size_str = line.split(':')[-1].strip()
                    if 'x' in size_str:
                        w, h = size_str.split('x')
                        width, height = int(w), int(h)
            return {'base64': b64, 'width': width, 'height': height}
        except Exception as e:
            return {'error': str(e)}

    # ==================== Screen Analysis ====================

    def dump_ui(self, serial):
        """Dump UI hierarchy via uiautomator. Returns {elements: [...]}."""
        try:
            _run_adb(serial, 'shell', 'uiautomator', 'dump', '/sdcard/mirage_ui.xml', timeout=15)
            tmp = os.path.join(tempfile.gettempdir(), 'mirage_ui.xml')
            subprocess.run(['adb', '-s', serial, 'pull', '/sdcard/mirage_ui.xml', tmp],
                           capture_output=True, timeout=10)
            _run_adb(serial, 'shell', 'rm', '/sdcard/mirage_ui.xml', timeout=5)
            if not os.path.exists(tmp):
                return {'error': 'UI dump file not found'}
            with open(tmp, 'r', encoding='utf-8') as f:
                xml_text = f.read()
            elements = self._parse_ui_xml(xml_text)
            return {'elements': elements, 'xml_size': len(xml_text)}
        except Exception as e:
            return {'error': str(e)}

    def find_text(self, serial, text):
        """Find UI elements containing text."""
        result = self.dump_ui(serial)
        if 'error' in result:
            return result
        text_lower = text.lower()
        matches = [e for e in result['elements']
                   if text_lower in (e.get('text', '') or '').lower()
                   or text_lower in (e.get('content_desc', '') or '').lower()]
        return {'matches': matches, 'count': len(matches)}

    def get_clickables(self, serial):
        """Get all clickable elements."""
        result = self.dump_ui(serial)
        if 'error' in result:
            return result
        clickables = [e for e in result['elements']
                      if e.get('clickable') or e.get('checkable') or e.get('scrollable')]
        return {'elements': clickables, 'count': len(clickables)}

    def capture_screen_with_elements(self, serial):
        """Screenshot + UI elements in one call."""
        screen = self.capture_screen(serial)
        if isinstance(screen, dict) and 'error' in screen:
            return screen
        ui = self.dump_ui(serial)
        screen['elements'] = ui.get('elements', []) if isinstance(ui, dict) and 'error' not in ui else []
        return screen

    def _parse_ui_xml(self, xml_text):
        elements = []
        try:
            root = ET.fromstring(xml_text)
        except ET.ParseError:
            return elements
        for node in root.iter('node'):
            bounds = _parse_bounds(node.get('bounds', ''))
            if not bounds:
                continue
            x1, y1, x2, y2 = bounds
            cx, cy = (x1 + x2) // 2, (y1 + y2) // 2
            w, h = x2 - x1, y2 - y1
            if w < 2 or h < 2:
                continue
            text = node.get('text', '') or ''
            resource_id = node.get('resource-id', '') or ''
            content_desc = node.get('content-desc', '') or ''
            class_name = node.get('class', '') or ''
            label = text or content_desc or (resource_id.split('/')[-1] if resource_id else '')
            elements.append({
                'text': text, 'resource_id': resource_id,
                'content_desc': content_desc,
                'class_name': class_name.split('.')[-1],
                'bounds': {'x1': x1, 'y1': y1, 'x2': x2, 'y2': y2},
                'center_x': cx, 'center_y': cy, 'width': w, 'height': h,
                'label': label[:40],
                'clickable': node.get('clickable') == 'true',
                'scrollable': node.get('scrollable') == 'true',
                'checkable': node.get('checkable') == 'true',
                'checked': node.get('checked') == 'true',
                'focused': node.get('focused') == 'true',
                'enabled': node.get('enabled') == 'true',
            })
        return elements

    # ==================== Macro Execution ====================

    def run_macro(self, serial, python_code):
        global _runner
        with _run_lock:
            try:
                client = MirageClient(timeout=30.0)
                client.connect()
                _runner = MacroRunner(client, serial, step_delay=0.3)
                result = _runner.execute(python_code)
                client.disconnect()
                _runner = None
                return result
            except ConnectionError:
                return self._run_macro_adb(serial, python_code)
            except Exception as e:
                _runner = None
                return {'status': 'error', 'error': str(e), 'log': [], 'steps': 0}

    def cancel_macro(self):
        global _runner
        if _runner:
            _runner.cancel()
            return {'status': 'cancelled'}
        return {'status': 'not_running'}

    def _run_macro_adb(self, serial, python_code):
        log_lines = []
        step_count = 0
        api_self = self

        class AdbDevice:
            def tap(self, x, y):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] tap({x}, {y})")
                subprocess.run(['adb', '-s', serial, 'shell', 'input', 'tap', str(x), str(y)],
                               capture_output=True, timeout=10)
            def swipe(self, x1, y1, x2, y2, duration=300):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] swipe({x1},{y1}->{x2},{y2})")
                subprocess.run(['adb', '-s', serial, 'shell', 'input', 'swipe',
                                str(x1), str(y1), str(x2), str(y2), str(duration)],
                               capture_output=True, timeout=10)
            def long_press(self, x, y, duration=1000):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] long_press({x}, {y}, {duration}ms)")
                subprocess.run(['adb', '-s', serial, 'shell', 'input', 'swipe',
                                str(x), str(y), str(x), str(y), str(duration)],
                               capture_output=True, timeout=15)
            def key(self, keycode):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] key({keycode})")
                subprocess.run(['adb', '-s', serial, 'shell', 'input', 'keyevent', str(keycode)],
                               capture_output=True, timeout=10)
            def text(self, t):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] text('{t}')")
                escaped = t.replace(' ', '%s').replace('&', '\\&').replace('<', '\\<')
                subprocess.run(['adb', '-s', serial, 'shell', 'input', 'text', escaped],
                               capture_output=True, timeout=10)
            def launch_app(self, package):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] launch_app({package})")
                subprocess.run(['adb', '-s', serial, 'shell', 'monkey', '-p', package,
                                '-c', 'android.intent.category.LAUNCHER', '1'],
                               capture_output=True, timeout=10)
            def force_stop(self, package):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] force_stop({package})")
                subprocess.run(['adb', '-s', serial, 'shell', 'am', 'force-stop', package],
                               capture_output=True, timeout=10)
            def screenshot(self, filename='screenshot.png'):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] screenshot({filename})")
            def screen_contains_text(self, text):
                nonlocal step_count; step_count += 1
                result = api_self.find_text(serial, text)
                found = result.get('count', 0) > 0
                log_lines.append(f"[ADB] screen_contains_text('{text}') -> {found}")
                return found
            def find_and_tap_text(self, text):
                nonlocal step_count; step_count += 1
                result = api_self.find_text(serial, text)
                matches = result.get('matches', [])
                if matches:
                    cx, cy = matches[0]['center_x'], matches[0]['center_y']
                    log_lines.append(f"[ADB] find_and_tap_text('{text}') -> tap({cx},{cy})")
                    subprocess.run(['adb', '-s', serial, 'shell', 'input', 'tap', str(cx), str(cy)],
                                   capture_output=True, timeout=10)
                    return True
                log_lines.append(f"[ADB] find_and_tap_text('{text}') -> not found")
                return False
            def wait_for_text(self, text, timeout_sec=10, interval=1.0):
                nonlocal step_count; step_count += 1
                import time
                deadline = time.time() + timeout_sec
                while time.time() < deadline:
                    result = api_self.find_text(serial, text)
                    if result.get('count', 0) > 0:
                        log_lines.append(f"[ADB] wait_for_text('{text}') -> found")
                        return True
                    time.sleep(interval)
                log_lines.append(f"[ADB] wait_for_text('{text}', {timeout_sec}s) -> timeout")
                return False
            def tap_element(self, resource_id):
                nonlocal step_count; step_count += 1
                result = api_self.dump_ui(serial)
                if 'elements' in result:
                    for e in result['elements']:
                        if resource_id in (e.get('resource_id', '') or ''):
                            cx, cy = e['center_x'], e['center_y']
                            log_lines.append(f"[ADB] tap_element('{resource_id}') -> tap({cx},{cy})")
                            subprocess.run(['adb', '-s', serial, 'shell', 'input', 'tap', str(cx), str(cy)],
                                           capture_output=True, timeout=10)
                            return True
                log_lines.append(f"[ADB] tap_element('{resource_id}') -> not found")
                return False
            def screen_record(self, duration=10):
                nonlocal step_count; step_count += 1
                log_lines.append(f"[ADB] screen_record({duration}s) -> stub")

        import time as _time
        import logging as _logging
        class _FakeLog:
            def info(self, msg): log_lines.append(f"[LOG] {msg}")
            def warning(self, msg): log_lines.append(f"[WARN] {msg}")
            def error(self, msg): log_lines.append(f"[ERROR] {msg}")

        namespace = {"device": AdbDevice(), "time": _time, "logging": _logging, "log": _FakeLog()}
        try:
            exec(compile(python_code, "<macro>", "exec"), namespace)
            return {'status': 'ok', 'log': log_lines, 'steps': step_count, 'mode': 'adb_fallback'}
        except Exception as e:
            log_lines.append(f"ERROR: {e}")
            return {'status': 'error', 'log': log_lines, 'steps': step_count, 'error': str(e)}

    # ==================== Save / Load ====================

    def save_macro(self, name, workspace_json, python_code):
        macro_path = MACROS_DIR / name
        macro_path.mkdir(exist_ok=True)
        (macro_path / 'workspace.json').write_text(
            json.dumps(workspace_json, indent=2, ensure_ascii=False), encoding='utf-8')
        (macro_path / f'{name}.py').write_text(python_code, encoding='utf-8')
        return {'status': 'ok', 'path': str(macro_path)}

    def load_macro(self, name):
        ws_file = MACROS_DIR / name / 'workspace.json'
        return json.loads(ws_file.read_text(encoding='utf-8')) if ws_file.exists() else None

    def list_macros(self):
        return [d.name for d in MACROS_DIR.iterdir() if d.is_dir()] if MACROS_DIR.exists() else []

    def ping(self):
        mirage_ok = False
        try:
            c = MirageClient(timeout=2.0); c.connect(); c.ping(); c.disconnect()
            mirage_ok = True
        except Exception:
            pass
        return {'status': 'ok', 'version': '0.3.0', 'mirage_connected': mirage_ok}
