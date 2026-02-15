# MirageSystem 問題点リスト・対策計画
## 作成: 2026-02-12 / 更新: 2026-02-13 セッション8

---

## P0: 致命的バグ — 全完了

- ✅ P0-1: swipe/pinch uint16_t アンダーフロー [commit 1af44d9]
- ✅ P0-2: ADBフォールバックGUIブロッキング [commit e1a4be7]
- ✅ P0-3: HIDが単一デバイス専用 [commit e1a4be7]

## P1: 高リスク — 全完了

- ✅ P1-1: MediaTek AOA v2 検証済み — VID/PID切替成功(870ms), 双方向通信OK
- ✅ P1-2: テストカバレッジ — 215テスト全パス
- ✅ P1-3: pixel_to_hid_x/y ゼロ除算 [commit e1a4be7]

## P2: 設計改善

- ✅ P2-1: ARCHITECTURE.md作成
- ✅ P2-2: include確認済み
- ✅ P2-3: AOA v2事前チェック + GUI表示
- 📋 P2-4: エンディアン明示 (低優先)

## P3: 品質改善

- ✅ P3-1: CI/CD — scripts/ci_local.bat
- ✅ P3-2: .clang-format導入
- ✅ P3-6: ConfigLoaderフォールバック修正
- ✅ P3-7: プロトコルユーティリティ
- 📋 P3-3: docstring強化
- 📋 P3-4: Android側メーカー固有対応
- ✅ P3-5: AOA Protocol経由のタッチ動作確認済み (HID不要)

## 実機検証結果 (2026-02-13)

| 項目 | 結果 | 備考 |
|------|------|------|
| AOA v2 VID/PID切替 | ✅ 870ms | 0E8D:201C → 18D1:2D01 |
| AOA双方向通信 | ✅ | PING→ACK, TAP→ACK |
| TAP (AccessibilityService) | ✅ 57ms | dispatchGesture |
| SWIPE | ✅ 317ms | 300ms gesture + 17ms overhead |
| BACK | ✅ 即時 | performGlobalAction |
| LONGPRESS | ✅ 805ms | 800ms gesture + 5ms overhead |
| KEY (Home) | ✅ 即時 | — |
| USB権限自動承認 | ✅ | uiautomator dump + auto-tap |
| WiFi ADB永続化 | ✅ | BootReceiver reboot後復旧確認 |
| MediaProjection bypass | ✅ | appops PROJECT_MEDIA allow |
| AccessibilityService | ✅ | 自動有効化・reboot後も維持 |
| Accessory preferences | ✅ | 「常に使用」保存済み |

## テスト統計

| スイート | 件数 | 状態 |
|---------|------|------|
| mirage_tests (gtest) | 104 | ✅ |
| test_device_registry | 12 | ✅ |
| test_protocol | 10 | ✅ |
| test_aoa_hid_touch | 8 | ✅ |
| test_rtt_tracker | 27 | ✅ |
| test_vulkan_compute | 54 | ✅ |
| aoa_full_test (実機) | 6 | ✅ |
| **合計** | **221** | **✅** |

## 残タスク

### 次フェーズ: 映像パイプライン統合
1. CMakeLists.txtにh264_decoder, video_texture追加
2. Android captureモジュールAPKビルド・インストール
3. AccessoryIoService → ScreenCaptureService IPC接続
4. USB VID0フレーム受信 → H264デコード → ImGUI表示
5. TCP/WiFiフォールバック映像経路テスト

### 低優先
- P2-4: エンディアン明示
- P3-3: docstring強化
- P3-4: メーカー固有対応 (Samsung OneUI, Xiaomi MIUI等)
