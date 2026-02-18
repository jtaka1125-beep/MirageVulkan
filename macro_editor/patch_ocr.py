#!/usr/bin/env python3
"""Patch mirage_client.py to add OCR methods."""
import sys

filepath = "backend/mirage_client.py"
with open(filepath, "r", encoding="utf-8") as f:
    content = f.read()

# ---- 1. Add OCR methods to MirageClient (after line 91) ----
ocr_client_methods = '''
    # ---- OCR API (requires USE_OCR=ON in MirageVulkan) ----
    def ocr_analyze(self, device_id: str) -> dict:
        return self._call("ocr_analyze", {"device_id": device_id})

    def ocr_find_text(self, device_id: str, query: str) -> dict:
        return self._call("ocr_find_text", {"device_id": device_id, "query": query})

    def ocr_has_text(self, device_id: str, query: str) -> dict:
        return self._call("ocr_has_text", {"device_id": device_id, "query": query})

    def ocr_tap_text(self, device_id: str, query: str) -> dict:
        return self._call("ocr_tap_text", {"device_id": device_id, "query": query})
'''

marker1 = '    def screenshot(self, device_id: str) -> dict: return self._call("screenshot", {"device_id": device_id})\n'
if "def ocr_analyze(self, device_id" not in content:
    if marker1 in content:
        content = content.replace(marker1, marker1 + ocr_client_methods)
        print("Added OCR methods to MirageClient")
    else:
        print("ERROR: Could not find MirageClient.screenshot marker")
        sys.exit(1)
else:
    print("OCR client methods already present")

# ---- 2. Add OCR proxy methods to _DeviceProxy (before screen_record) ----
ocr_proxy_methods = '''    # ---- OCR (H264 frame analysis, no ADB needed) ----
    def ocr_analyze(self) -> dict:
        """OCR: extract all text from current H264 frame."""
        self._check()
        result = self._r.client.ocr_analyze(self._r.device_id)
        wc = result.get("word_count", 0)
        self._r.log_lines.append(f"ocr_analyze() -> {wc} words")
        return result

    def ocr_find_text(self, query: str) -> list:
        """OCR: find text matches with bounding boxes."""
        self._check()
        result = self._r.client.ocr_find_text(self._r.device_id, query)
        matches = result.get("matches", [])
        self._r.log_lines.append(f"ocr_find_text('{query}') -> {len(matches)} matches")
        return matches

    def ocr_has_text(self, query: str) -> bool:
        """OCR: check if text exists on screen (via H264 frame)."""
        self._check()
        result = self._r.client.ocr_has_text(self._r.device_id, query)
        found = result.get("found", False)
        self._r.log_lines.append(f"ocr_has_text('{query}') -> {found}")
        return found

    def ocr_tap_text(self, query: str) -> bool:
        """OCR: find text on screen and tap its center."""
        self._step()
        result = self._r.client.ocr_tap_text(self._r.device_id, query)
        found = result.get("found", False)
        if found:
            self._r.log_lines.append(f"ocr_tap_text('{query}') -> tap({result.get('x')},{result.get('y')})")
        else:
            self._r.log_lines.append(f"ocr_tap_text('{query}') -> not found")
        return found

    def ocr_wait_for_text(self, query: str, timeout_sec: int = 10, interval: float = 1.0) -> bool:
        """OCR: poll until text appears on screen."""
        self._step()
        import time as _t
        deadline = _t.time() + timeout_sec
        while _t.time() < deadline:
            self._check()
            result = self._r.client.ocr_has_text(self._r.device_id, query)
            if result.get("found", False):
                self._r.log_lines.append(f"ocr_wait_for_text('{query}') -> found")
                return True
            _t.sleep(interval)
        self._r.log_lines.append(f"ocr_wait_for_text('{query}', {timeout_sec}s) -> timeout")
        return False

'''

marker2 = '    def screen_record(self, duration: int = 10):\n'
if "def ocr_analyze(self) -> dict:" not in content:
    if marker2 in content:
        content = content.replace(marker2, ocr_proxy_methods + marker2)
        print("Added OCR proxy methods to _DeviceProxy")
    else:
        print("ERROR: Could not find _DeviceProxy.screen_record marker")
        sys.exit(1)
else:
    print("OCR proxy methods already present")

with open(filepath, "w", encoding="utf-8") as f:
    f.write(content)
print("DONE")
