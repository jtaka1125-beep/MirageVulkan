#!/usr/bin/env python3
"""
Ollama Vision Wrapper for Popup Detection (Layer 3)
====================================================
llava:7b を使用してスクリーンショットからポップアップ/ダイアログを検出し、
閉じるボタンの座標を返す。

Usage:
    # CLI
    python ollama_vision.py screenshot.png

    # Python API
    from ollama_vision import detect_popup
    result = detect_popup("screenshot.png")
"""

import base64
import json
import sys
import time
from pathlib import Path
from typing import Optional, TypedDict

import requests

OLLAMA_URL = "http://localhost:11434/api/generate"
MODEL = "llava:7b"
TIMEOUT = 120  # seconds (初回ロード時間含む)

# ポップアップ検出用プロンプト (日本語/英語両対応)
POPUP_DETECTION_PROMPT = """Look at this Android screenshot carefully.

Task: Find any popup dialog, modal, alert, or overlay that blocks the main content.

If you find a popup/dialog:
1. Describe what kind of popup it is (ad, permission request, error, notification, etc.)
2. Find the close/dismiss button (usually: X, 閉じる, OK, Cancel, キャンセル, 後で, Skip, etc.)
3. Return the button's approximate position as percentage of screen (x%, y%)

Output ONLY valid JSON in this exact format:
{"found": true, "type": "ad/permission/error/notification/other", "button_text": "X", "x_percent": 85, "y_percent": 15}

If NO popup/dialog found:
{"found": false}

IMPORTANT: Output ONLY the JSON, no explanation."""


class PopupResult(TypedDict):
    found: bool
    type: Optional[str]
    button_text: Optional[str]
    x_percent: Optional[int]
    y_percent: Optional[int]
    raw_response: str
    elapsed_ms: int


def encode_image_base64(image_path: str) -> str:
    """画像をBase64エンコード"""
    with open(image_path, "rb") as f:
        return base64.b64encode(f.read()).decode("utf-8")


def detect_popup(image_path: str, prompt: str = POPUP_DETECTION_PROMPT) -> PopupResult:
    """
    スクリーンショットからポップアップを検出

    Args:
        image_path: 画像ファイルパス (PNG/JPG)
        prompt: カスタムプロンプト (省略時はデフォルト)

    Returns:
        PopupResult: 検出結果
    """
    start = time.perf_counter()

    # 画像をBase64エンコード
    image_base64 = encode_image_base64(image_path)

    # Ollama API呼び出し
    payload = {
        "model": MODEL,
        "prompt": prompt,
        "images": [image_base64],
        "stream": False,
        "options": {
            "temperature": 0.1,  # 決定論的に
            "num_predict": 200,  # 短い応答で十分
        }
    }

    try:
        response = requests.post(OLLAMA_URL, json=payload, timeout=TIMEOUT)
        response.raise_for_status()
        data = response.json()
        raw_response = data.get("response", "")
    except requests.exceptions.RequestException as e:
        elapsed = int((time.perf_counter() - start) * 1000)
        return {
            "found": False,
            "type": None,
            "button_text": None,
            "x_percent": None,
            "y_percent": None,
            "raw_response": f"ERROR: {e}",
            "elapsed_ms": elapsed,
        }

    elapsed = int((time.perf_counter() - start) * 1000)

    # JSONパース試行
    result: PopupResult = {
        "found": False,
        "type": None,
        "button_text": None,
        "x_percent": None,
        "y_percent": None,
        "raw_response": raw_response,
        "elapsed_ms": elapsed,
    }

    # JSON部分を抽出 (LLMが余計なテキストを付けることがある)
    try:
        # 最初の { から最後の } までを抽出
        json_start = raw_response.find("{")
        json_end = raw_response.rfind("}") + 1
        if json_start >= 0 and json_end > json_start:
            json_str = raw_response[json_start:json_end]
            parsed = json.loads(json_str)

            result["found"] = parsed.get("found", False)
            if result["found"]:
                result["type"] = parsed.get("type")
                result["button_text"] = parsed.get("button_text")
                result["x_percent"] = parsed.get("x_percent")
                result["y_percent"] = parsed.get("y_percent")
    except json.JSONDecodeError:
        pass  # パース失敗時はfound=Falseのまま

    return result


def main():
    """CLI エントリポイント"""
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <screenshot.png>")
        sys.exit(1)

    image_path = sys.argv[1]
    if not Path(image_path).exists():
        print(f"Error: File not found: {image_path}")
        sys.exit(1)

    print(f"Analyzing: {image_path}")
    print(f"Model: {MODEL}")
    print("-" * 40)

    result = detect_popup(image_path)

    print(f"Elapsed: {result['elapsed_ms']}ms")
    print(f"Found popup: {result['found']}")

    if result["found"]:
        print(f"  Type: {result['type']}")
        print(f"  Button: {result['button_text']}")
        print(f"  Position: ({result['x_percent']}%, {result['y_percent']}%)")

    print("-" * 40)
    print(f"Raw response:\n{result['raw_response']}")

    # JSON出力 (パイプ用)
    if sys.stdout.isatty():
        pass  # 対話モードでは上記の出力で十分
    else:
        # パイプ時はJSONのみ
        print(json.dumps(result, ensure_ascii=False))


if __name__ == "__main__":
    main()
