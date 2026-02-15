// Blockly toolbox definition
var TOOLBOX = {
  "kind": "categoryToolbox",
  "contents": [
    {
      "kind": "category", "name": "🎯 録画・コンテナ", "colour": "230",
      "contents": [
        {"kind": "block", "type": "adb_container"},
        {"kind": "block", "type": "adb_screen_record"}
      ]
    },
    {"kind": "sep"},
    {
      "kind": "category", "name": "👆 タッチ操作", "colour": "160",
      "contents": [
        {"kind": "block", "type": "adb_tap"},
        {"kind": "block", "type": "adb_swipe"},
        {"kind": "block", "type": "adb_long_press"}
      ]
    },
    {
      "kind": "category", "name": "⌨️ 入力", "colour": "45",
      "contents": [
        {"kind": "block", "type": "adb_keyevent"},
        {"kind": "block", "type": "adb_type_text"}
      ]
    },
    {
      "kind": "category", "name": "📦 アプリ", "colour": "330",
      "contents": [
        {"kind": "block", "type": "adb_launch_app"},
        {"kind": "block", "type": "adb_force_stop"}
      ]
    },
    {
      "kind": "category", "name": "🖥️ 画面", "colour": "200",
      "contents": [
        {"kind": "block", "type": "adb_screenshot"},
        {"kind": "block", "type": "adb_if_text"}
      ]
    },
    {
      "kind": "category", "name": "🔄 制御", "colour": "120",
      "contents": [
        {"kind": "block", "type": "adb_wait"},
        {"kind": "block", "type": "adb_repeat"}
      ]
    },
    {
      "kind": "category", "name": "📋 ログ", "colour": "60",
      "contents": [
        {"kind": "block", "type": "adb_log"}
      ]
    },
    {"kind": "sep"},
    {"kind": "category", "name": "変数", "custom": "VARIABLE", "colour": "330"},
    {"kind": "category", "name": "関数", "custom": "PROCEDURE", "colour": "290"}
  ]
};
