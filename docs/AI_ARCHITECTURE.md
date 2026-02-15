# MirageSystem AI・自動操作 アーキテクチャ解説
**作成日**: 2026-02-07

---

## 全体像: 「見て」→「判断して」→「操作する」

```
┌─────────────────────────────────────────────────────────────┐
│                    MirageSystem AI パイプライン               │
│                                                              │
│  ① 見る        ② 認識する       ③ 判断する      ④ 操作する  │
│                                                              │
│  Android画面 → テンプレート照合 → アクション決定 → タップ送信 │
│  (RGBA)        OCRテキスト検出    学習データ参照   スワイプ    │
│                UI要素検索         ルール評価       キー入力    │
└─────────────────────────────────────────────────────────────┘
```

---

## ① 見る: 画面取得パイプライン

Android端末の画面をPC側で取得する3つの経路がある。
**USBデバッグ(ADB)なしで動作させることが前提**。AOAが本番経路。

```
Android端末
    │
    ├── 【経路A: USB AOA映像】 ← 本番メイン経路 🎯
    │   MediaProjection → H.264 → VID0プロトコル → USB Bulk
    │   → UsbVideoReceiver → MirrorReceiver → RGBA
    │   速度: 30fps（最低遅延）
    │   特徴: USBデバッグ不要、AOAモードのみ必要
    │   ファイル: usb_video_receiver.cpp, hybrid_receiver.cpp
    │
    ├── 【経路B: Wi-Fi RTP映像】 ← 本番サブ経路（無線時）
    │   MediaProjection → H.264 → RTP → UDP:60000
    │   → MirrorReceiver → H264Decoder → RGBA
    │   速度: 30fps（リアルタイム）
    │   特徴: USBデバッグ不要、Wi-Fi接続のみ
    │   ファイル: mirror_receiver.cpp, h264_decoder.cpp
    │
    └── 【経路C: ADBスクリーンショット】 ← フォールバック/デバッグ用
        screencap → PNG → ADB pull → stb_image → RGBA
        速度: 2秒間隔（低速だが確実）
        用途: AOA不具合時、初期セットアップ、デバッグ
        ファイル: adb_device_manager.cpp (takeScreenshot)
                 gui_threads.cpp (screenCaptureThread)

全経路の合流点:
    RGBA フレーム → queueFrame() → D3D11テクスチャ → ImGui描画
    ファイル: gui_application.cpp (queueFrame, processPendingFrames)
             gui_render.cpp (renderDeviceView)
```

---

## ② 認識する: 3つのエンジン

画面のRGBAフレームから「何が映ってるか」を認識する3つの方法。

### A. テンプレートマッチング (AIEngine)
```
保存済み画像テンプレート（ボタン、アイコン等のPNG）
    ↓
GPU上でフレームと照合 (SAD: Sum of Absolute Differences)
    ↓
一致位置 (x, y) + 信頼度スコア を返す

ファイル群:
  ai_engine.cpp/hpp          ← メインエンジン (OpenCL初期化、マッチング実行)
  ai/gpu_template_matcher_mvp.cpp  ← D3D11コンピュートシェーダ版
  gpu/gpu_template_matcher_d3d11.cpp ← D3D11 SADマッチャー
  ai/template_store.cpp      ← テンプレート画像のGPU管理
  ai/template_manifest.cpp   ← テンプレートID・メタデータ管理
  ai/template_autoscan.cpp   ← テンプレートフォルダ監視（新規/更新検出）
  ai/template_hot_reload.cpp ← 実行中にテンプレート追加・更新
  ai/template_capture.cpp    ← 実行中の画面からテンプレート切り出し
  ai/template_writer.cpp     ← テンプレートPNG保存

使い方の流れ:
  1. templates/ フォルダにPNG画像を置く
  2. AIEngine起動時にautoscanで自動読み込み
  3. 毎フレーム、全テンプレートをGPU照合
  4. 閾値超えの一致があれば位置を返す
```

### B. OCR テキスト認識 (OCREngine)
```
画面フレーム (RGBA)
    ↓
前処理 (グレースケール変換、ヒストグラム平坦化)
    ↓
Tesseract LSTM エンジン
    ↓
認識テキスト + バウンディングボックス

ファイル:
  ocr_engine.cpp/hpp    ← Tesseract初期化、ROI認識、テキスト検索
  
機能:
  - 日本語 + 英語 対応
  - 画面の特定領域 (ROI) だけを認識可能
  - 「設定」「OK」等のテキスト位置を返せる
```

### C. UI要素検索 (UiFinder)
```
ADB経由で画面のUI構造を取得
    ↓
3つの検索戦略:
  ① リソースID検索: "com.example:id/button_ok"
  ② テキスト検索: "設定"
  ③ 座標テーブル: 事前定義の固定座標
  ④ OCR: 上記3つで見つからない場合のフォールバック

ファイル:
  ui_finder.cpp/hpp     ← マルチ戦略UI検索
  
仕組み:
  adb shell uiautomator dump → XML解析 → 要素検出
```

---

## ③ 判断する: 何をするか決める

### 学習モード (LearningMode)
```
ユーザーの操作を記録して学習データを作る。

操作の流れ:
  1. GUI上で Ctrl+L で学習モードON
  2. ユーザーがマウスで画面をクリック
  3. クリック位置の周辺画像を切り出してテンプレート保存
  4. クリック座標を相対位置として記録
  5. 「このテンプレートが見えたら→この座標をタップ」のルールが生成される

ファイル:
  ai/learning_mode.cpp  ← フレームROI切出し→Gray8テンプレート保存→登録
  gui_application.cpp   ← 学習セッション管理 (LearningSession構造体)
  gui_input.cpp         ← マウスクリック記録
  
データ保存先:
  templates/ フォルダにPNG + manifest.json
```

### VisionDecisionEngine (ai_engine.cpp内)
```
テンプレートマッチング結果を受けてアクションを決定。

入力: マッチ結果のリスト [{template_id, x, y, score}, ...]
出力: 実行すべきアクション [{tap, x, y}, {swipe, x1,y1,x2,y2}, ...]

判断ロジック:
  - 最高スコアのマッチを優先
  - テンプレートに紐づくアクション定義を参照
  - 同じテンプレートの連続マッチ防止（デバウンス）
```

---

## ④ 操作する: コマンド送信

### 送信経路
```
GUI操作 or AI決定
    ↓
gui_command.cpp (sendTapCommand / sendSwipeCommand)
    ↓
    ├── 【USB AOA】HybridCommandSender → MultiUsbCommandSender ← 本番メイン 🎯
    │   MIRAプロトコル: [MAGIC:4][VER:1][CMD:1][SEQ:4][LEN:4][PAYLOAD]
    │   libusb_bulk_transfer → Android AOA受信
    │   USBデバッグ不要、AOAモードで直接送信
    │
    ├── 【Wi-Fi UDP】WifiCommandSender ← 本番サブ（無線時）
    │   同じMIRAプロトコルをUDP送信
    │   → Android UDP受信 → コマンド実行
    │
    └── 【IPC → ADB】MirageIpcClient → miraged.exe → adb shell input
        名前付きパイプ経由でJSON → ADB inputコマンド
        ※AOA操作が効かない時のフォールバック / デバッグ用

コマンド種別:
  CMD_TAP (0x01)   : x, y, screen_w, screen_h
  CMD_SWIPE (0x07) : x1, y1, x2, y2, duration_ms
  CMD_BACK (0x02)  : 戻るキー
  CMD_KEY (0x03)   : 任意キーコード
  CMD_CLICK_ID (0x05) : リソースIDクリック
  CMD_CLICK_TEXT (0x06) : テキストクリック
```

### マウス入力→コマンド変換
```
ユーザーのマウス操作:
  gui_input.cpp
    ↓
  WM_LBUTTONDOWN → ドラッグ開始座標記録
  WM_LBUTTONUP   → ドラッグ距離判定
    ├── 5px以下 → タップ → processMainViewClick()
    └── 5px超   → スワイプ → processSwipe()
    ↓
  screenToDeviceCoords() ← GUI座標 → デバイス座標変換
    (ImGui描画領域のオフセット・スケール計算)
    ↓
  tap_callback_ / swipe_callback_ → gui_command.cpp
```

---

## 全体フロー図（自動操作時）

```
    Android端末                    PC (MirageSystem)
    ──────────                    ─────────────────
                                  screenCaptureThread
                                  │ ADB screencap (2秒間隔)
    画面PNG ◄──── ADB pull ─────┤
                                  │ stb_image デコード
                                  ▼
                                  RGBA フレーム
                                  │
                    ┌─────────────┼─────────────────┐
                    │             │                  │
                    ▼             ▼                  ▼
              AIEngine      OCREngine          UiFinder
              テンプレート   テキスト認識       UI構造検索
              GPU照合        Tesseract         uiautomator
                    │             │                  │
                    └─────────────┼──────────────────┘
                                  │
                                  ▼
                          VisionDecisionEngine
                          「OKボタンが見えた→タップ」
                                  │
                                  ▼
                          gui_command.cpp
                          sendTapCommand(device_id, x, y)
                                  │
                    ┌─────────────┼──────────────┐
                    │             │               │
                    ▼             ▼               ▼
               USB AOA       Wi-Fi UDP      IPC/ADB
               (無効)        MIRAプロト     adb input
                                  │
    タップ実行 ◄─────────────────┘
```

---

## マクロエディタとの関係

```
マクロエディタ (別アプリケーション、Phase 1実装済み)
  │
  │ pywebview + Blockly UI
  │ ブロックを組み合わせてマクロ定義
  │
  │  ┌──────────────────────────────┐
  │  │ [タップ] x=400 y=670         │
  │  │ [待機] 1秒                   │
  │  │ [スワイプ] (100,500)→(100,100)│
  │  │ [テンプレート待ち] ok_btn.png │
  │  │ [タップ] テンプレート位置     │
  │  └──────────────────────────────┘
  │
  ▼
  Pythonスクリプト生成 (コードジェネレータ)
  │
  │ 生成されたスクリプトが使うAPI:
  │   AdbClient.tap(device, x, y)
  │   AdbClient.swipe(device, x1,y1, x2,y2)
  │   AIEngine.find_template("ok_btn.png")
  │
  ▼
  MirageSystem本体のADB/コマンド経路で実行
  
座標系: デバイスネイティブ解像度
  A9: 800x1340
  Npad X1: 1200x2000
  → screencapと同じ座標系なのでズレなし ✅
```

---

## ファイル→機能 早見表

| やりたいこと | 見るファイル |
|-------------|-------------|
| 画面取得方法を変えたい | gui_threads.cpp (screenCaptureThread) |
| テンプレートマッチングの閾値変更 | ai_engine.cpp |
| 新しいテンプレート追加 | templates/ にPNG置くだけ (autoscan) |
| OCRの言語追加 | ocr_engine.cpp (tessdata パス) |
| タップ座標の変換ロジック | gui_input.cpp (screenToDeviceCoords) |
| コマンド送信経路の切替 | gui_command.cpp |
| 学習モードの挙動変更 | ai/learning_mode.cpp |
| マクロエディタのブロック追加 | macro_editor/ (Blockly定義) |
| デバイス検出ロジック | adb_device_manager.cpp |
| 接続監視 | connection_daemon.py |
