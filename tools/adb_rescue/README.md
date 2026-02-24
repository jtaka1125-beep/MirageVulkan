# adb_rescue

ADB救急箱（運用時の診断/復旧用）。通常運用はAOA。

## 端末ID
- X1: `192.168.0.3:5555`
- A9 #956: `192.168.0.6:5555`
- A9 #479: `192.168.0.8:5555`

## 1) 30秒診断
```powershell
./diag_mirage.ps1 -Device 192.168.0.3:5555
```

出力:
- Mirage系パッケージの存在/バージョン
- プロセス（mirage系）
- MediaProjection状態（投影中か）
- mirage関連logcat末尾

## 2) UI起動（許可フロー/復旧）
```powershell
# Accessory UI
./start_accessory_ui.ps1 -Device 192.168.0.3:5555

# Capture UI（MediaProjectionの許可を出す）
./start_capture_ui.ps1 -Device 192.168.0.3:5555
```

## 3) 動画パラメータ調整（サービス稼働中に有効）
```powershell
# USBルートへ
./video_route_usb.ps1 -Device 192.168.0.3:5555

# Wi-Fi(UDP)ルートへ
./video_route_wifi.ps1 -Device 192.168.0.3:5555 -Host 192.168.0.2 -Port 50000

# FPS
./video_set_fps.ps1 -Device 192.168.0.3:5555 -Fps 30

# IDR要求（キーフレーム）
./video_request_idr.ps1 -Device 192.168.0.3:5555
```

## メモ
- `ACTION_VIDEO_*` は `com.mirage.capture.ipc.AccessoryCommandReceiver` が処理。
- `ScreenCaptureService` が動いていない場合、route/fps/idr は無視されることがあります（logcatで確認）。
