// Python generators for AOA blocks
// Generated code calls MirageApiDevice (JSON-RPC -> MacroApiServer -> HybridCommandSender)
(function() {
  var python = Blockly.Python;

  // ── helper: API client import (once per script) ─────────────────────────
  var API_IMPORT = [
    'from mirage_api_device import MirageApiDevice',
  ].join('\n');

  function ensureApiImport() {
    python.definitions_['mirage_api_device'] = API_IMPORT;
    // device object reuse - init once
    python.definitions_['mirage_device_init'] =
      'device = MirageApiDevice(device_id=DEVICE_ID, host="127.0.0.1", port=19840)';
  }

  // ── タッチ操作 ────────────────────────────────────────────────────────────
  python.forBlock['aoa_tap'] = function(block) {
    ensureApiImport();
    return 'device.aoa_tap(' + block.getFieldValue('X') + ', ' + block.getFieldValue('Y') + ')\n';
  };

  python.forBlock['aoa_swipe'] = function(block) {
    ensureApiImport();
    return 'device.aoa_swipe('
      + block.getFieldValue('X1') + ', ' + block.getFieldValue('Y1') + ', '
      + block.getFieldValue('X2') + ', ' + block.getFieldValue('Y2') + ', '
      + block.getFieldValue('DURATION') + ')\n';
  };

  python.forBlock['aoa_long_press'] = function(block) {
    ensureApiImport();
    return 'device.aoa_long_press('
      + block.getFieldValue('X') + ', ' + block.getFieldValue('Y') + ', '
      + block.getFieldValue('DURATION') + ')\n';
  };

  python.forBlock['aoa_multi_touch'] = function(block) {
    ensureApiImport();
    return 'device.aoa_multi_touch('
      + block.getFieldValue('X1') + ', ' + block.getFieldValue('Y1') + ', '
      + block.getFieldValue('X2') + ', ' + block.getFieldValue('Y2') + ', '
      + block.getFieldValue('DURATION') + ')\n';
  };

  python.forBlock['aoa_pinch'] = function(block) {
    ensureApiImport();
    var dir = block.getFieldValue('DIR');
    return 'device.aoa_pinch("' + dir + '", '
      + block.getFieldValue('CX') + ', ' + block.getFieldValue('CY') + ', '
      + block.getFieldValue('D1') + ', ' + block.getFieldValue('D2') + ')\n';
  };

  // ── キー操作 ──────────────────────────────────────────────────────────────
  python.forBlock['aoa_key'] = function(block) {
    ensureApiImport();
    return 'device.aoa_key(' + block.getFieldValue('KEY') + ')\n';
  };

  // ── 画面 ──────────────────────────────────────────────────────────────────
  python.forBlock['aoa_screenshot'] = function(block) {
    ensureApiImport();
    var fn = block.getFieldValue('FILENAME').replace(/'/g, "\\'");
    return "device.aoa_screenshot('" + fn + "')\n";
  };

  python.forBlock['aoa_if_text'] = function(block) {
    ensureApiImport();
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var body = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    return "if device.aoa_has_text('" + text + "'):\n" + body;
  };

  // ── アプリ ────────────────────────────────────────────────────────────────
  python.forBlock['aoa_launch_app'] = function(block) {
    ensureApiImport();
    var pkg = block.getFieldValue('PACKAGE').replace(/'/g, "\\'");
    return "device.aoa_launch_app('" + pkg + "')\n";
  };

  python.forBlock['aoa_force_stop'] = function(block) {
    ensureApiImport();
    var pkg = block.getFieldValue('PACKAGE').replace(/'/g, "\\'");
    return "device.aoa_force_stop('" + pkg + "')\n";
  };

})();
