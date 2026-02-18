// ADB operation blocks + Screen Analysis blocks
Blockly.defineBlocksWithJsonArray([
  // ==================== Touch Operations ====================
  {
    "type": "adb_tap",
    "message0": "タップ x: %1 y: %2",
    "args0": [
      {"type": "field_number", "name": "X", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y", "value": 960, "min": 0}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 160,
    "tooltip": "指定座標をタップ"
  },
  {
    "type": "adb_swipe",
    "message0": "スワイプ (%1,%2) → (%3,%4) %5ms",
    "args0": [
      {"type": "field_number", "name": "X1", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y1", "value": 1500, "min": 0},
      {"type": "field_number", "name": "X2", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y2", "value": 500, "min": 0},
      {"type": "field_number", "name": "DURATION", "value": 300, "min": 0}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 160,
    "tooltip": "スワイプ操作"
  },
  {
    "type": "adb_long_press",
    "message0": "ロングプレス x: %1 y: %2 %3ms",
    "args0": [
      {"type": "field_number", "name": "X", "value": 540, "min": 0},
      {"type": "field_number", "name": "Y", "value": 960, "min": 0},
      {"type": "field_number", "name": "DURATION", "value": 1000, "min": 100}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 160,
    "tooltip": "長押し操作"
  },
  // ==================== Input ====================
  {
    "type": "adb_keyevent",
    "message0": "キー %1",
    "args0": [
      {"type": "field_dropdown", "name": "KEY", "options": [
        ["HOME", "3"], ["BACK", "4"], ["ENTER", "66"],
        ["POWER", "26"], ["VOLUME_UP", "24"], ["VOLUME_DOWN", "25"],
        ["TAB", "61"], ["DELETE", "67"], ["MENU", "82"]
      ]}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 45,
    "tooltip": "キーイベント送信"
  },
  {
    "type": "adb_type_text",
    "message0": "テキスト入力 %1",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "hello"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 45,
    "tooltip": "テキストを入力"
  },
  // ==================== App ====================
  {
    "type": "adb_launch_app",
    "message0": "アプリ起動 %1",
    "args0": [
      {"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 330,
    "tooltip": "パッケージ名でアプリ起動"
  },
  {
    "type": "adb_force_stop",
    "message0": "アプリ停止 %1",
    "args0": [
      {"type": "field_input", "name": "PACKAGE", "text": "com.android.settings"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 330,
    "tooltip": "アプリを強制停止"
  },
  // ==================== Screen ====================
  {
    "type": "adb_screenshot",
    "message0": "スクリーンショット保存 %1",
    "args0": [
      {"type": "field_input", "name": "FILENAME", "text": "screenshot.png"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 200,
    "tooltip": "スクリーンショットを撮影・保存"
  },
  // ==================== Screen Analysis (Phase 6) ====================
  {
    "type": "adb_if_text",
    "message0": "画面に「%1」が表示されたら",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "OK"}
    ],
    "message1": "実行 %1",
    "args1": [{"type": "input_statement", "name": "DO"}],
    "previousStatement": null, "nextStatement": null, "colour": 210,
    "tooltip": "画面テキストによる条件分岐（uiautomator使用）"
  },
  {
    "type": "adb_find_and_tap",
    "message0": "🔍 テキスト「%1」を探してタップ",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "OK"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 210,
    "tooltip": "画面上のテキストを検索し、見つかったらその座標をタップ"
  },
  {
    "type": "adb_wait_for_text",
    "message0": "🔍 テキスト「%1」が表示されるまで最大 %2 秒待機",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "完了"},
      {"type": "field_number", "name": "TIMEOUT", "value": 10, "min": 1, "max": 60}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 210,
    "tooltip": "指定テキストが画面に現れるまでポーリング待機"
  },
  {
    "type": "adb_if_text_else",
    "message0": "画面に「%1」があれば",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "エラー"}
    ],
    "message1": "実行 %1",
    "args1": [{"type": "input_statement", "name": "DO"}],
    "message2": "なければ %1",
    "args2": [{"type": "input_statement", "name": "ELSE"}],
    "previousStatement": null, "nextStatement": null, "colour": 210,
    "tooltip": "画面テキストの有無で分岐（if/else）"
  },
  {
    "type": "adb_tap_element",
    "message0": "🎯 要素タップ ID: %1",
    "args0": [
      {"type": "field_input", "name": "RES_ID", "text": "com.app:id/button_ok"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 280,
    "tooltip": "resource-idで要素を特定してタップ（uiautomator検索）"
  },
  {
    "type": "adb_assert_text",
    "message0": "✅ テキスト「%1」が表示されていることを確認",
    "args0": [
      {"type": "field_input", "name": "TEXT", "text": "成功"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 65,
    "tooltip": "画面にテキストが存在するか検証。なければエラー停止"
  },
  // ==================== Control ====================
  {
    "type": "adb_wait",
    "message0": "待機 %1 秒",
    "args0": [
      {"type": "field_number", "name": "SECONDS", "value": 1, "min": 0.1, "precision": 0.1}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 120,
    "tooltip": "指定秒数待機"
  },
  {
    "type": "adb_repeat",
    "message0": "%1 回繰り返す",
    "args0": [
      {"type": "field_number", "name": "TIMES", "value": 3, "min": 1}
    ],
    "message1": "実行 %1",
    "args1": [{"type": "input_statement", "name": "DO"}],
    "previousStatement": null, "nextStatement": null, "colour": 120,
    "tooltip": "指定回数繰り返し"
  },
  // ==================== Utility ====================
  {
    "type": "adb_log",
    "message0": "ログ出力 %1",
    "args0": [
      {"type": "field_input", "name": "MSG", "text": "ステップ完了"}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 60,
    "tooltip": "ログメッセージを出力"
  },
  {
    "type": "adb_container",
    "message0": "📦 %1",
    "args0": [
      {"type": "field_input", "name": "NAME", "text": "操作グループ"}
    ],
    "message1": "%1",
    "args1": [{"type": "input_statement", "name": "STEPS"}],
    "previousStatement": null, "nextStatement": null, "colour": 230,
    "tooltip": "操作をグループ化。折りたたみ可能。"
  },
  {
    "type": "adb_screen_record",
    "message0": "🎬 画面録画 %1 秒",
    "args0": [
      {"type": "field_number", "name": "DURATION", "value": 10, "min": 1, "max": 180}
    ],
    "previousStatement": null, "nextStatement": null, "colour": 0,
    "tooltip": "端末画面を録画（mp4）"
  }
]);
