# tools/archive/

MirageCompleteから移行したデバッグ・テスト用ツール群のアーカイブ。

**注意**: これらは開発・デバッグ用であり、本番運用には不要。

## 内容

### AOA (Android Open Accessory) テストツール
- `aoa_test*.cpp` — AOA接続テスト
- `aoa_io_test*.cpp` — AOA I/Oスループットテスト
- `aoa_all_test*.cpp` — AOA総合テスト
- `aoa_cmd_test.cpp` — AOAコマンドテスト
- `aoa_full_test.cpp` — AOAフルフローテスト
- `aoa_stress_test.cpp` — AOAストレステスト
- `aoa_reset.cpp` — AOAデバイスリセット
- `aoa_switch_dev.py` — AOAデバイス切替
- `aoa_switch_libusb0.py` — libusb0ドライバ切替

### Bluetooth PAN テストツール
- `bt_auto_pair*.py/.ps1` — BT自動ペアリング
- `bt_bnep_connect.cpp` — BNEP接続
- `bt_pan_connect.cpp` — PAN接続
- `bt_pan_dial.cpp` — PANダイアル

### その他
- `port_patch.cpp` — ポートパッチ
- `send_usb_route.cpp` — USBルート送信
- `check_libusb.py` — libusb確認
- `bring_to_front.ps1` — ウィンドウ前面化
