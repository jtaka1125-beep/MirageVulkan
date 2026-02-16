# MirageVulkan

Android端末の画面ミラーリング・一括操作プラットフォーム。  
USB AOA + WiFi ハイブリッド通信、Vulkan GPU H.264デコード、AI画像認識による自動操作を統合。

## アーキテクチャ

```
[Android端末 x N] ──USB AOA / WiFi UDP──→ [Windows PC: MirageVulkan]
  com.mirage.android   → H.264映像ストリーム       → Vulkan GPUデコード → GUI表示
  com.mirage.accessory ← AOA HIDタッチコマンド     ← ユーザー操作 / AI自動操作
  com.mirage.capture   → MediaProjection権限管理
```

## ディレクトリ構成

```
MirageVulkan/
├── src/                    C++コア (90ファイル)
│   ├── vulkan_video_decoder.*   Vulkan Computeベース H.264デコーダ
│   ├── h264_parser.*            NAL Unit パーサー
│   ├── device_registry.*       端末中央レジストリ (DeviceEntity)
│   ├── adb_device_manager.*    ADB デバイス管理・重複排除
│   ├── multi_usb_command_sender.* AOA マルチデバイスコマンド送信
│   ├── gui_application.*       Direct2D/ImGui GUIアプリケーション
│   └── config_loader.*         config.json 外部設定ローダ
├── shaders/                Vulkan Compute シェーダ (7本)
│   ├── yuv_to_rgba.comp        YUV→RGBA変換
│   ├── template_match_ncc.comp NCC テンプレートマッチング
│   └── ...
├── tests/                  Google Test テストスイート (10本)
├── android/                Android APKソース (3モジュール)
│   ├── app/                    MirageAndroid (映像送信)
│   ├── accessory/              MirageAccessory (AOAコマンド受信)
│   ├── capture/                MirageCapture (権限管理)
│   └── mirage.keystore         APK署名鍵 (.gitignore除外)
├── scripts/                運用スクリプト
│   ├── deploy_apk.py           APKビルド→全端末デプロイ→起動
│   ├── device_health.py        端末ヘルスチェック一覧
│   ├── device_backup.py        端末データ一括バックアップ
│   ├── mirage_cli.py           MirageCLI (操作コマンド)
│   ├── auto_setup_devices.py   端末自動セットアップ
│   ├── bt_auto_pair.py         Bluetooth自動ペアリング
│   └── ...                     (計31本)
├── docs/                   技術ドキュメント (15本)
├── driver_installer/       WinUSBドライバインストーラ
├── macro_editor/           マクロエディタ (Python)
├── tools/archive/          デバッグツールアーカイブ
├── config.json             外部設定ファイル
├── CMakeLists.txt          C++ ビルド設定
└── MIGRATION_PLAN.md       MirageComplete→MirageVulkan移行計画
```

## ビルド

### 前提条件

- Windows 10/11
- Visual Studio 2022 (C++17)
- CMake 4.x
- Vulkan SDK 1.4+
- FFmpeg (PATH上)
- Python 3.13+
- Android SDK / Gradle (APKビルド時)

### C++ ビルド

```bat
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
ctest --output-on-failure
```

### APKビルド+デプロイ

```bat
python scripts/deploy_apk.py               # 全モジュール ビルド+デプロイ
python scripts/deploy_apk.py --module app   # appのみ
python scripts/deploy_apk.py --skip-build   # 既存APKをデプロイ
```

## 運用コマンド

```bat
# 端末ヘルスチェック
python scripts/device_health.py
python scripts/device_health.py --watch     # 継続監視

# 端末バックアップ
python scripts/device_backup.py             # 全端末一括
python scripts/device_backup.py --no-ab     # adb backupスキップ

# ログ収集
python scripts/collect_logs.py              # Mirageタグのlogcat収集

# フルビルド (C++ + APK + テスト)
python scripts/full_build.py
```

## テスト

10テストスイート、全PASS:

| テスト | 内容 |
|---|---|
| VulkanVideoTest | Vulkan GPUデコーダ |
| H264ParserTest | H.264 NALパーサー |
| E2EDecodeTest | エンドツーエンドデコード |
| EventBusTest | イベントバス |
| Vid0ParserTest | VID0プロトコルパーサー |
| AdbSecurityTest | ADBセキュリティ検証 |
| AoaHidTest | AOA HIDコマンド |
| MiraProtocolTest | MIRAプロトコル |
| FrameDispatcherTest | フレーム配信 |
| RttBandwidthTest | RTT/帯域幅計測 |

## 対応端末

| 端末 | 解像度 | Android | 備考 |
|---|---|---|---|
| Npad X1 | 1200x2000 | 13 (SDK 33) | 10.4インチ |
| A9 | 800x1340 | 15 (SDK 35) | 8.7インチ |

## バックアップ

- Git: https://github.com/jtaka1125-beep/MirageVulkan (private)
- 端末データ: `C:\MirageWork\device_backup\` (毎日3:00自動実行)
- keystore: `C:\MirageWork\device_backup\keystore\`
- ADB鍵: `C:\MirageWork\device_backup\pc_keys\`
