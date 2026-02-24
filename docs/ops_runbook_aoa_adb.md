# Mirage 運用Runbook（AOA通常運用 + ADB救急箱）

更新: 2026-02-25

## 目的
- **通常はAOAで運用**（低遅延・USB挿すだけ）
- **導入/復旧/診断だけADB**（確実な逃げ道）
- 本番は **MirageVulkan単体で動作**。MCPは保守ツールとして任意。

---

## 対象デバイスID
- X1: `192.168.0.3:5555`
- A9 #956: `192.168.0.6:5555`
- A9 #479: `192.168.0.8:5555`

---

## 通常運用（AOA）

### MirageVulkan側UI（必須ステータス）
- AOA: Connected / Disconnected
- Video: Running / Stopped
- Permission: OK / **MediaProjection未許可**
- Battery: OK / **最適化で落ちる可能性**
- Last error: 直近エラー（短文）

### 通常起動フロー
1. USB Hubに3台接続
2. MirageVulkan起動
3. 端末自動検出→AOAハンドシェイク
4. 必要に応じて capture 起動要求（映像開始）

---

## ADB救急箱（診断→復旧）

### ADB救急箱の場所
- `MirageVulkan/tools/adb_rescue/`

### 30秒診断（端末ごと）
```powershell
./diag_mirage.ps1 -Device 192.168.0.3:5555
```

#### 判定
- `Media Projection: null` → 投影未開始/許可待ち
- `com.mirage.capture` がいない → capture起動失敗/停止
- `com.mirage.accessory` もいない → 常駐崩壊/省電力/クラッシュ

---

## 復旧アクション（軽→重）

### 1) UI起動（まずこれ）
```powershell
# Accessory UI（AOA側の再初期化導線）
./start_accessory_ui.ps1 -Device 192.168.0.3:5555

# Capture UI（MediaProjection許可を出す）
./start_capture_ui.ps1 -Device 192.168.0.3:5555
```

### 2) 動画ルート/FPS/IDR（ScreenCaptureService稼働中に有効）
```powershell
./video_route_usb.ps1 -Device 192.168.0.3:5555
./video_route_wifi.ps1 -Device 192.168.0.3:5555 -Host 192.168.0.2 -Port 50000
./video_set_fps.ps1 -Device 192.168.0.3:5555 -Fps 30
./video_request_idr.ps1 -Device 192.168.0.3:5555
```

### 3) 省電力/権限（最後はUI誘導が確実）
- バッテリー最適化除外（設定画面誘導）
- RECORD_AUDIO/通知などの再許可

### 4) APK再導入（最終手段）
- `adb install -r <apk>`（MCP/スクリプトで一括）

---

## MCPに載せる（保守オプション）
- `diag_mirage(device)`：packages/versions, ps, media_projection, logcat短縮
- `open_capture_ui(device)` / `open_accessory_ui(device)`
- `video_route_usb(device)` / `video_route_wifi(device, host, port)`
- `set_fps(device, fps)` / `request_idr(device)`
- `reinstall_apks(device)`（最終）

---

## 既知の事実（2026-02-25 実機確認）
- 3台すべてに以下がインストール済み
  - `com.mirage.accessory 1.0.0 (vc=1)`
  - `com.mirage.capture 1.0.0 (vc=1)`
  - `com.mirage.android 2.1.0-audio (vc=2)`
- `dumpsys media_projection` は3台とも `null`（投影中なし）
- プロセスは端末差あり（X1はcaptureも動作、A9はaccessoryのみの状態を確認）
