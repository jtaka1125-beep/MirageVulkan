// Python code generators for OCR blocks
(function() {
  var python = Blockly.Python;

  python.forBlock['ocr_tap_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    return "device.ocr_tap_text('" + text + "')\n";
  };

  python.forBlock['ocr_has_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    return ["device.ocr_has_text('" + text + "')", python.ORDER_FUNCTION_CALL];
  };

  python.forBlock['ocr_if_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var body = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    return "if device.ocr_has_text('" + text + "'):\n" + body;
  };

  python.forBlock['ocr_if_text_else'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var doBody = python.statementToCode(block, 'DO') || python.INDENT + 'pass\n';
    var elseBody = python.statementToCode(block, 'ELSE') || python.INDENT + 'pass\n';
    return "if device.ocr_has_text('" + text + "'):\n" + doBody
      + "else:\n" + elseBody;
  };

  python.forBlock['ocr_wait_for_text'] = function(block) {
    var text = block.getFieldValue('TEXT').replace(/'/g, "\\'");
    var timeout = block.getFieldValue('TIMEOUT');
    return "device.ocr_wait_for_text('" + text + "', timeout_sec=" + timeout + ")\n";
  };

  python.forBlock['ocr_analyze'] = function(block) {
    return ["device.ocr_analyze().get('full_text', '')", python.ORDER_FUNCTION_CALL];
  };
})();
