# AOA Flow Review Report

Date: 2025-02-08

## Summary

AOA切替フロー（gui_device_control.cpp）の全懸念点レビュー＆修正結果。

## Fixes Applied

### 1. Detached Thread → Joinable (FIXED)
- **問題**: `std::thread(aoaSwitchThread).detach()` でスレッドがdetachされていた
- **修正**: `static std::thread g_aoa_switch_thread` で管理、switchAllDevicesToAOA()でjoin後に新スレッド代入、closeAllAOADevices()でjoin

### 2. system() → CreateProcess (FIXED)
- **問題**: `system("adb kill-server")` がコンソールウィンドウをフラッシュ、_popen()もタイムアウト制御なし
- **修正**: execAdbCommand()をCreateProcess(CREATE_NO_WINDOW)ベースに完全書き換え
  - パイプ経由で出力取得
  - タイムアウト制御（デフォルト10秒、PeekNamedPipe+WaitForSingleObject）
  - TerminateProcess()でハング防止
  - system("adb kill-server")もexecAdbCommand("kill-server")に変更

### 3. IP Validation (FIXED)
- **問題**: collectDeviceWifiInfo()で取得したIPアドレスの検証なし（コマンドインジェクション可能性）
- **修正**: `isValidWifiIp()` 追加（digits+dotsのみ、3ドット必須、15文字以下）
  - IP取得後に検証、不正IPは拒否
  - startScreenCapture()でpc_ipも検証

### 4. substr Crash Prevention (FIXED)
- **問題**: `tcpip_output.substr(0, 60)` が空文字列でも安全だが、改行文字がログに混入
- **修正**: サイズチェック追加 + 改行文字除去

## Acceptable / No Fix Needed

### 5. config_loader.hpp Include
- **状態**: OK ✅ — 既にinclude済み
- `getConfig().network.pc_ip` / `video_base_port` 正常動作確認

### 6. Android auto_mirror Intent
- **状態**: OK ✅ — MainActivity.kt で `auto_mirror`, `mirror_host`, `mirror_port` を正しく処理
- `createScreenCaptureIntent()` → MediaProjectionダイアログ表示

### 7. MediaProjection Dialog Coordinates
- **状態**: Acceptable (要実機調整)
- 比率 (0.87, 0.67) はA9 (800x1340)で検証済み
- Npad X1 (1200x2000)では比率スケーリングで対応
- Android 14+でUI変更の可能性あり → 実機テストで調整
- 改善案: UIダンプでボタン座標を動的取得（重いので現時点ではフォールバック座標で十分）

### 8. Wi-Fi ADB Timing
- **状態**: OK
- tcpip 5555はデバイス側の設定で永続（USB切断しても有効）
- AOA切替後2秒待機 + adb start-server + connect

### 9. screenCaptureThread Competition
- **状態**: Acceptable (共存可能)
- screencapThreadはADB screencapでPNGスクショ取得
- H.264ミラーリングが動いてる場合でもscreencapは独立して動作
- 将来的にH.264受信に切り替えた場合はscreencapを停止すべき

### 10. MirrorReceiver Startup
- **状態**: 要確認 (実機テスト時)
- MultiDeviceReceiverがGUI起動時にUDPリッスンを開始してるか未確認
- Android側がRTPストリームを送信開始してもPC側が受信してないと映像表示されない
- 次のテストセッションで確認

## Remaining Tasks

1. **MirrorReceiver起動タイミング確認** — AOA切替後にMirrorReceiverが自動起動するか実機テスト
2. **MediaProjection座標の実機検証** — 全3台で正しくボタンがタップされるか確認
3. **Windows Firewall UDP許可** — UDP 60000+のインバウンドルール確認
4. **PC IP自動検出** — config.json手動設定の代わりに自動検出を検討
5. **executeADB()のsystem()呼び出し** — ADB Connection Controlセクションにまだsystem()が残っている（AOAフロー外なので優先度低）

## Files Modified

- `src/gui/gui_device_control.cpp` — 全修正適用

## Build Status

- ✅ Build SUCCESS (mirage_gui.exe 3.1 MB)
