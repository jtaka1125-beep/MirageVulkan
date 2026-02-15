# USB H.264ミラーリング 実機テスト手順

## 前提
- Android Studioでビルド済みAPK
- USB接続の3台: RebotAi A9 x2, N-one Npad X1

## Step 1: APKインストール
```
adb -s <SERIAL1> install -r app-debug.apk
adb -s <SERIAL2> install -r app-debug.apk
adb -s <SERIAL3> install -r app-debug.apk
```

## Step 2: PC側GUI起動
```
cd C:\MirageWork\MirageComplete\build
.\mirage_gui.exe
```
確認: ログに3台のADBデバイス検出、UDPリスナー60000-60009起動

## Step 3: AOA切替
GUIの「全デバイス AOA切替」ボタンを押す。
確認: デバイスリストが ADB(USB) → AOA に変わる

## Step 4: USB映像開始（1台目）
AOA接続後、AccessoryIoServiceが自動起動する。
ScreenCaptureServiceをUSBモードで起動:

```
adb -s <SERIAL1> shell am start --activity-clear-top \
  -n com.mirage.android/.ui.MainActivity \
  --ez auto_mirror true \
  --es mirror_mode usb
```

※ AOA切替後はadbが切断される場合がある。
その場合はAndroid側アプリのUIから「Start Mirror」を手動タップ。

## Step 5: MediaProjectionダイアログ承認
AOA状態ではUIAutomatorが使えない可能性。
手動で:
1. 「画面全体」を選択
2. 「開始」をタップ

## Step 6: PC側確認
GUI確認ポイント:
- メイン映像エリアにH.264映像が表示される
- fps > 0
- ログに VID0 パケット受信のメッセージ

stderrで確認:
```
[UsbCmd] Received video data: XXXX bytes
```
または
```
VID0 packet
```

## Step 7: 操作テスト
映像表示中にGUIからTAP操作を送信。
- 映像送信と操作コマンドが同時に動作すること
- TAP応答が遅延しないこと（操作優先度確認）

## Step 8: 複数台テスト
残り2台も同様にAOA切替→映像開始。
- メイン+サブで3台の映像が表示される
- 帯域表示が0より大きい

## トラブルシューティング

### AOA切替後にadbが使えない
AOAモードではADBが無効になる場合がある。
→ GUIの「操作」→「全て更新」でADB再接続試行
→ ダメな場合はAndroidアプリのUIから手動操作

### VID0パケットが来ない
1. AccessoryIoServiceが起動しているか: logcat | grep AccessoryIO
2. ScreenCaptureServiceがUSBモードか: logcat | grep ScreenCapture
3. UsbVideoSenderが作成されたか: logcat | grep UsbVideoSender
4. outputStreamがnullでないか: logcat | grep "Video output stream"

### 映像が表示されるがカクカク
USB帯域圧迫。次フェーズのFPS可変制御で対応予定。

## 成功条件
- USB接続のみでH.264映像がGUIに表示される
- 操作コマンドが映像送信中も遅延なく通る
- 複数台の映像が同時表示される
