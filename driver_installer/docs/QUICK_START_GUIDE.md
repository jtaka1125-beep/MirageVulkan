# MirageUnified - クイックスタートガイド

**所要時間:** 5分（GUI使用）/ 10分（CLI使用）

---

## 📋 前提条件

| 項目 | 要件 |
|------|------|
| OS | Windows 10/11 (64-bit) |
| Python | 3.8以上 |
| 権限 | 管理者権限 |
| ハードウェア | AOA対応Androidデバイス + USBケーブル |

---

## 🚀 Step 1: インストール（2分）

### 1.1 依存パッケージ

```bash
pip install PyQt5
```

### 1.2 wdi-simple.exe（オプション）

WDI方式を使用する場合は、`tools/wdi/` に `wdi-simple.exe` を配置:

```
MirageUnified/
└── tools/
    └── wdi/
        └── wdi-simple.exe  ← ここに配置
```

> **Note:** wdi-simple.exe がない場合は pnputil 方式にフォールバックします

---

## 🖥️ Step 2: GUI で実行（推奨）

### 2.1 ウィザード起動

```bash
cd MirageUnified
python -m ui.mirage_driver_installer_wizard
```

### 2.2 ウィザードの流れ

```
[1] Welcome        → 説明を確認
[2] Environment    → 管理者権限・WDI確認
[3] Device         → Androidデバイス検出
[4] Install        → ドライバインストール
[5] Verification   → WinUSBサービス確認
[6] Complete       → 完了！
```

### 2.3 スクリーンショット

```
┌─────────────────────────────────────────────┐
│  MirageSystem v2 - AOA Driver Setup Wizard  │
├─────────────────────────────────────────────┤
│                                             │
│  ✓ Device detected                          │
│  ✓ Service = WinUSB                         │
│  ✓ Driver verified                          │
│                                             │
│  [Rollback Driver]              [Close]     │
└─────────────────────────────────────────────┘
```

---

## ⌨️ Step 3: CLI で実行（上級者向け）

### 3.1 ステータス確認

```bash
python -m core.driver.setup_orchestrator --check
```

出力例:
```
============================================================
Device & Driver Status
============================================================
  ✓ AOA Device Connected
  ✓ WinUSB Service OK
  ✓ Driver Flag Exists
  ✗ WDI Mode Available
============================================================

Driver Details:
  Service:  WinUSB
  Provider: libwdi
  Version:  1.0.0.0
  Driver:   oem123.inf
```

### 3.2 インストール

```bash
python -m core.driver.setup_orchestrator --install
```

### 3.3 ロールバック

```bash
python -m core.driver.setup_orchestrator --rollback
```

### 3.4 対話式ウィザード

```bash
python -m core.driver.setup_orchestrator --wizard
```

---

## 🔧 トラブルシューティング

### デバイスが検出されない

1. USBケーブルを確認（データ転送対応か？）
2. Androidで「USBデバッグ」を有効化
3. AOAモードに切り替え

```bash
# PowerShellでデバイス確認
Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*VID_18D1*" }
```

### インストール失敗

1. **署名エラー**: テストモードを有効化
   ```cmd
   bcdedit /set testsigning on
   ```
   → 再起動必要

2. **アクセス拒否**: 管理者として実行

3. **INFファイル不足**: pnputil方式では `android_accessory_interface.inf` が必要

### ロールバックしたい

GUI:
- 完了画面の「Rollback Driver」ボタン

CLI:
```bash
python -m core.driver.setup_orchestrator --rollback
```

バッチファイル:
```cmd
core\driver\rollback_aoa_driver.bat
```

---

## 📁 ファイル構成

```
MirageUnified/
├── core/driver/
│   ├── enums.py                 # 共通Enum定義
│   ├── driver_controller.py     # GUI用バックエンド
│   ├── setup_orchestrator.py    # CLI用オーケストレータ
│   └── rollback_aoa_driver.bat  # ロールバックスクリプト
├── ui/
│   └── mirage_driver_installer_wizard.py  # PyQt5 GUI
├── docs/
│   ├── QUICK_START_GUIDE.md     # ← このファイル
│   ├── SIGNATURE_OPERATIONAL_GUIDE.md  # 署名戦略
│   └── ...
└── tools/wdi/
    └── README.txt               # wdi-simple.exe の入手方法
```

---

## 🔗 次のステップ

1. **署名について詳しく**: `docs/SIGNATURE_OPERATIONAL_GUIDE.md`
2. **本番デプロイ**: `docs/PRODUCTION_DEPLOYMENT_CHECKLIST.md`
3. **GUI統合ガイド**: `docs/UI_INTEGRATION_GUIDE.md`

---

## ❓ FAQ

**Q: WDI方式とpnputil方式の違いは？**

| 項目 | WDI | pnputil |
|------|-----|---------|
| 署名 | 自己署名可 | 要テストモード |
| INFファイル | 自動生成 | 手動準備 |
| 推奨環境 | 開発・テスト | 本番（署名済み） |

**Q: MirageSystem v2 本体との関係は？**

```
[MirageUnified]     →  ドライバインストーラー（このツール）
       ↓
[MirageSystem v2]   →  本体アプリ（タップ制御等）
```

MirageUnified でドライバをセットアップ後、MirageSystem v2 でデバイス制御を行います。

---

**Happy Hacking! 🎉**
