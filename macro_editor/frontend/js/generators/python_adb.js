// Python code generators for ADB blocks + Screen Analysis
(function() {
  var python = Blockly.Python;

  python.forBlock['adb_tap'] = function(block) {
    return 'device.tap(' + block.getFieldValue('X') + ', ' + block.getFieldValue('Y') + ')\n';
  };
  python.forBlock['adb_swipe'] = function(block) {
    return 'device.swipe(' + block.getFieldValue('X1') + ', ' + block.getFieldValue('Y1') + ', '
      + block.getFieldValue('X2') + ', ' + block.getFieldValue('Y2') + ', '
      + block.getFieldValue('DURATION') + ')\n';
  };
  python.forBlock['adb_long_press'] = function(block) {
    return 'device.long_press(' + block.getFieldValue('X') + ', ' + block.getFieldValue('Y')
      + ', ' + block.getFieldValue('DURATION') + ')\n';
  };
  python.forBlock['adb_wait'] = function(block) {
    python.definitions_['import_time'] = 'import time';
    return 'time.sleep(' + block.getFieldValue('SECONDS') + ')\n';
  };
  python.forBlock['adb_keyevent'] = function(block) {
    return 'device.key(' + block.getFieldValue('KEY') + ')\n';
  };
  python.forBlock['adb_type_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    return "device.text('" + text + "')\n";
  };
  python.forBlock['adb_launch_app'] = function(block) {
    return "device.launch_app('" + block.getFieldValue('PACKAGE') + "')\n";
  };
  python.forBlock['adb_force_stop'] = function(block) {
    return "device.force_stop('" + block.getFieldValue('PACKAGE') + "')\n";
  };
  python.forBlock['adb_screenshot'] = function(block) {
    return "device.screenshot('" + block.getFieldValue('FILENAME') + "')\n";
  };
  python.forBlock['adb_log'] = function(block) {
    python.definitions_['import_logging'] = 'import logging\nlog = logging.getLogger(__name__)';
    var msg = block.getFieldValue('MSG').replace(/'/g, "\\'");
    return "log.info('" + msg + "')\n";
  };
  python.forBlock['adb_repeat'] = function(block) {
    var body = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    return 'for _i in range(' + block.getFieldValue('TIMES') + '):\n' + body;
  };
  python.forBlock['adb_container'] = function(block) {
    var name = block.getFieldValue('NAME').replace(/'/g, "\\'");
    var body = python.statementToCode(block, 'STEPS') || python.INDENT + 'pass\n';
    return '# --- ' + name + ' ---\n' + body + '# --- end ---\n';
  };
  python.forBlock['adb_screen_record'] = function(block) {
    return "device.screen_record(" + block.getFieldValue('DURATION') + ")\n";
  };

  // ==================== Screen Analysis (Phase 6) ====================

  python.forBlock['adb_if_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var body = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    return "if device.screen_contains_text('" + text + "'):\n" + body;
  };

  python.forBlock['adb_find_and_tap'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    return "device.find_and_tap_text('" + text + "')\n";
  };

  python.forBlock['adb_wait_for_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var timeout = block.getFieldValue('TIMEOUT');
    return "device.wait_for_text('" + text + "', timeout_sec=" + timeout + ")\n";
  };

  python.forBlock['adb_if_text_else'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var doBody = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    var elseBody = python.statementToCode(block, 'ELSE') || python.INDENT + 'pass\n';
    return "if device.screen_contains_text('" + text + "'):\n" + doBody
      + "else:\n" + elseBody;
  };

  python.forBlock['adb_tap_element'] = function(block) {
    var resId = block.getFieldValue('RES_ID').replace(/'/g, "\\'");
    return "device.tap_element('" + resId + "')\n";
  };

  python.forBlock['adb_assert_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    return "assert device.screen_contains_text('" + text + "'), 'テキスト未検出: " + text + "'\n";
  };
})();
