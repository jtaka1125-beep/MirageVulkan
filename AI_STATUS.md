# AI Vision System Status
# Updated: 2026-03-11

## 3層アーキテクチャ

| Layer | 状態 | 機能 | 負荷 | オーバーレイ色 |
|-------|------|------|------|---------------|
| Layer 0 | STANDBY | 待機モード (テンプレートマッチング停止) | 低 | なし |
| Layer 1 | IDLE/DETECTED/CONFIRMED/VERIFYING/COOLDOWN | VulkanTemplateMatcher | 中 | スコア基準 |
| Layer 2.5 | LfmClassifier | qwen3:0.6b テキスト分類 | 低 | - |
| Layer 3 | OllamaVision | llava-phi3 ポップアップ検出 | 高 | マゼンタ |

## テンプレート状況 (2026-03-11)
- 自動生成: 6枚 (Layer 3による自動収集)
- 保存先: build/templates/
- タグ: layer3_auto
- 重複排除: NCC類似度 > 0.85 でスキップ

## 状態遷移トリガー

- **Layer 0 → 1**: 操作なし 5秒
- **Layer 1 → 0**: ユーザー操作検出 (tap/swipe/pinch)
- **Layer 1 → 3**: 60フレーム(~2秒)マッチなし or 90フレーム(~3秒)同一テンプレート
- **Layer 3 → 1**: ポップアップ検出・アクション実行後

## アクション検証 (VERIFYING状態) - 2026-03-11追加

アクション実行後、ポップアップが消失したか検証するフェーズ:

| 設定項目 | デフォルト | 説明 |
|---------|-----------|------|
| enable_verify | false | 検証を有効化（後方互換のためデフォルトOFF）|
| verify_delay_ms | 500 | アクション後、検証開始までの待機時間 |
| verify_timeout_ms | 2000 | 検証タイムアウト |
| verify_max_retry | 2 | 最大リトライ回数 |

状態遷移:
- enable_verify=false: CONFIRMED → TAP → COOLDOWN (従来動作)
- enable_verify=true:  CONFIRMED → TAP → VERIFYING → (消失確認) → COOLDOWN
                       VERIFYING → (テンプレート残存) → リトライTAP → VERIFYING

## AI モデル選定 (2026年ベンチマーク)

| モデル | ポップアップ検出 | 日本語OCR | 備考 |
|--------|-----------------|----------|------|
| qwen2.5vl:3b | ~1秒 ✅ | ✅ | ローカル高速 |
| qwen3-vl:4b | ~12秒 ✅ | ✅ | ローカル高精度 |
| llava-phi3 | ~2秒 ✅ | ⚠️ | 現在使用中 |
| qwen3:0.6b | ~0.5秒 | - | LfmClassifier用 |

## テスト結果 (2026-03-11)
- CTest: 32/32 PASSED
- VDE: 40/40 PASSED
- TemplateStore: 12/12 PASSED
- TemplateManifest: 21/21 PASSED

---

## エンコーダー解像度仕様 (Android H264Encoder)

**重要**: Android側のMediaProjection/VirtualDisplayはFHD(1080x1920)以上の解像度を要求しても、
システム側で勝手にFHDにスケールダウンされる挙動がある。

### 解像度決定ルール
| 物理解像度 | エンコード解像度 | 備考 |
|-----------|-----------------|------|
| FHD以下 (≤1080x1920) | ネイティブ解像度そのまま | A9 (800x1340) など |
| FHD超 (>1080x1920) | FHDに収まるよう縮小 | X1 (1200x2000) → 1080x1800 (90%) |

### 縮小計算
FHD超の場合、アスペクト比を維持したまま長辺が1920以下になるように縮小:
- X1: 1200x2000 → scale = min(1080/1200, 1920/2000) = 0.9 → 1080x1800

### デバイス別設定 (devices.json)
| デバイス | 物理解像度 | エンコード解像度 | screen_width/height設定 |
|---------|-----------|-----------------|------------------------|
| A9 | 800x1340 | 800x1340 (native) | 800x1340 |
| X1 (Npad X1) | 1200x2000 | 1080x1800 (90%) | 1080x1800 |

> ※ 座標マッピング用のscreen_width/heightはエンコード解像度を設定する。
