# CURRENT_TRUTH.md

**MirageSystem 現行状態スナップショット**

作成日: 2026-03-13
目的: 統合フェーズ開始前の基準線。変更前の状態を確定し、差分計測の起点とする。

> このドキュメントは「現実」を記録する。希望・予定・将来の設計は含めない。
> 変更を加えたら必ずこのドキュメントを更新し、git diffで基準線との差分を追える状態を維持する。

---

## 1. デバイス構成

| デバイス | WiFi ADB | USB Serial | AOA PID | 備考 |
|---------|----------|-----------|---------|------|
| Npad X1 | 192.168.0.3:5555 | 93020523431940 | 0x201C | メイン開発機 (MTK) |
| A9 #956 | 192.168.0.6:5555 | A9250700956 | 0x201C | サブ機 |
| A9 #479 | 192.168.0.8:5555 | A9250700479 | 0x201C | サブ機 |

**USB HUB**: CT-USB4HUBV2 (ReTRY HUB) → **撤去済み**。リモート制御不良によりハブを撤去。USB復旧は手動操作のみ。

**GPU**: AMD Radeon Graphics (id:0x1638, integrated), Vulkan 1.3.260, driver 2.0.279, AMD proprietary 26.1.1

---

## 2. 唯一の正ルート

```
WiFi ADB接続
    ↓
scrcpy-server 映像取得
    ↓
Vulkan GUI 表示
```

これ以外のルートは全て非推奨または棚上げ:
- **USB ADB直接接続**: セキュリティリスクのため非推奨。ADBはフォールバック/デバッグ用途のみ
- **Bluetooth ADB**: 日本語UI自動化の課題により棚上げ
- **AOA (Android Open Accessory)**: 映像・操作のメイン経路として設計中だが、本実装は未完了

---

## 3. 映像パイプライン

2レーン同時走行構成（2026-03時点で安定稼働確認済み）:

| レーン | 解像度 | コーデック | 用途 | 動作モード |
|--------|--------|----------|------|----------|
| Canonical | 1200×2000 | JPEG | AI Vision入力 / 高解像度保存 | 常時 |
| Presentation | 1920×1080 | H.264 | GUI表示 / リアルタイムモニタリング | 動的 |

**ベンチマーク結果** (Phase C-0〜C-4 完了):
- H.264 ソフトデコーダ: 720×1200@30.5fps、エラーゼロ
- Npad X1: HWエンコードに制限あり（MTK固有）

**通信設計 (2系統×2パイプ)**:
- 操作系 + 画像系を USB + WiFi の2パイプで運用
- USB優先。帯域圧迫時は画像をWiFiに逃がす。WiFi不可ならFPS可変
- メイン: 60〜30fps / サブ: 30〜10fps
- 回線障害時は生存回線に全系統避難

### フレーム配信経路（現行・実コード検証済み 2026-03-13）

```
[Android] MediaProjection → MediaCodec → scrcpy-server
    ↓ (WiFi ADB経由)
[PC] MirrorReceiver (2スレッドパイプライン)
    ├─ 受信スレッド: UDP/TCP/VID0/外部フィード → enqueue_nal()
    │   → NAL queue (condition_variable通知, max 512)
    ├─ デコードスレッド: decode_thread_func()
    │   → UnifiedDecoder → on_unified_frame(const uint8_t* rgba)
    │   → Copy #1: std::memcpy(buf.get(), rgba, frame_bytes)
    │     [FrameBufferPool::acquire()でプールからバッファ取得、カスタムdeleterで返却]
    │   → current_shared_frame_ = SharedFrame (frame_mtx_内で上書き)
    │
    └─ GUI側: get_latest_shared_frame() → shared_ptr取得
        → VulkanTexture::update()
        → Copy #2: staging buffer → GPU upload
```

**コピー回数: 2回**（実コード検証による修正。旧記載の「4回」は概念的表現だった）
- Copy #1: decode→SharedFrame memcpy (on_unified_frame内)
- Copy #2: staging buffer→GPU upload (VulkanTexture::update内)
- 理論帯域: 1920×1080×4bytes×30fps = 248MB/s (Copy #1のみ)

**フレーム管理の実態**:
- FrameBufferPoolが既存（malloc/freeオーバーヘッドほぼゼロ）
- 上書きモデル: shared_ptrの差し替えのみ、古いフレームはrefcount=0で自動回収
- GUI側が読みに来る前に次フレームが来ると前フレームはサイレントに消える（dropped framesカウントなし = A-1で追加予定）

**A-1 RingBuffer挿入時の設計判断ポイント**:
- 挿入点: on_unified_frame()内だが、ここはframe_mtx_の中
- RingBufferのpush()がmutex内に入るとGUI側pop()と排他になる
- 選択肢: lock-free RingBuffer or mutex粒度変更
- Copy #1のゼロコピー化にはUnifiedDecoderのコールバックI/F変更が必要（`const uint8_t*` raw pointer制約）
- A-1の主目的は「最新1枚上書きモデル」を複数スロット保持に置き換え、drop可視化・可観測性強化・GUI/Layer2消費タイミングの安定化を図ること

**dedup整合リスク** (A-1実装時の確認必須):
- 現行の上書きモデルではLayer2は最新1枚のみ取得 → dedupは「前回と今回の差分」で動作
- RingBuffer化でLayer2が複数フレームを順番に処理する可能性 → フレーム間隔変化でdedup率が変動しうる
- 確認方法: RingBuffer経由でもraw=N→Mのログが従来と同等であること

**過去の修正済み問題**:
- 映像固まり問題 → update()経路で解決済み（stageUpdate経路のデータレース排除）
- gui_application.cppのフレーム更新はVulkanTexture::update()に統一済み

---

## 4. AI / Vision パイプライン

### レイヤー構成

```
Layer 0  Hardware / Devices
Layer 1  Video transport (scrcpy-server → MirrorReceiver)
Layer 2  Vision dedup (UiElementHit / Layer2Input基盤)
Layer 3  AI reasoning (llava:7b via Ollama)
Layer 4  Automation (未実装: Runtime最小版 = B-4タスク)
Layer 5  MCP orchestration (v5.0.0 安定稼働)
```

### Layer 1→Layer 2 I/F (2026-03-12完成, commit `e5a6e4f`)

- UiElementHit / Layer2Input / dedup基盤が完成
- dedupパラメータ: IoU≥0.5 / dist<24px / text≥0.85（保守的設定）
- buildLayer2Input内でraw収集→dedup→candidate生成の順
- ai_engine消費側もLayer2Inputベースに接続済み
- 現行Layer2は「最新1枚取得」前提で動作しており、A-1で複数フレーム保持に変わる場合はdedup temporal behaviorの再検証が必要

### Layer 3 (AI Vision)

- モデル: llava:7b（Ollama経由）
- 入力サイズ: 336→224pxに縮小済み（総レイテンシ41%削減）
- `ai.vde_enable_layer3` = true（config.json修正済み）
- **cold startレイテンシ: 約65秒**（モデルロード時間が支配的）
- **warmup後レイテンシ: 約3.7秒**（warmupAsync() + keep_alive設定で実現）
- 注意: keep_aliveが切れるとcold startに戻る

### Layer 4 (Automation)

- **未実装**。B-4タスクで最小版（vision→tap 1パス）を実装予定
- 設計方針: vision結果1件→ルール一致→tap 1回→実行ログ。分岐・リトライなし

---

## 5. AOA (Android Open Accessory)

### MTKデバイスのUSB PID構造

| モード | PID | sys.usb.config | 備考 |
|--------|-----|----------------|------|
| ADB only | 0x2005 | adb | RNDIS+ADB複合 |
| ADB+AOA複合 | **0x201C** | adb（変化しない） | WinUSBドライバ必要 |
| RNDIS+ADB | 0x2005 | rndis,adb | 映像USB経由転送時 |

> ⚠️ MTKデバイスは `sys.usb.config=adb` のままでも PID=0x201C で列挙される。
> sys.usb.config の変化でAOAモード判定はできない。

### 修正済みバグ

- `usb_device_discovery.cpp` 行32: PID=0x2005を誤ってAOAリストに含めていた → 修正済み
- `setprop sys.usb.config` はSELinuxで拒否される → `svc usb setFunctions` を使用

### AOA接続フロー（正常時）

```
[PC GUI起動]
    ├─ PID=0x201C 検出 → WinUSBでopen
    │       ↓
    │   EP_OUT送信 → Android側 HIDデバイス作成
    │       ↓
    │   AccessoryIoService が /dev/usb_accessory をopen
    │       ↓
    │   EP_IN受信開始 → ACK通信確立
    └─ 成功: "USB AOA mode (1 device(s)) - dual-channel active"
```

---

## 6. MCPサーバー

### 基本情報

- バージョン: v5.0.0
- プロトコル: Streamable HTTP
- 完成度: 88%（残課題: AOAフルパス検証、メモリDB蓄積）
- 場所: `C:/MirageWork/mcp-server`
- 起動方式: Watchdog経由（二重起動防止のPIDファイル付き）
- 公開: Cloudflare Tunnel (`mcp.mirage-sys.com`)
- LLMフォールバック: Cerebras → Groq（APIキーはレジストリから安全に取得）

### Pipeline Engine

- 3-stage quality gates: BuildGate → TestGate → AIReview
- TestGateはCTestTestfile.cmake不在時に自動スキップ → **実質2段階で動作中**
- Dynamic Model Selection: Sonnet/Opus自動選択（コスト50-70%削減）
- classify_error(): TRANSIENT / CODE_ERROR / PERMANENT の3分類
- ParallelScheduler: 依存グラフベースの並列実行（opt-in）
- DeviceLockManager: デバイス別ADB排他ロック（30秒タイムアウト）
- テスト: 66/66全通過
- session_context.md: パイプライン状態のコンテキスト復旧用

### BufferProxy / 接続管理

- BufferProxy v2で同時接続数を制限
- SSEバイパスで502エラー防止
- CloudflaredReplicaを廃止しPrimary単体運用に統合

### 外部記憶 (MirageMemory)

| namespace | 用途 | レコード数 |
|-----------|------|-----------|
| mirage-vulkan | Vulkan・描画・レンダリング | 37 |
| mirage-android | ADB・AOA・デバイス操作 | 23 |
| mirage-infra | MCP・サーバー・Cloudflare | 1,743 |
| mirage-design | アーキテクチャ・UI設計 | - |
| mirage-general | 雑談・その他 | - |

---

## 7. 起動順序

```
1. Watchdog
2. MCPサーバー (PIDファイルで二重起動防止)
3. Cloudflared (mcp.mirage-sys.com トンネル確立)
4. mirage_vulkan.exe (DLL配置+バッチ起動方式)
```

### 起動時の注意

- mirage_vulkan.exe起動前にDLL配置を確認（過去に起動バグあり→修正済み、start /wait使用）
- ビルド前に実行中プロセスを `taskkill /F` で終了させること
- WiFi ADBはリブート後にOFFになる → 自動再有効化が必須
- リブートを伴う操作はClaude Codeへの指示書に必ず明記すること
- Android 15のMediaProjection権限取得: adb input tap / UIAutomatorで自動化済み

---

## 8. ログ場所

| コンポーネント | ログ場所 |
|---------------|---------|
| MCPサーバー | `C:/MirageWork/mcp-server/logs/` |
| GUI | stdout |
| Vulkan | debug callback |
| Cloudflared | Windowsサービスログ |
| Watchdog | MCPサーバーログと統合 |

---

## 9. 既知の不安定ポイント

| 項目 | 状態 | 影響 |
|------|------|------|
| Layer 3 VDE cold start | llava:7bで約65秒 | Vision初回推論が遅い |
| CPU fallback | HW decode失敗時に発動 | 映像品質低下 |
| keep_alive切れ | Ollamaモデルunload | cold startに戻る |
| WiFi ADB切断 | ネットワーク不安定時 | 全操作不能（Watchdog自動復元機能あり） |
| Android 15 MediaProjection | 権限取得に追加操作必要 | 自動化済みだが端末差分リスクあり |
| PC電源管理 | スリープ/USBサスペンド | 無効化済み（ADB切断問題の根本解決） |
| フレームドロップ未検出 | 上書きモデルでサイレント消失 | dropped framesカウントなし（A-1で追加予定） |

---

## 10. ファイル構成（主要）

### PC側 (C:/MirageWork/MirageVulkan)

| ファイル | 責務 | A-1影響 |
|---------|------|---------|
| src/mirror_receiver.cpp | 2スレッドパイプライン（受信→NAL queue→デコード→SharedFrame）、FrameBufferPool管理 | **RingBuffer挿入点** (on_unified_frame内, frame_mtx_内) |
| src/gui_application.cpp | GUI表示・フレーム更新 (get_latest_shared_frame→VulkanTexture::update) | **影響範囲** (フレーム取得I/F変更) |
| src/vulkan_texture.cpp | Vulkanテクスチャ管理、staging buffer→GPU upload (Copy #2) | **影響範囲** (変更は小) |
| src/usb_device_discovery.cpp | AOA PIDリスト・switch処理 | - |
| src/aoa_protocol.cpp | open_aoa_device・switch_device_to_aoa_mode | - |
| src/aoa_hid_touch.cpp | AOA HIDタッチ操作 | - |
| src/multi_usb_command_sender.cpp | EP_INエラーハンドリング | - |
| CLAUDE.md | Claude Code CLI用指示書 | - |
| PROJECT_STATE.md | プロジェクト状態記録 | - |

### Android側

| ファイル | 責務 |
|---------|------|
| AccessoryIoService.kt | Android側 accessory open・IO処理 |
| AccessoryActivity.kt | 1秒ポーリング・Permission要求UI |
| com.mirage.capture | 統合済みキャプチャアプリ |

### MCPサーバー (C:/MirageWork/mcp-server)

| ファイル | 責務 |
|---------|------|
| server.py | MCPサーバー本体・ツールハンドラ |
| task_queue.py | Pipeline Engine・品質ゲート・エラー分類 |

---

## 11. 変更履歴

| 日付 | 変更内容 | commit |
|------|---------|--------|
| 2026-03-13 | セクション3: 実コード検証によりcopy chain 4回→2回に修正、mirror_receiver.cpp構造詳細追記 | - |
| 2026-03-13 | セクション4: Layer2にdedup temporal behavior再検証の注記追加 | - |
| 2026-03-13 | CURRENT_TRUTH.md 初版作成 | - |
| 2026-03-12 | Layer1→Layer2 I/F完成、dedup基盤完成 | `e5a6e4f` |
| 2026-03-11 | AOA USBリセット手順書作成、PID=0x2005バグ修正 | - |
| 2026-02-18 | Pipeline Engine大規模拡張 (66/66テスト通過) | - |

---

> **このドキュメントの更新ルール:**
> 1. 実装変更を加えたら、影響するセクションを即座に更新する
> 2. 「希望」や「予定」は書かない。「現在の事実」のみ記載する
> 3. git commitと一緒にCURRENT_TRUTH.mdの更新もcommitする
> 4. 95%判定時に、このドキュメントが現実と100%一致していることが判定基準の1つ
