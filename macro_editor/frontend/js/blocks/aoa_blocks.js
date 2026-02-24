// AOA operation blocks (via MirageGUI MacroApiServer on localhost:19840)
// Color: Blue family (200-240) to distinguish from ADB green blocks
Blockly.defineBlocksWithJsonArray([
  // ── タッチ操作 ──────────────────────────────────────────
  {
    "type": "aoa_tap",
    "message0": "⚡AOA タップ x: %1 y: %2",
    "args0": [
      {"type": "field_number", "name": "X", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y", "value": 960, "min": 0}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 220,
    "tooltip": "AOA HIDタップ（ADB fallback付き）。MirageGUI必須。"
  },
  {
    "type": "aoa_swipe",
    "message0": "⚡AOA スワイプ (%1,%2) → (%3,%4) %5ms",
    "args0": [
      {"type": "field_number", "name": "X1", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y1", "value": 1500, "min": 0},
      {"type": "field_number", "name": "X2", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y2", "value": 500, "min": 0},
      {"type": "field_number", "name": "DURATION", "value": 300, "min": 0}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 220,
    "tooltip": "AOA HIDスワイプ。MirageGUI必須。"
  },
  {
    "type": "aoa_long_press",
    "message0": "⚡AOA ロングプレス x: %1 y: %2 %3ms",
    "args0": [
      {"type": "field_number", "name": "X", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y", "value": 960, "min": 0},
      {"type": "field_number", "name": "DURATION", "value": 1000, "min": 100}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 220,
    "tooltip": "AOA HID長押し。MirageGUI必須。"
  },
  {
    "type": "aoa_multi_touch",
    "message0": "⚡AOA マルチタッチ 指1(%1,%2) 指2(%3,%4) %5ms",
    "args0": [
      {"type": "field_number", "name": "X1", "value": 300, "min": 0},
      {"type": "field_number", "name": "Y1", "value": 960, "min": 0},
      {"type": "field_number", "name": "X2", "value": 780, "min": 0},
      {"type": "field_number", "name": "Y2", "value": 960, "min": 0},
      {"type": "field_number", "name": "DURATION", "value": 200, "min": 0}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 220,
    "tooltip": "AOA HID 2指同時タッチ（ピンチ・ズーム等）。ADB不可の操作。"
  },
  {
    "type": "aoa_pinch",
    "message0": "⚡AOA ピンチ %1 中心(%2,%3) 距離%4→%5px",
    "args0": [
      {"type": "field_dropdown", "name": "DIR", "options": [
        ["イン（縮小）", "in"], ["アウト（拡大）", "out"]
      ]},
      {"type": "field_number", "name": "CX", "value": 540, "min": 0},
      {"type": "field_number", "name": "CY", "value": 960, "min": 0},
      {"type": "field_number", "name": "D1", "value": 400, "min": 10},
      {"type": "field_number", "name": "D2", "value": 100, "min": 10}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 220,
    "tooltip": "AOA HID ピンチイン/アウト（2指）。ADB不可の操作。"
  },
  // ── キー操作 ─────────────────────────────────────────────
  {
    "type": "aoa_key",
    "message0": "⚡AOA キー %1",
    "args0": [
      {"type": "field_dropdown", "name": "KEY", "options": [
        ["HOME", "3"], ["BACK", "4"], ["ENTER", "66"],
        ["POWER", "26"], ["VOLUME_UP", "24"], ["VOLUME_DOWN", "25"],
        ["TAB", "61"], ["DELETE", "67"], ["MENU", "82"],
        ["APP_SWITCH", "187"], ["BRIGHTNESS_UP", "221"], ["BRIGHTNESS_DOWN", "220"],
        ["SCREENSHOT", "120"], ["DPAD_UP", "19"], ["DPAD_DOWN", "20"],
        ["DPAD_LEFT", "21"], ["DPAD_RIGHT", "22"]
      ]}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 200,
    "tooltip": "AOA HIDキーイベント。MirageGUI必須。"
  },
  // ── 画面・スクリーンショット ──────────────────────────────
  {
    "type": "aoa_screenshot",
    "message0": "⚡AOA スクリーンショット → %1",
    "args0": [
      {"type": "field_input", "name": "FILENAME", "text": "screenshot.png"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 190,
    "tooltip": "AOA経由スクリーンショット取得。MirageGUI必須。"
  },
  {
    "type": "aoa_if_text",
    "message0": "⚡AOA 画面に「%1」があれば",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "OK"}
    ],
    "message1": "実行 %1",
    "args1": [{"type": "input_statement", "name": "DO"}],
    "previousStatement": null, "nextStatement": null, "colour": 190,
    "tooltip": "AOA OCRで画面テキスト検索。ADB fallback付き。MirageGUI必須。"
  },
  // ── アプリ操作 ────────────────────────────────────────────
  {
    "type": "aoa_launch_app",
    "message0": "⚡AOA アプリ起動 %1",
    "args0": [
      {"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 270,
    "tooltip": "AOA経由アプリ起動（HybridCommandSender）。MirageGUI必須。"
  },
  {
    "type": "aoa_force_stop",
    "message0": "⚡AOA アプリ停止 %1",
    "args0": [
      {"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 270,
    "tooltip": "AOA経由アプリ強制停止。MirageGUI必須。"
  }
]);
