"""MacroEditorAPI - pywebview JS bridge"""
import json
import subprocess
import base64
import tempfile
import os
from pathlib import Path

MACROS_DIR = Path(__file__).parent.parent / 'macros'
MACROS_DIR.mkdir(exist_ok=True)


class MacroEditorAPI:
    """Exposed to JavaScript via window.pywebview.api"""

    def get_devices(self):
        """Return connected ADB devices."""
        try:
            result = subprocess.run(
                ['adb', 'devices', '-l'],
                capture_output=True, text=True, timeout=5
            )
            devices = []
            for line in result.stdout.strip().split('\n')[1:]:
                if '\tdevice' in line:
                    parts = line.split()
                    serial = parts[0]
                    model = next(
                        (p.split(':')[1] for p in parts if p.startswith('model:')),
                        'unknown'
                    )
                    devices.append({'serial': serial, 'model': model})
            return devices
        except Exception as e:
            return {'error': str(e)}

    def capture_screen(self, serial):
        """Capture screenshot from device, return base64 + dimensions."""
        try:
            # Take screenshot on device
            subprocess.run(
                ['adb', '-s', serial, 'shell', 'screencap', '-p', '/sdcard/mirage_cap.png'],
                capture_output=True, timeout=10
            )
            # Pull to temp file
            tmp = os.path.join(tempfile.gettempdir(), 'mirage_screen.png')
            subprocess.run(
                ['adb', '-s', serial, 'pull', '/sdcard/mirage_cap.png', tmp],
                capture_output=True, timeout=10
            )
            # Clean up device file
            subprocess.run(
                ['adb', '-s', serial, 'shell', 'rm', '/sdcard/mirage_cap.png'],
                capture_output=True, timeout=5
            )

            if not os.path.exists(tmp):
                return {'error': 'Screenshot file not found'}

            # Read and encode
            with open(tmp, 'rb') as f:
                img_bytes = f.read()
            b64 = base64.b64encode(img_bytes).decode('ascii')

            # Get dimensions from wm size
            result = subprocess.run(
                ['adb', '-s', serial, 'shell', 'wm', 'size'],
                capture_output=True, text=True, timeout=5
            )
            width, height = 1080, 1920  # defaults
            for line in result.stdout.strip().split('\n'):
                if 'Physical size' in line or 'Override size' in line:
                    size_str = line.split(':')[-1].strip()
                    if 'x' in size_str:
                        w, h = size_str.split('x')
                        width, height = int(w), int(h)

            return {'base64': b64, 'width': width, 'height': height}

        except Exception as e:
            return {'error': str(e)}

    def save_macro(self, name, workspace_json, python_code):
        """Save macro workspace and generated script."""
        macro_path = MACROS_DIR / name
        macro_path.mkdir(exist_ok=True)
        (macro_path / 'workspace.json').write_text(
            json.dumps(workspace_json, indent=2, ensure_ascii=False),
            encoding='utf-8'
        )
        (macro_path / f'{name}.py').write_text(python_code, encoding='utf-8')
        return {'status': 'ok', 'path': str(macro_path)}

    def load_macro(self, name):
        """Load saved macro workspace."""
        ws_file = MACROS_DIR / name / 'workspace.json'
        if ws_file.exists():
            return json.loads(ws_file.read_text(encoding='utf-8'))
        return None

    def list_macros(self):
        """List all saved macros."""
        if not MACROS_DIR.exists():
            return []
        return [d.name for d in MACROS_DIR.iterdir() if d.is_dir()]

    def ping(self):
        """Health check."""
        return {'status': 'ok', 'version': '0.1.0'}
