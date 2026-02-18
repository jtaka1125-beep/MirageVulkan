// =============================================================================
// ocr_blocks.js - OCR (H264 Frame Analysis) ブロック定義
// =============================================================================
Blockly.defineBlocksWithJsonArray([
{
    "type": "ocr_tap_text",
    "message0": "🔍 OCR テキストをタップ %1",
    "args0": [{"type": "field_input", "name": "TEXT", "text": "ログイン"}],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 30,
    "tooltip": "H264フレームからOCRでテキストを検索し、見つかった位置をタップ"
},
{
    "type": "ocr_has_text",
    "message0": "🔍 OCR テキストが存在 %1",
    "args0": [{"type": "field_input", "name": "TEXT", "text": "OK"}],
    "output": "Boolean",
    "colour": 30,
    "tooltip": "H264フレームからOCRでテキストの存在を確認（true/false）"
},
{
    "type": "ocr_if_text",
    "message0": "🔍 OCR テキストがあれば %1 %2 実行 %3",
    "args0": [
        {"type": "field_input", "name": "TEXT", "text": "OK"},
        {"type": "input_dummy"},
        {"type": "input_statement", "name": "DO"}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 30,
    "tooltip": "OCRでテキスト検出時にブロックを実行"
},
{
    "type": "ocr_if_text_else",
    "message0": "🔍 OCR テキストがあれば %1 %2 実行 %3 なければ %4",
    "args0": [
        {"type": "field_input", "name": "TEXT", "text": "成功"},
        {"type": "input_dummy"},
        {"type": "input_statement", "name": "DO"},
        {"type": "input_statement", "name": "ELSE"}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 30,
    "tooltip": "OCRでテキスト検出の有無で分岐"
},
{
    "type": "ocr_wait_for_text",
    "message0": "🔍 OCR テキスト待機 %1  タイムアウト %2 秒",
    "args0": [
        {"type": "field_input", "name": "TEXT", "text": "完了"},
        {"type": "field_number", "name": "TIMEOUT", "value": 10, "min": 1}
    ],
    "previousStatement": null,
    "nextStatement": null,
    "colour": 30,
    "tooltip": "OCRでテキストが表示されるまで待機"
},
{
    "type": "ocr_analyze",
    "message0": "🔍 OCR 全テキスト取得",
    "output": "String",
    "colour": 30,
    "tooltip": "H264フレームから全テキストをOCR抽出"
}
]);
