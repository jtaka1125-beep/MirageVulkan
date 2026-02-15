# MirageComplete → MirageVulkan 移行計画

**作成日**: 2026-02-16
**ステータス**: ドラフト

---

## 1. Executive Summary

MirageVulkanはMirageCompleteのC++コア(src/)をほぼ完全に引き継いでおり、さらにVulkan Video H.264デコード、GPUコンピュートシェーダ、包括的テストスイート(10スイート)を追加した**上位互換**プロジェクトである。

移行の主眼は、MirageCompleteに残る**周辺エコシステム**（Androidアプリ、Pythonスクリプト、ドライバインストーラ、マクロエディタ等）をMirageVulkanに統合し、単一リポジトリとして運用することにある。

**推定工数**: Phase 1 (1-2日) / Phase 2 (2-3日) / Phase 3 (1日)

---

## 2. 現状比較

### 2.1 コードベース規模

| 指標 | MirageComplete | MirageVulkan |
|---|---|---|
| C++ソースファイル (src/) | ~135ファイル | ~101ファイル |
| 総行数 | ~38,000行 | ~25,300行 |
| テストスイート | 0 | 10 (全PASS) |
| ビルドシステム | CMake | CMake (最適化済) |
| Git管理 | あり | あり (4コミット) |
| config外部化 | なし | config.json対応済 |

### 2.2 C++ src/ ファイル差分

**MirageCompleteにのみ存在:**
- なし（全ファイルがVulkanに移行済み）

**MirageVulkanにのみ存在（新規追加）:**
- `src/video/vulkan_video_decoder.cpp/.hpp` — Vulkan Video H.264デコード
- `src/video/h264_parser.cpp/.hpp` — NAL Unitパーサー
- `src/video/yuv_converter.cpp/.hpp` — GPU YUV→RGBA変換
- `src/video/unified_decoder.cpp/.hpp` — Vulkan/FFmpegフォールバック統合
- `src/vulkan/vulkan_image.cpp/.hpp` — Vulkanイメージ管理
- `shaders/` — 7つのGPUコンピュートシェーダ (.comp)

### 2.3 MirageCompleteの周辺エコシステム（Vulkan未統合）

| ディレクトリ | 内容 | ファイル数 |
|---|---|---|
| `android/` | MirageServer Android APK (Kotlin/Java) | ~23ファイル |
| `scripts/` | Python/Bat自動化スクリプト | ~27ファイル |
| `tools/` | AOA/BT/USB テスト・デバッグツール | ~40ファイル |
| `driver_installer/` | WinUSBドライバインストーラ | - |
| `setup/` | 初期セットアップ関連 | - |
| `auto_setup/` | 自動セットアップ | - |
| `macro_editor/` | マクロエディタ (Python) | ~3ファイル |
| `docs/` | ドキュメント | - |
| `usb_driver/` | USBドライバ関連 | - |
| `integration_staging/` | 統合ステージング | - |

---

## 3. 移行対象コンポーネント（優先度付き）

### P1: 必須（Phase 1）

| コンポーネント | 理由 |
|---|---|
| `android/` | デバイス側APKがないとミラーリング不可 |
| `scripts/mirage_cli.py` | 運用に必須のCLIツール |
| `scripts/auto_setup_devices.py` | デバイス初期設定自動化 |
| `docs/` | 運用ドキュメント |

### P2: 推奨（Phase 2）

| コンポーネント | 理由 |
|---|---|
| `driver_installer/` | WinUSBセットアップの自動化 |
| `setup/` | 環境構築手順 |
| `scripts/bt_auto_pair.py` | Bluetooth自動ペアリング |
| `scripts/usb_lan_controller.py` | USB LAN制御 |
| `scripts/adb_video_capture.py` | デバッグ用キャプチャ |
| `macro_editor/` | マクロ自動化GUI |

### P3: 低優先/廃止検討（Phase 3）

| コンポーネント | 判断 |
|---|---|
| `tools/aoa_*.cpp/exe` | デバッグ用、本番不要 → アーカイブ |
| `tools/bt_*.py/ps1` | src/bt_auto_pair.cppに統合済 → 廃止 |
| `integration_staging/` | 一時的な統合テスト領域 → 廃止 |
| `auto_setup/` | scripts/に統合 → 廃止 |
| `usb_driver/` | driver_installer/に統合 → 廃止 |

---

## 4. 移行フェーズ計画

### Phase 1: コア統合（1-2日）

```
1. android/ ディレクトリをMirageVulkanにコピー
2. mirage.keystore の配置確認
3. 必須スクリプトの移行:
   - scripts/mirage_cli.py
   - scripts/auto_setup_devices.py
   - scripts/config_loader.py
4. docs/ の移行
5. README.md 統合
6. ビルド確認: Android APK + Windows PC双方
7. E2Eテスト: 実機ミラーリング動作確認
```

### Phase 2: 運用ツール統合（2-3日）

```
1. driver_installer/ の移行・動作確認
2. setup/ の移行
3. 運用スクリプト群の移行:
   - bt_auto_pair.py
   - usb_lan_controller.py
   - adb_video_capture.py
   - hybrid_video_viewer.py
4. macro_editor/ の移行
5. scripts/__pycache__ 等の除外 (.gitignore更新)
6. CI/CD パイプライン検討
```

### Phase 3: クリーンアップ（1日）

```
1. tools/ から必要なものだけ tools/archive/ に保存
2. integration_staging/ 廃止
3. auto_setup/ → scripts/ 統合確認後に廃止
4. usb_driver/ → driver_installer/ 統合確認後に廃止
5. .gitignore 最終整理
6. MirageComplete リポジトリを archived に変更
```

---

## 5. リスクと対策

| リスク | 影響度 | 対策 |
|---|---|---|
| Android APKビルド環境の差異 | 高 | gradlew clean build で検証、local.properties再設定 |
| Pythonスクリプトのパス依存 | 中 | config_loader.py の相対パス化、config.json統合 |
| WinUSBドライバ署名 | 中 | 既存署名済みドライバをそのまま移行 |
| FFmpegライブラリパス | 低 | CMake PKG_CONFIG_PATH で対応済み |
| Git履歴の喪失 | 低 | MirageCompleteリポジトリをアーカイブとして保持 |

---

## 6. テスト戦略

### 移行前（現状）
- MirageVulkan: 10テストスイート全PASS ✅
- MirageComplete: テストなし

### 移行中
- 各Phase完了時にRelease/Debugビルド確認
- ctest 全PASS維持
- Android APK: `gradlew assembleRelease` 成功確認

### 移行後の追加テスト候補
- E2E: 実機USB接続 → ミラーリング → タッチ操作
- E2E: WiFiフォールバック → ミラーリング
- スクリプト: mirage_cli.py 基本コマンド
- ドライバ: WinUSBインストール（手動テスト）

---

## 7. ロールバック計画

- MirageCompleteリポジトリは**Phase 3完了まで変更しない**
- 各Phase完了時にMirageVulkanでGitタグを打つ:
  - `v1.1-phase1-migration`
  - `v1.2-phase2-tools`
  - `v2.0-unified`
- 問題発生時: 該当Phaseのコミットをrevertし、MirageCompleteに戻る
- MirageCompleteのアーカイブ化はPhase 3完了+1週間運用後

---

## 8. 結論

MirageVulkanはC++コア部分で既にMirageCompleteの**完全上位互換**を達成している。移行作業の本質はAndroid APK・スクリプト・ドライバ等の**周辺ツールチェーン統合**であり、コードレベルの大規模変更は不要。

推奨アプローチ: Phase 1から段階的に実行し、各フェーズ完了時にE2E動作確認を行う。
