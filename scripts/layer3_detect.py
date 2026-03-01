#!/usr/bin/env python3
"""
Layer 3 Popup Detection CLI
============================
C++から呼び出せる簡易ラッパー。
VisionDecisionEngineのERROR_RECOVERY状態から呼び出される想定。

Usage:
    python layer3_detect.py <device_serial> [--auto-register]

Output (JSON):
    {"found": true, "x_percent": 85, "y_percent": 15, "button_text": "X", ...}
    {"found": false}

Exit codes:
    0: Success (result in stdout as JSON)
    1: Error (message in stderr)
"""

import json
import sys
import urllib.request
import urllib.error

MCP_URL = "http://localhost:3000/mcp"


def call_detect_popup(device: str, auto_register: bool = False) -> dict:
    """MCP経由でdetect_popupを呼び出し"""
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {
            "name": "detect_popup",
            "arguments": {
                "device": device,
                "auto_register": auto_register,
                "timeout": 90
            }
        }
    }

    req = urllib.request.Request(
        MCP_URL,
        data=json.dumps(payload).encode('utf-8'),
        headers={"Content-Type": "application/json"}
    )

    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            data = json.loads(resp.read().decode('utf-8'))
    except urllib.error.URLError as e:
        return {"error": f"MCP connection failed: {e}"}
    except Exception as e:
        return {"error": str(e)}

    # MCPレスポンスをパース
    if "error" in data:
        return {"error": data["error"].get("message", "Unknown MCP error")}

    result = data.get("result", {})
    content = result.get("content", [])
    if content and content[0].get("type") == "text":
        text = content[0].get("text", "{}")
        try:
            # Python dict形式をJSONに変換
            import ast
            parsed = ast.literal_eval(text)
            return parsed
        except (ValueError, SyntaxError):
            # フォールバック: 単純な置換
            try:
                text = text.replace("'", '"').replace("None", "null").replace("True", "true").replace("False", "false")
                return json.loads(text)
            except json.JSONDecodeError:
                return {"error": f"JSON parse failed: {text[:200]}"}

    return {"error": "Unexpected MCP response"}


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <device_serial> [--auto-register]", file=sys.stderr)
        sys.exit(1)

    device = sys.argv[1]
    auto_register = "--auto-register" in sys.argv

    result = call_detect_popup(device, auto_register)

    # JSON出力
    print(json.dumps(result, ensure_ascii=False))

    # エラーがあれば非ゼロ終了
    if "error" in result:
        sys.exit(1)


if __name__ == "__main__":
    main()
