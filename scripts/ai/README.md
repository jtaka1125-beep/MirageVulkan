# scripts/ai - OCR-based Pairing Automation

Android デバイスのペアリングダイアログを OCR で読み取り、自動的にペアリングを完了するスクリプト群。

## スクリプト

### auto_pairing_ocr.py
Bluetooth / WiFi ADB 両方のペアリングに対応する統合スクリプト。
画面を ADB スクリーンショットで取得し、OCR でペアリングコードを読み取る。

```bash
# WiFi ADB ペアリング（ペアリングコード + IP:port を自動読取）
python scripts/ai/auto_pairing_ocr.py wifi_adb

# Bluetooth ペアリング（PIN + Pair ボタン自動タップ）
python scripts/ai/auto_pairing_ocr.py bluetooth

# 自動検出モード
python scripts/ai/auto_pairing_ocr.py auto

# デバイス指定
python scripts/ai/auto_pairing_ocr.py wifi_adb 192.168.0.7:5555
```

### bluetooth_pairing.py
Bluetooth ペアリング専用スクリプト。IPC 経由のタップコマンドにも対応。

```bash
python scripts/ai/bluetooth_pairing.py [slot] [device]
```

## 必要な pip パッケージ

```bash
pip install pytesseract pillow opencv-python
```

## システム要件

- **Tesseract-OCR** がインストールされている必要があります
  - Windows: https://github.com/UB-Mannheim/tesseract/wiki からインストーラをダウンロード
  - インストール後、`tesseract` コマンドが PATH に通っていることを確認
- **ADB** が PATH に通っていること（または `ADB` 環境変数で指定）

## 環境変数

| 変数 | 説明 | デフォルト |
|------|------|------------|
| `MIRAGE_HOME` | MirageComplete ルートディレクトリ | スクリプト位置から自動検出 |
| `ADB` | adb 実行ファイルパス | PATH から自動検出 |
