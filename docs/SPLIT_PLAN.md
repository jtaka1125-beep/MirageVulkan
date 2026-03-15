# 大規模ファイル分割計画

作成日: 2026-03-15
対象: ai_engine.cpp (21,539行/134KB), gui_init.cpp (1,312行/65KB), mirror_receiver.cpp (1,530行/55KB), gui_threads.cpp (889行/51KB)

---

## 1. ai_engine.cpp 分割計画（最優先）

### 1.1 現状の構造

```
ai_engine.cpp (21,539行)
├─ Includes & namespace          (L1-623,     623行)
├─ anonymous namespace
│  ├─ AiTemplateSource class     (L680-731,    52行)
│  └─ ActionMapper class         (L781-1805, 1,025行)
├─ AIEngine::Impl class          (L1901-20650, 18,750行)
│  ├─ メンバ変数宣言              (L1901-1932)
│  ├─ 初期化・終了               (L1933-3179, 1,247行)
│  ├─ テンプレート接続            (L3275-3531,  257行)
│  ├─ テンプレート管理            (L3627-5552, 1,926行)
│  ├─ processFrame (メインループ) (L5648-8460, 2,812行) ← 最大メソッド
│  ├─ 統計・状態                 (L8559-9005,  447行)
│  ├─ VDE状態機械                (L8799-9005,  207行)
│  ├─ テンプレート無視リスト      (L8963-8982,   20行)
│  ├─ VDE/Jitter設定             (L9036-9564,  529行)
│  ├─ OCR・テキストキーワード     (L9620-10100, 481行)
│  ├─ デバイス適応                (L9712-9904,  193行)
│  ├─ ホットリロード              (L10136-10696, 561行)
│  ├─ 非同期処理                 (L10696-12254, 1,559行)
│  ├─ フラグ・イベント            (L12254-12390, 137行)
│  └─ (残り: ヘルパー群)         (~5,000行)
├─ AIEngine公開ラッパー           (L18553-20650, 2,098行)
└─ #ifndef USE_AI スタブ          (L20652-21539, 888行)
```

### 1.2 分割案

#### ファイル構成（7ファイルに分割）

| # | 新ファイル名 | 移動する内容 | 推定行数 | 優先度 |
|---|-------------|-------------|---------|--------|
| 1 | `src/ai/action_mapper.cpp` | ActionMapper class全体 + ScreenState enum | ~1,100行 | P1 |
| 2 | `src/ai/action_mapper.hpp` | ActionMapper公開インターフェース | ~80行 | P1 |
| 3 | `src/ai_engine_init.cpp` | Impl::initialize(), shutdown(), setTemplateStore(), setFrameAnalyzer() | ~1,600行 | P2 |
| 4 | `src/ai_engine_templates.cpp` | Impl::loadTemplatesFromDir(), addTemplate(), clearTemplates(), registerLayer2Template(), computeNccSimilarity(), maxSimilarityFromManifest() | ~2,500行 | P2 |
| 5 | `src/ai_engine_frame.cpp` | Impl::processFrame(), cacheMatches(), decideAction(), tryOcrFallback(), getLastMatches() | ~3,200行 | P3 |
| 6 | `src/ai_engine_async.cpp` | Impl::startAsyncWorkers(), stopAsyncWorkers(), enqueueAsync*(), flushPendingActions(), ホットリロード関連 | ~2,200行 | P3 |
| 7 | `src/ai_engine.cpp` (残留) | Impl class定義, メンバ変数, 統計, VDE設定, OCR, デバイス適応, AIEngine公開ラッパー, スタブ | ~10,800行 | - |

#### 各ファイルの詳細

**[P1] src/ai/action_mapper.cpp + action_mapper.hpp**
```
移動対象:
  - ActionMapper class (L781-1805)
    - addTemplateAction()
    - getAction()
    - classifyState() + ScreenState enum
    - registerTextAction() / removeTextAction() / hasTextAction()
    - getTextAction() / getTextKeywords()
  - AiTemplateSource class (L680-731) → action_mapper.cppのanonymous namespaceへ

依存関係:
  - 入力: VkMatchResult, unordered_map<int,string>
  - 自己完結度: 高い（外部依存少ない）
  - ai_engine.cppからはunique_ptr<ActionMapper>で保持

理由: 最も自己完結しており、分離リスクが最小
```

**[P2] src/ai_engine_init.cpp**
```
移動対象:
  - Impl::initialize() (L1981-2893, ~912行)
  - Impl::shutdown() (L2925-3179, ~255行)
  - Impl::setTemplateStore() (L3275-3323)
  - Impl::setFrameAnalyzer() (L3419-3531)

依存関係:
  - VulkanContext, VulkanComputeProcessor, VulkanTemplateMatcher
  - VisionDecisionEngine, ContinuousLearningV2, LfmClassifier
  - ActionMapper, TemplateStore, EventBus
  - すべてのメンバ変数にアクセス

include必要: ai_engine_impl.hpp (後述の内部ヘッダ)
```

**[P2] src/ai_engine_templates.cpp**
```
移動対象:
  - Impl::loadTemplatesFromDir() (L3627-4500, ~873行)
  - Impl::addTemplate() (L4532-5060, ~529行)
  - Impl::computeNccSimilarity() (L5070-5096)
  - Impl::maxSimilarityFromManifest() (L5099-5118)
  - Impl::addLayer2Detection() (L5121-5127)
  - Impl::registerLayer2Template() (L5133-5431, ~299行)
  - Impl::clearTemplates() (L5439-5552)

依存関係:
  - VulkanTemplateMatcher, VulkanComputeProcessor
  - TemplateStore, TemplateManifest, TemplateWriter
  - id_to_name_, id_to_tags_ maps
  - ActionMapper
```

**[P3] src/ai_engine_frame.cpp**
```
移動対象:
  - Impl::processFrame() (L5648-8460, ~2,812行) ← ファイル最大のメソッド
  - Impl::cacheMatches()
  - Impl::getLastMatches() (L8492-8527)
  - tryOcrFallback() (OCR有効時)

依存関係: ほぼ全メンバ変数を参照（最も結合度が高い）
  - perception::detect(), VisionDecisionEngine
  - ActionMapper, ContinuousLearningV2, LfmClassifier
  - Layer2Client (async invocation)
  - matches_mutex_, layer2_matches_mutex_

注意: processFrame()は最大メソッドだが最も結合度が高い。
      分離は有効だが、ai_engine_impl.hppへの依存が最も大きい。
```

**[P3] src/ai_engine_async.cpp**
```
移動対象:
  - Impl::startAsyncWorkers() (L10696-11297, ~602行)
  - Impl::stopAsyncWorkers() (L11297-11425)
  - Impl::enqueueAsyncFrame() (L11457-11481)
  - Impl::enqueueAsyncFrameShared() (L11481-11559)
  - Impl::flushPendingActions() (L11671-12254, ~584行)
  - Impl::startHotReload() (L10136-10584, ~449行)
  - Impl::stopHotReload() (L10584-10696)
  - Impl::setHotReload() (L11559-11671)

依存関係:
  - async_work_queue_, async_queue_mutex_, async_queue_cv_
  - pending_actions_, async_worker_
  - hot_reload_thread_, manifest_last_mtime_
  - processFrame() を呼び出す（ai_engine_frame.cppに依存）
```

### 1.3 Pimplパターンを維持した分割テクニック

#### 内部ヘッダ: `src/ai_engine_impl.hpp`（新規作成）

```cpp
// ai_engine_impl.hpp — AIEngine::Impl クラス定義（内部専用）
// 公開ヘッダ(ai_engine.hpp)には露出しない
#pragma once
#include "ai_engine.hpp"
#include <mutex>
#include <thread>
#include <atomic>
#include <queue>
// ... 内部依存

namespace mirage::ai {

// forward declarations
class ActionMapper;
class VisionDecisionEngine;
class ContinuousLearningV2;

class AIEngine::Impl {
public:
    // --- Lifecycle ---
    Impl();
    ~Impl();
    mirage::Result<void> initialize(const AIConfig& config, mirage::vk::VulkanContext* vk_ctx);
    void shutdown();

    // --- Template Management ---
    mirage::Result<void> loadTemplatesFromDir(const std::string& dir);
    bool addTemplate(const std::string& name, const uint8_t* rgba, int w, int h);
    void clearTemplates();
    // ... 全メソッド宣言

    // --- Frame Processing ---
    AIAction processFrame(int slot, const uint8_t* rgba, int width, int height, bool can_send);
    std::vector<AIEngine::MatchRect> getLastMatches() const;
    void cacheMatches(const std::vector<VkMatchResult>& results);

    // --- Async ---
    void startAsyncWorkers(std::function<void(int,const AIAction&)>* cb_ptr);
    void stopAsyncWorkers();
    // ... etc

private:
    // 全メンバ変数をここに宣言
    bool initialized_ = false;
    AIConfig config_;
    mirage::vk::VulkanContext* vk_ctx_ = nullptr;
    std::unique_ptr<mirage::vk::VulkanComputeProcessor> vk_processor_;
    std::unique_ptr<mirage::vk::VulkanTemplateMatcher> vk_matcher_;
    std::mutex vk_processor_mutex_;
    // ... 残りのメンバ変数
};

} // namespace mirage::ai
```

#### 分割後の各.cppファイルの構造

```cpp
// ai_engine_templates.cpp
#include "ai_engine_impl.hpp"  // Impl定義にアクセス
#include "ai/template_manifest.hpp"
#include "ai/template_writer.hpp"
// ...

namespace mirage::ai {

mirage::Result<void> AIEngine::Impl::loadTemplatesFromDir(const std::string& dir) {
    // そのまま移動（メンバアクセスは this-> 経由で同一）
}

bool AIEngine::Impl::addTemplate(...) { ... }
void AIEngine::Impl::clearTemplates() { ... }

} // namespace mirage::ai
```

#### ポイント

1. **ai_engine.hpp（公開ヘッダ）は変更不要** — `class Impl;` のforward declarationのみ
2. **ai_engine_impl.hpp（内部ヘッダ）を新設** — Impl classの完全な定義を持つ
3. **分割後の各.cppは `ai_engine_impl.hpp` をinclude** — メンバ変数に直接アクセス可能
4. **リンク時に結合** — 同一クラスのメソッド定義を複数.cppに分散させるのはC++で合法
5. **インクリメンタルビルド**: 各.cppの変更は自身の.objのみ再ビルド

### 1.4 CMakeLists.txt の変更

```cmake
# 既存
set(CORE_SOURCES
    src/ai_engine.cpp
    src/ai_engine.hpp
)

# 変更後
set(CORE_SOURCES
    src/ai_engine.cpp
    src/ai_engine.hpp
    src/ai_engine_impl.hpp      # 追加
    src/ai_engine_init.cpp       # 追加
    src/ai_engine_templates.cpp  # 追加
    src/ai_engine_frame.cpp      # 追加
    src/ai_engine_async.cpp      # 追加
)

set(AI_SOURCES
    src/ai/action_mapper.cpp     # 追加
    src/ai/action_mapper.hpp     # 追加
    # ... 既存のAI_SOURCES
)
```

---

## 2. gui_init.cpp 分割計画（P4）

### 2.1 現状: 1,312行 / 65KB

分析の結果、行数は1,312行で管理可能なサイズ。KBが大きいのは空行・コメントが多いため。
**分割は推奨するが緊急度は低い。**

### 2.2 分割案（2ファイル）

| 新ファイル名 | 移動する内容 | 推定行数 |
|-------------|-------------|---------|
| `src/gui/gui_routing.cpp` | startRouteEvalThread(), computePolicyMaxSizeFromNative(), onQualityCommand(), onFpsCommand(), onRouteCommand(), registerDevicesForRouteController(), initializeRouting() | ~270行 |
| `src/gui/gui_init.cpp` (残留) | Service起動, USB/MultiReceiver, HybridCommand, EventBus, GUI/AI初期化 | ~1,040行 |

#### 移動詳細
```
gui_routing.cpp:
  - startRouteEvalThread()                  (L659-717)
  - computePolicyMaxSizeFromNative()        (L723-739)
  - onQualityCommand()                      (L741-763)
  - onFpsCommand()                          (L765-790)
  - onRouteCommand()                        (L795-815)
  - registerDevicesForRouteController()     (L822-894)
  - initializeRouting()                     (L896-928)

依存: RouteController, BandwidthMonitor, EventBus
自己完結度: 高い（ルーティング関連で閉じている）
```

---

## 3. mirror_receiver.cpp 分割計画（P4）

### 3.1 現状: 1,530行 / 55KB

1,530行は許容範囲だが、受信パス(UDP/TCP/VID0)とデコードパスの分離は保守性向上に有効。

### 3.2 分割案（2ファイル）

| 新ファイル名 | 移動する内容 | 推定行数 |
|-------------|-------------|---------|
| `src/mirror_receiver_net.cpp` | receive_thread(), tcp_receive_thread(), tcp_vid0_receive_thread(), process_raw_h264(), process_rtp_packet(), enqueue_nal() | ~640行 |
| `src/mirror_receiver.cpp` (残留) | BitReader, SPS parse, constructor, init_decoder, start/stop, get_latest_*, decode_thread_func, decode_nal, callbacks | ~890行 |

#### 内部ヘッダ
`mirror_receiver.hpp` は既にclass定義を持つため、private メンバが見える状態なら追加ヘッダ不要。
ただし private メンバが .hpp に宣言されていない場合は `mirror_receiver_internal.hpp` が必要。

---

## 4. gui_threads.cpp 分割計画（P5 — 低優先度）

### 4.1 現状: 889行 / 51KB

889行は分割不要なサイズ。KBが大きいのは空行。
各関数がスレッドエントリポイントとして独立しており、結合度も低い。

### 4.2 提案: 分割しない

ただし将来的に `wifiAdbWatchdogThread()` (248行) が肥大化した場合は
`gui_threads_watchdog.cpp` への分離を検討。

---

## 5. 作業手順（推奨順序）

### Phase 1: ActionMapper分離（リスク最小）
```
1. src/ai/action_mapper.hpp 作成（ActionMapper class宣言）
2. src/ai/action_mapper.cpp 作成（L680-1805のコードを移動）
3. ai_engine.cpp から該当コード削除、#include "ai/action_mapper.hpp" 追加
4. CMakeLists.txt に追加
5. ビルド確認 + テスト全通過
6. git commit
```

### Phase 2: Impl定義の内部ヘッダ化
```
1. src/ai_engine_impl.hpp 作成（Impl class定義をai_engine.cppから抽出）
2. ai_engine.cpp で #include "ai_engine_impl.hpp"
3. ビルド確認（この時点では分割なし、ヘッダ抽出のみ）
4. git commit
```

### Phase 3: 初期化・終了の分離
```
1. src/ai_engine_init.cpp 作成
2. initialize(), shutdown(), setTemplateStore(), setFrameAnalyzer() を移動
3. CMakeLists.txt に追加
4. ビルド確認 + テスト全通過
5. git commit
```

### Phase 4: テンプレート管理の分離
```
1. src/ai_engine_templates.cpp 作成
2. loadTemplatesFromDir(), addTemplate(), clearTemplates() 等を移動
3. ビルド確認 + テスト全通過
4. git commit
```

### Phase 5: フレーム処理の分離
```
1. src/ai_engine_frame.cpp 作成
2. processFrame(), cacheMatches(), getLastMatches() を移動
3. ビルド確認 + テスト全通過
4. git commit
```

### Phase 6: 非同期処理の分離
```
1. src/ai_engine_async.cpp 作成
2. startAsyncWorkers(), flushPendingActions(), ホットリロード関連を移動
3. ビルド確認 + テスト全通過
4. git commit
```

### Phase 7: GUI系ファイル（余裕があれば）
```
1. src/gui/gui_routing.cpp 分離
2. src/mirror_receiver_net.cpp 分離
```

---

## 6. 分割後の最終ファイルサイズ見込み

| ファイル | 分割前 | 分割後 |
|---------|-------|-------|
| ai_engine.cpp | 21,539行 | ~10,800行 |
| ai_engine_impl.hpp | - | ~200行 |
| ai_engine_init.cpp | - | ~1,600行 |
| ai_engine_templates.cpp | - | ~2,500行 |
| ai_engine_frame.cpp | - | ~3,200行 |
| ai_engine_async.cpp | - | ~2,200行 |
| ai/action_mapper.cpp | - | ~1,100行 |
| ai/action_mapper.hpp | - | ~80行 |
| **合計** | **21,539行** | **~21,680行** (ヘッダ重複分微増) |

残留する ai_engine.cpp (~10,800行) の内訳:
- Impl メンバ変数宣言 → ai_engine_impl.hpp に移動済み
- 統計 (getStats, resetStats, reset): ~200行
- VDE状態機械 (getDeviceVisionState等): ~210行
- テンプレート無視リスト: ~60行
- VDE/Jitter設定: ~530行
- OCR/テキストキーワード: ~480行
- デバイス適応: ~190行
- フラグ/イベント: ~140行
- AIEngine公開ラッパー: ~2,100行
- USE_AIスタブ: ~890行
- ヘルパー群・残余: ~5,000行

→ **Phase 5まで完了後、残留部分が10,800行なら追加分割を検討**
  - VDE設定 + デバイス適応 → `ai_engine_config.cpp` (~720行)
  - OCR関連 → `ai_engine_ocr.cpp` (~480行)

---

## 7. 制約・リスク

### ビルド制約
- **Windows CMake + MinGW**: 同一クラスのメソッドを複数.cppに分散可能（標準C++）
- **インクリメンタルビルド**: ai_engine_impl.hpp変更時は全分割ファイルが再ビルド
  → impl.hppの変更頻度を下げることが重要（メンバ変数追加は影響大）
- **リンク順序**: MinGW/MSVC共に問題なし（static linkなのでオブジェクトファイル順序不問）

### Pimplパターン維持
- ai_engine.hpp（公開）は変更不要 → **ABI互換性維持**
- ai_engine_impl.hpp は src/ 内部でのみ使用 → external includeに露出しない
- unique_ptr<Impl> の生成・破棄は ai_engine.cpp に残す

### テスト
- 24テストスイート全通過が必須
- 分割はリファクタリングのみ（機能変更なし）なので論理的にはテスト影響なし
- 各Phase後にビルド+テスト確認

### 注意点
- **processFrame()の結合度が最も高い** — 全メンバ変数を参照するため、
  ai_engine_impl.hppにすべてのメンバを宣言する必要がある
- **anonymous namespace内のヘルパー関数**はファイル間で共有できないため、
  必要に応じて名前付きnamespace (detail::) に変更する
- **#ifdef USE_AI / MIRAGE_OCR_ENABLED** の条件コンパイルブロックが
  ファイルをまたぐ場合、各.cppでも同じ#ifdefが必要
