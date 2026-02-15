# H.264ミラーリング接続テスト指示書

## 目的
MultiDeviceReceiver (Port-First設計) + ScreenCaptureService で
H.264映像をGUIに表示する。

## 前提条件
- mirage_gui.exe ビルド済み (3.1 MB, 2026-02-08)
- UDP 60000-60009 の10ポートリスナーがGUI起動時に自動作成される
- Android側 ScreenCaptureService は H.264→UDP送信の実装済み
- PC IP: 192.168.0.7, config.json設定済み

## 現状の問題
1. GUI起動後、ADBスクショ(screencap)で映像表示は動いている
2. H.264ミラーリングは動いていない
3. ScreenCaptureServiceを手動でam startすれば送信自体は動作確認済み
4. MultiDeviceReceiverが受信→デコード→GUI表示のパイプラインが未検証

## 作業手順

### Step 1: GUI起動確認
```
cd C:\MirageWork\MirageComplete\build
mirage_gui.exe
```
- stderrに `[Main] Listeners ready on ports 60000-60009` が出ることを確認
- UDP 60000-60009がLISTEN状態: `netstat -an | findstr "UDP.*6000"`

### Step 2: ADBデバイス確認
```
adb devices -l
```
- USB接続の端末シリアル取得 (A9250700956, A9250700479, 93020523431940等)
- Wi-Fi ADB接続中の端末IP確認

### Step 3: 1台目のScreenCapture開始 (ポート60000)
```
adb -s A9250700956 shell am start --activity-clear-top \
  -n com.mirage.android/.ui.MainActivity \
  --ez auto_mirror true \
  --es mirror_host 192.168.0.7 \
  --ei mirror_port 60000
```

### Step 4: MediaProjectionダイアログ承認
端末にダイアログが出る。UIAutomator経由で自動操作:

1. UI構造をdump:
```
adb -s A9250700956 shell uiautomator dump /data/local/tmp/ui.xml
adb -s A9250700956 shell cat /data/local/tmp/ui.xml
```

2. 「1つのアプリ」Spinnerをタップしてドロップダウン表示:
   - Spinnerのboundsの中心をtap

3. 「画面全体」をタップ:
   - text="画面全体" のboundsの中心をtap

4. 再度UIダンプして「開始」ボタンの座標確認:
```
adb -s A9250700956 shell uiautomator dump /data/local/tmp/ui.xml
```
   - text="開始" resource-id="android:id/button1" のboundsの中心をtap

### Step 5: 送信確認
```
adb -s A9250700956 logcat -d | grep -iE "MirageCapture|MirageH264|MirageUdpSender" | tail -10
```
- `MirageUdpSender: UDP sender initialized: 192.168.0.7:60000` が出ること
- `MirageH264: Output format changed` が出ること

### Step 6: PC側受信確認
```
netstat -an | findstr "UDP.*6000"
```
- UDP 60000が受信してるか確認

### Step 7: GUI映像表示確認
- デスクトップスクリーンショットでGUIの映像エリアを確認
- ADBスクショ方式から切り替わってH.264映像が表示されているか

### Step 8: 2台目以降 (ポート60001, 60002)
2台目:
```
adb -s A9250700479 shell am start --activity-clear-top \
  -n com.mirage.android/.ui.MainActivity \
  --ez auto_mirror true \
  --es mirror_host 192.168.0.7 \
  --ei mirror_port 60001
```
同様にMediaProjectionダイアログ承認。

3台目 (Npad X1):
```
adb -s 93020523431940 shell am start --activity-clear-top \
  -n com.mirage.android/.ui.MainActivity \
  --ez auto_mirror true \
  --es mirror_host 192.168.0.7 \
  --ei mirror_port 60002
```

## デバッグポイント

### GUIに映像が出ない場合
1. gui_threads.cpp の screenCaptureThread 内でMultiDeviceReceiverからフレーム取得してるか
   - `g_multi_receiver->get_latest_frame_by_port(port, frame)` を使用
   - まだデバイス未登録の場合 get_latest_frame(hw_id) は失敗する
   - **port指定で取得する方がPort-First設計に合致**

2. MirrorReceiver の init_decoder() が呼ばれてるか
   - `#ifdef USE_FFMPEG` が有効か確認 (CMakeLists.txt: ON)
   - H264Decoder::init() が成功してるか

3. gui_threads.cpp のフレーム更新ループを確認:
   - 326行付近: `if (g_multi_receiver && g_multi_receiver->running())`
   - getDeviceIds() が空の場合（デバイス未登録）、フレーム取得されない
   - **修正案**: getDeviceIds()ではなくポート番号ベースでフレーム取得に変更

### 重要: gui_threads.cppの修正が必要な可能性
現在のscreenCaptureThreadはget_latest_frame(hardware_id)を使っている。
Port-First設計では、デバイスがregisterされる前はhardware_idが空。
get_latest_frame_by_port(port)に切り替える修正が必要かもしれない。

該当箇所: src/gui/gui_threads.cpp 326行付近
```cpp
// 現在のコード (デバイスID必須)
auto device_ids = g_multi_receiver->getDeviceIds();
for (const auto& hw_id : device_ids) {
    if (g_multi_receiver->get_latest_frame(hw_id, frame)) { ... }
}

// 修正案 (ポートベースで全ポート走査)
auto base_port = g_multi_receiver->getBasePort();
auto num_slots = g_multi_receiver->getNumSlots();
for (int i = 0; i < num_slots; i++) {
    ::gui::MirrorFrame frame;
    int port = base_port + i;
    if (g_multi_receiver->get_latest_frame_by_port(port, frame)) {
        if (frame.width > 0 && frame.height > 0 && !frame.rgba.empty()) {
            std::string device_label = "port_" + std::to_string(port);
            // ... GUIに登録・フレーム更新
        }
    }
}
```

## ファイル一覧
- src/multi_device_receiver.hpp/cpp - Port-First受信管理
- src/gui/gui_main.cpp - initializeMultiReceiver() (10スロット)
- src/gui/gui_threads.cpp - screenCaptureThread フレーム取得ループ
- src/mirror_receiver.hpp/cpp - UDP受信→H.264デコード
- src/h264_decoder.hpp/cpp - FFmpegデコーダ
- android/.../capture/ScreenCaptureService.kt - Android H.264エンコード+UDP送信
- android/.../ui/MainActivity.kt - auto_mirror intent処理
- config.json - pc_ip, video_base_port設定
