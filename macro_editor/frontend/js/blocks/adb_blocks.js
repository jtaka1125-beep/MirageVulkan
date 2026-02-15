// =============================================================================
// adb_blocks.js - MirageSystem カスタムブロック定義
// =============================================================================

// --- デバイス選択 ---
Blockly.defineBlocksWithJsonArray([
{
    "type": "adb_device_select",
    "message0": "デバイス %1",
    "args0": [{
        "type": "field_dropdown",
        "name": "DEVICE",
        "options": [["自動（最初のデバイス）", "auto"]]
    }],
    "output": "String",
    "colour": 290,
    "tooltip": "操作対象デバイスを選択"
},

// --- タッチ操作 ---
{
    "type": "adb_tap",
    "message0": "タップ  x: %1  y: %2",
    "args0": [
        {"type": "field_number", "name": "X", "value": 540, "min": 0},
        {"type": "field_number", "name": "Y", "value": 960, "min": 0}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 160,
    "tooltip": "指定座標をタップ"
},
{
    "type": "adb_swipe",
    "message0": "スワイプ  (%1, %2) → (%3, %4)  %5 ms",
    "args0": [
        {"type": "field_number", "name": "START_X", "value": 540, "min": 0},
        {"type": "field_number", "name": "START_Y", "value": 1500, "min": 0},
        {"type": "field_number", "name": "END_X", "value": 540, "min": 0},
        {"type": "field_number", "name": "END_Y", "value": 500, "min": 0},
        {"type": "field_number", "name": "DURATION", "value": 300, "min": 0}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 160,
    "tooltip": "指定座標間をスワイプ"
},
{
    "type": "adb_long_press",
    "message0": "ロングプレス  x: %1  y: %2  %3 ms",
    "args0": [
        {"type": "field_number", "name": "X", "value": 540, "min": 0},
        {"type": "field_number", "name": "Y", "value": 960, "min": 0},
        {"type": "field_number", "name": "DURATION", "value": 1000, "min": 100}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 160,
    "tooltip": "指定座標をロングプレス"
},

// --- 入力 ---
{
    "type": "adb_type_text",
    "message0": "テキスト入力 %1",
    "args0": [{"type": "field_input", "name": "TEXT", "text": "hello"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 45,
    "tooltip": "テキストを入力"
},
{
    "type": "adb_keyevent",
    "message0": "キー %1",
    "args0": [{
        "type": "field_dropdown",
        "name": "KEY",
        "options": [
            ["HOME", "KEYCODE_HOME"],
            ["BACK", "KEYCODE_BACK"],
            ["ENTER", "KEYCODE_ENTER"],
            ["TAB", "KEYCODE_TAB"],
            ["DELETE", "KEYCODE_DEL"],
            ["POWER", "KEYCODE_POWER"],
            ["VOLUME UP", "KEYCODE_VOLUME_UP"],
            ["VOLUME DOWN", "KEYCODE_VOLUME_DOWN"],
            ["メニュー", "KEYCODE_MENU"],
            ["ESC", "KEYCODE_ESCAPE"]
        ]
    }],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 45,
    "tooltip": "キーイベント送信"
},

// --- アプリ制御 ---
{
    "type": "adb_launch_app",
    "message0": "アプリ起動 %1",
    "args0": [{"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 330,
    "tooltip": "アプリを起動"
},
{
    "type": "adb_force_stop",
    "message0": "アプリ停止 %1",
    "args0": [{"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 330,
    "tooltip": "アプリを強制停止"
},

// --- 画面 ---
{
    "type": "adb_screenshot",
    "message0": "スクリーンショット 📸 %1",
    "args0": [{"type": "field_input", "name": "FILENAME", "text": "screenshot.png"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 200,
    "tooltip": "スクリーンショットを保存"
},
{
    "type": "adb_wait_for_text",
    "message0": "テキスト %1 を待つ（最大 %2 秒）",
    "args0": [
        {"type": "field_input", "name": "TEXT", "text": "OK"},
        {"type": "field_number", "name": "TIMEOUT", "value": 30, "min": 1}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 200,
    "tooltip": "画面にテキストが表示されるまで待機"
},

// --- フロー制御 ---
{
    "type": "adb_wait",
    "message0": "⏱ %1 秒待つ",
    "args0": [{"type": "field_number", "name": "SECONDS", "value": 1, "min": 0.1, "precision": 0.1}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 120,
    "tooltip": "指定秒数待機"
},
{
    "type": "adb_repeat",
    "message0": "🔄 %1 回繰り返す",
    "args0": [{"type": "field_number", "name": "TIMES", "value": 3, "min": 1}],
    "message1": "%1",
    "args1": [{"type": "input_statement", "name": "DO"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 120,
    "tooltip": "指定回数繰り返す"
},
{
    "type": "adb_log",
    "message0": "📋 ログ: %1",
    "args0": [{"type": "field_input", "name": "MESSAGE", "text": "ステップ完了"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 60,
    "tooltip": "ログメッセージを出力"
}
]);
