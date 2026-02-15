# MirageSystem マクロエディタ 設計仕様書
# ================================================

## 1. アーキテクチャ概要

```
┌─────────────────────────────────────────────────────────┐
│  マクロエディタ (Python + pywebview)                      │
│  ┌────────────┐   ┌──────────────┐   ┌──────────────┐   │
│  │ Blockly UI │←→│ pywebview    │←→│ Python API   │   │
│  │ (HTML/JS)  │   │ JSブリッジ    │   │              │   │
│  └────────────┘   └──────────────┘   └──────┬───────┘   │
└─────────────────────────────────────────────┼───────────┘
                                              │
                              TCP JSON-RPC (localhost:19840)
                                              │
┌─────────────────────────────────────────────┼───────────┐
│  MirageGUI (C++ / ImGui)                    │           │
│  ┌──────────────┐   ┌──────────────────────┼────────┐  │
│  │ MacroAPI     │←──│ デバイス管理統合       │        │  │
│  │ TCPサーバー   │   │                      ▼        │  │
│  └──────────────┘   │  ┌─────────┐   ┌──────────┐   │  │
│                     │  │ AOA     │   │ ADB      │   │  │
│                     │  │ (USB)   │   │ (WiFi等) │   │  │
│                     │  └────┬────┘   └────┬─────┘   │  │
│                     └───────┼─────────────┼─────────┘  │
└─────────────────────────────┼─────────────┼────────────┘
                              │             │
                         ┌────┴────┐   ┌────┴────┐
                         │Android  │   │Android  │
                         │端末(AOA)│   │端末(ADB)│
                         └─────────┘   └─────────┘
```

## 2. デバイス操作の二重経路

### 2.1 統一操作インターフェース

マクロエディタのブロックは `tap`, `swipe`, `key` 等の抽象操作。
MirageGUIがデバイスごとに適切な経路を自動選択：

- **AOA端末**: HybridCommandSender経由（MIRAプロトコル、低遅延）
- **ADB端末**: adb shell input / adb shell am 等（フォールバック）

### 2.2 マクロエディタ側の操作

マクロエディタは接続方式を意識しない。
「tap(device_id, x, y)」を送るだけ。

```python
# マクロエディタからの呼び出し例
client.send({
    "method": "tap",
    "params": {
        "device_id": "device_hw_001",  # hardware_id
        "x": 540,
        "y": 300
    }
})
```

### 2.3 MirageGUI側の振り分けロジック

```cpp
void MacroApiServer::handle_tap(const std::string& device_id, int x, int y) {
    auto& dev = device_manager_.getUniqueDevice(device_id);
    
    if (dev.has_aoa_connection()) {
        // AOA経由: 低遅延、MIRAプロトコル
        hybrid_sender_.send_tap(dev.aoa_usb_id, x, y);
    } else {
        // ADB経由: フォールバック
        device_manager_.sendTap(dev.preferred_adb_id, x, y);
    }
}
```

## 3. TCP JSON-RPCプロトコル

### 3.1 接続

- ポート: 19840 (localhost固定)
- プロトコル: JSON行 (1リクエスト = 1行JSON + \n)
- 同期: リクエスト→レスポンス

### 3.2 コマンド一覧

#### デバイス管理
| メソッド | パラメータ | 説明 |
|---------|-----------|------|
| list_devices | - | 全デバイス一覧（接続方式含む） |
| device_info | device_id | 端末詳細情報 |

#### 操作コマンド（AOA/ADB自動切替）
| メソッド | パラメータ | 説明 |
|---------|-----------|------|
| tap | device_id, x, y | タップ |
| swipe | device_id, x1, y1, x2, y2, duration_ms | スワイプ |
| long_press | device_id, x, y, duration_ms | ロングプレス |
| key | device_id, keycode | キーイベント |
| text | device_id, text | テキスト入力 |
| click_id | device_id, resource_id | UIリソースIDクリック (AOAのみ) |
| click_text | device_id, text | UIテキストクリック (AOAのみ) |
| launch_app | device_id, package | アプリ起動 |
| force_stop | device_id, package | アプリ停止 |
| screenshot | device_id | スクリーンショット取得 |

#### マクロ管理
| メソッド | パラメータ | 説明 |
|---------|-----------|------|
| ping | - | 接続確認 |

### 3.3 レスポンス形式

```json
// 成功
{"id": 1, "result": {"status": "ok", "data": ...}}

// エラー
{"id": 1, "error": {"code": -1, "message": "Device not found"}}
```

### 3.4 デバイス一覧レスポンス例

```json
{
  "id": 1,
  "result": {
    "devices": [
      {
        "device_id": "abc123",
        "display_name": "RebotAi A9",
        "model": "A9",
        "connection": "aoa",
        "usb_id": "A9250700479",
        "ip_address": "192.168.0.5"
      },
      {
        "device_id": "def456",
        "display_name": "Npad X1",
        "model": "X1",
        "connection": "adb",
        "adb_id": "192.168.0.10:5555",
        "ip_address": "192.168.0.10"
      }
    ]
  }
}
```

## 4. Blocklyブロック設計（AOA/ADB統一）

### 4.1 基本原則

- ブロックは接続方式に依存しない
- `device_id`でデバイスを指定
- MirageGUI側で自動的にAOA/ADBを切り替え

### 4.2 AOA専用ブロック

AOAでしか使えない操作は専用ブロックにして、
ADB端末選択時はグレーアウト or 警告表示：

- `click_id` (リソースIDタップ) → AOAのみ
- `click_text` (テキストタップ) → AOAのみ

### 4.3 ADBフォールバック時の制限

| 操作 | AOA | ADB | 差異 |
|------|-----|-----|------|
| tap | ✅ 高速 | ✅ やや遅い | AOAのが低遅延 |
| swipe | ✅ | ✅ | 同上 |
| key | ✅ | ✅ | - |
| text | △ (制限あり) | ✅ | ADBの方が安定 |
| click_id | ✅ | ❌ | AOA専用 |
| click_text | ✅ | ❌ | AOA専用 |
| screenshot | △ ADB経由 | ✅ | どちらもADB |
| launch_app | ADB経由 | ✅ | どちらもADB |
| force_stop | ADB経由 | ✅ | どちらもADB |

## 5. コード生成の二重モード

### 5.1 統合モード（MirageGUI連携実行）

MirageGUIのTCPサーバー経由で実行。AOA/ADB自動切替。

```python
from mirage_client import MirageClient

client = MirageClient()
client.tap("device_hw_001", 540, 300)
client.swipe("device_hw_001", 540, 1500, 540, 500, 300)
```

### 5.2 スタンドアロンモード（ADBのみ）

MirageGUIなしで単独実行可能。ADB操作のみ。

```python
from adb_helpers import AdbHelper

adb = AdbHelper(serial="192.168.0.5:5555")
adb.tap(540, 300)
adb.swipe(540, 1500, 540, 500, 300)
```

### 5.3 生成時の切替

エクスポート時にユーザーが選択：
- 「MirageSystem統合スクリプト」→ mirage_client使用
- 「スタンドアロンスクリプト」→ adb_helpers使用（ADB操作のみ）

## 6. 実装フェーズ（修正版）

### Phase 1: プロジェクト構造 + pywebview起動 (4-6h)
- ディレクトリ構造作成
- pywebviewでBlocklyワークスペース表示

### Phase 2: C++ TCPサーバー追加 (6-8h)  ← 新規
- MirageGUIにMacroApiServer追加
- JSON-RPCハンドラ実装
- AOA/ADB振り分けロジック

### Phase 3: コアブロック12個 + コード生成 (8-10h)
- タップ/スワイプ/キー/テキスト/アプリ操作ブロック
- Python コードジェネレータ
- mirage_client.py (TCPクライアント)

### Phase 4: 保存/読込/実行ブリッジ (6-8h)
- マクロ保存・読込 (JSON workspace)
- エディタからの実行・停止
- 実行ログ表示

### Phase 5: ライブテスト + デバイス管理 (6-8h)
- 右クリック→即時実行
- デバイス一覧の動的更新
- AOA/ADB状態表示

### Phase 6: 画面分析ブロック (8-12h)
- スクリーンショット取得・表示
- uiautomator dump テキスト検出
- 座標ピッカー（スクリーンショット上でクリック→座標取得）

### Phase 7: テーマ・UX仕上げ (4-6h)
### Phase 8: パッケージング (3-5h)

合計: 約45-63時間

## 7. ファイル構成

```
macro_editor/
├── app.py                      # pywebview エントリポイント
├── mirage_client.py            # MirageGUI TCP接続クライアント
├── adb_helpers.py              # スタンドアロン用ADBヘルパー
├── backend/
│   └── api.py                  # MacroEditorAPI (pywebview js_api)
├── frontend/
│   ├── index.html              # メインページ
│   ├── css/
│   │   └── editor.css          # カスタムスタイル
│   ├── js/
│   │   ├── blocks/             # カスタムブロック定義
│   │   │   ├── adb_touch.js
│   │   │   ├── adb_input.js
│   │   │   ├── adb_app.js
│   │   │   ├── adb_screen.js
│   │   │   └── adb_flow.js
│   │   ├── generators/         # コードジェネレータ
│   │   │   └── python_adb.js
│   │   ├── toolbox.js          # ツールボックス定義
│   │   └── workspace.js        # 初期化・ブリッジ
│   └── lib/
│       └── blockly/            # Blockly本体
└── macros/                     # 保存マクロ
    └── example/
        ├── workspace.json
        └── example.py

# MirageGUI側追加ファイル
src/
├── macro_api_server.hpp        # TCP JSON-RPCサーバー
└── macro_api_server.cpp
```
