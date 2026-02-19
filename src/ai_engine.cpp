// =============================================================================
// MirageVulkan - AI Engine Implementation
// =============================================================================
// Vulkan一本化。VulkanTemplateMatcher + FrameAnalyzer(OCR)統合。
// EventBus連携: FrameReadyEvent → processFrame → AIActionEvent発火。
// =============================================================================

#include "ai_engine.hpp"

#ifdef USE_AI

#include "vulkan/vulkan_context.hpp"
#include "vulkan_compute_processor.hpp"
#include "vulkan_template_matcher.hpp"
#include "ai/template_store.hpp"
#include "ai/template_autoscan.hpp"
#include "ai/vision_decision_engine.hpp"
#include "event_bus.hpp"
#include "mirage_log.hpp"
#include "stb_image.h"

#ifdef MIRAGE_OCR_ENABLED
#include "frame_analyzer.hpp"
#endif

#include <chrono>
#include <mutex>
#include <filesystem>
#include <algorithm>
#include <unordered_map>

namespace mirage::ai {

// =============================================================================
// アクションマッパー — テンプレートIDからアクション文字列を決定
// =============================================================================

class ActionMapper {
public:
    void addTemplateAction(const std::string& template_id, const std::string& action) {
        actions_[template_id] = action;
    }

    // テンプレート名からアクションを取得（未登録なら "tap:<name>"）
    std::string getAction(const std::string& template_id) const {
        auto it = actions_.find(template_id);
        if (it != actions_.end()) return it->second;
        return "tap:" + template_id;
    }

    // マッチ結果の分類（loading/errorの判定）
    enum class ScreenState { NORMAL, LOADING, ERROR_POPUP };

    ScreenState classifyState(const std::vector<vk::VkMatchResult>& matches,
                              const std::unordered_map<int, std::string>& id_to_name) const {
        for (const auto& m : matches) {
            auto it = id_to_name.find(m.template_id);
            if (it == id_to_name.end()) continue;
            const auto& name = it->second;
            if (name.find("loading") != std::string::npos ||
                name.find("spinner") != std::string::npos) {
                return ScreenState::LOADING;
            }
            if (name.find("error") != std::string::npos ||
                name.find("popup") != std::string::npos) {
                return ScreenState::ERROR_POPUP;
            }
        }
        return ScreenState::NORMAL;
    }

private:
    std::unordered_map<std::string, std::string> actions_;
};

// =============================================================================
// AIEngine::Impl
// =============================================================================

class AIEngine::Impl {
public:
    Impl() = default;
    ~Impl() { shutdown(); }

    mirage::Result<void> initialize(const AIConfig& config,
                                    mirage::vk::VulkanContext* vk_ctx) {
        if (!vk_ctx) {
            return mirage::Err<void>("VulkanContext is required (OpenCL fallback removed)");
        }

        config_ = config;
        vk_ctx_ = vk_ctx;

        // RGBA → Gray プロセッサ
        vk_processor_ = std::make_unique<mirage::vk::VulkanComputeProcessor>();
        if (!vk_processor_->initialize(*vk_ctx, "shaders")) {
            vk_processor_.reset();
            return mirage::Err<void>("VulkanComputeProcessor 初期化失敗");
        }

        // テンプレートマッチャー
        mirage::vk::VkMatcherConfig mc;
        mc.default_threshold = config.default_threshold;
        mc.enable_multi_scale = config.enable_multi_scale;

        vk_matcher_ = std::make_unique<mirage::vk::VulkanTemplateMatcher>();
        auto matcherResult = vk_matcher_->initialize(*vk_ctx, mc, "shaders");
        if (matcherResult.is_err()) {
            auto err = matcherResult.error().message;
            vk_matcher_.reset();
            vk_processor_.reset();
            return mirage::Err<void>(err);
        }

        // アクションマッパー
        action_mapper_ = std::make_unique<ActionMapper>();

        // VisionDecisionEngine（状態遷移マシン）
        VisionDecisionConfig vde_config;
        vde_config.confirm_count = 3;
        vde_config.cooldown_ms = 2000;
        vde_config.debounce_window_ms = 500;
        vision_engine_ = std::make_unique<VisionDecisionEngine>(vde_config);

        // EventBus購読
        if (config.subscribe_events) {
            frame_sub_ = mirage::bus().subscribe<mirage::FrameReadyEvent>(
                [this](const mirage::FrameReadyEvent& evt) {
                    onFrameReady(evt);
                });
            MLOG_INFO("ai", "EventBus FrameReadyEvent 購読開始");
        }

        initialized_ = true;
        MLOG_INFO("ai", "AI Engine 初期化完了 (Vulkan Compute)");
        return mirage::Ok();
    }

    void shutdown() {
        if (!initialized_) return;
        MLOG_INFO("ai", "AI Engine シャットダウン");

        // EventBus購読解除（SubscriptionHandle RAIIで自動解除）
        frame_sub_ = mirage::SubscriptionHandle();

        vk_matcher_.reset();
        vk_processor_.reset();
        action_mapper_.reset();
        vision_engine_.reset();

        initialized_ = false;
    }

    // =========================================================================
    // TemplateStore接続
    // =========================================================================

    void setTemplateStore(TemplateStore* store) {
        template_store_ = store;
        MLOG_INFO("ai", "TemplateStore接続: %s", store ? "有効" : "null");
    }

    // =========================================================================
    // FrameAnalyzer(OCR)接続
    // =========================================================================

    void setFrameAnalyzer([[maybe_unused]] mirage::FrameAnalyzer* analyzer) {
#ifdef MIRAGE_OCR_ENABLED
        frame_analyzer_ = analyzer;
        MLOG_INFO("ai", "FrameAnalyzer接続: %s", analyzer ? "有効" : "null");
#else
        MLOG_WARN("ai", "OCR未コンパイル (MIRAGE_OCR_ENABLED未定義) — FrameAnalyzer無視");
#endif
    }

    // =========================================================================
    // テンプレート管理
    // =========================================================================

    mirage::Result<void> loadTemplatesFromDir(const std::string& dir) {
        namespace fs = std::filesystem;
        if (!fs::exists(dir)) {
            return mirage::Err<void>("ディレクトリが見つかりません: " + dir);
        }
        if (!vk_matcher_) {
            return mirage::Err<void>("VulkanTemplateMatcher未初期化");
        }

        // autoscan でマニフェスト同期
        AutoScanConfig scan_cfg;
        scan_cfg.templates_dir = dir;
        scan_cfg.manifest_path = dir + "/manifest.json";
        TemplateManifest manifest;
        auto scan_result = syncTemplateManifest(scan_cfg, manifest);
        if (!scan_result.ok) {
            return mirage::Err<void>("オートスキャン失敗: " + scan_result.error);
        }

        MLOG_INFO("ai", "オートスキャン完了: 追加=%d 更新=%d 保持=%d 削除=%d",
                  scan_result.added, scan_result.updated,
                  scan_result.kept, scan_result.removed);

        // マニフェストの各エントリを TemplateStore → VulkanTemplateMatcher に登録
        int count = 0;
        for (const auto& entry : manifest.entries) {
            std::string full_path = dir + "/" + entry.file;

            auto addResult = addTemplateFromFile(full_path, entry.name, entry.template_id);
            if (addResult.is_ok()) {
                count++;
            } else {
                MLOG_WARN("ai", "テンプレート読み込みスキップ: %s (%s)",
                          entry.name.c_str(), addResult.error().message.c_str());
            }
        }

        stats_.templates_loaded = count;
        MLOG_INFO("ai", "テンプレート %d 個読み込み完了 (dir=%s)", count, dir.c_str());
        if (count > 0) return mirage::Ok();
        return mirage::Err<void>("テンプレートが1つも読み込めませんでした");
    }

    bool addTemplate(const std::string& name, const uint8_t* rgba, int w, int h) {
        if (!vk_matcher_) return false;

        // RGBA→GrayをGPUで変換
        auto* gray_gpu = vk_processor_->rgbaToGrayGpu(rgba, w, h);
        if (!gray_gpu) {
            MLOG_WARN("ai", "RGBA→Gray変換失敗: %s", name.c_str());
            return false;
        }

        // GPU上のGrayデータをCPUに読み戻してテンプレート登録
        // VulkanTemplateMatcher::addTemplate はCPU grayデータを受け取る
        std::vector<uint8_t> gray_cpu(w * h);
        if (!vk_processor_->rgbaToGray(rgba, w, h, gray_cpu.data())) {
            MLOG_WARN("ai", "RGBA→Gray(CPU)変換失敗: %s", name.c_str());
            return false;
        }

        auto addResult = vk_matcher_->addTemplate(name, gray_cpu.data(), w, h, "");
        if (addResult.is_err()) {
            MLOG_WARN("ai", "テンプレート追加失敗: %s (%s)", name.c_str(), addResult.error().message.c_str());
            return false;
        }
        int id = addResult.value();

        // ID→名前マッピング
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            id_to_name_[id] = name;
        }
        action_mapper_->addTemplateAction(name, "tap:" + name);
        stats_.templates_loaded++;
        return true;
    }

    void clearTemplates() {
        if (vk_matcher_) vk_matcher_->clearAll();
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            id_to_name_.clear();
        }
        stats_.templates_loaded = 0;
    }

    // =========================================================================
    // フレーム処理
    // =========================================================================

    AIAction processFrame(int slot, const uint8_t* rgba, int width, int height,
                          bool can_send) {
        auto start = std::chrono::high_resolution_clock::now();

        AIAction action;
        action.type = AIAction::Type::NONE;

        if (!initialized_) { action.reason = "未初期化"; return action; }
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
            action.reason = "不正なフレームサイズ"; return action;
        }
        if (!rgba) { action.reason = "nullフレーム"; return action; }

        // RGBA → Gray (GPU)
        auto* gray_gpu = vk_processor_->rgbaToGrayGpu(rgba, width, height);
        if (!gray_gpu) {
            action.reason = "RGBA→Gray変換失敗";
            MLOG_WARN("ai", "Vulkan RGBA→Gray失敗");
            return action;
        }

        // テンプレートマッチング (GPU)
        auto matchResult = vk_matcher_->matchGpu(gray_gpu, width, height);
        if (matchResult.is_err()) {
            action.reason = "マッチング失敗: " + matchResult.error().message;
            MLOG_WARN("ai", "Vulkan match失敗: %s", matchResult.error().message.c_str());
            return action;
        }
        auto vk_results = std::move(matchResult).value();

        // マッチ結果をOverlay用にキャッシュ
        cacheMatches(vk_results);

        // VisionDecisionEngine経由で状態遷移判断
        std::string device_id = "slot_" + std::to_string(slot);
        if (vision_engine_) {
            // VkMatchResult → VisionMatch変換
            std::unordered_map<int, std::string> names_snap;
            {
                std::lock_guard<std::mutex> lock(names_mutex_);
                names_snap = id_to_name_;
            }
            std::vector<VisionMatch> vision_matches;
            vision_matches.reserve(vk_results.size());
            for (const auto& r : vk_results) {
                VisionMatch vm;
                auto it = names_snap.find(r.template_id);
                vm.template_id = (it != names_snap.end()) ? it->second
                    : "tpl_" + std::to_string(r.template_id);
                vm.x = r.x;
                vm.y = r.y;
                vm.score = r.score;
                // errorグループ判定: テンプレート名に"error"/"popup"を含む
                vm.is_error_group = (vm.template_id.find("error") != std::string::npos ||
                                     vm.template_id.find("popup") != std::string::npos);
                vision_matches.push_back(std::move(vm));
            }

            auto decision = vision_engine_->update(device_id, vision_matches);

            if (decision.should_act && can_send) {
                // VisionDecisionEngineが確定 → decideActionを実行
                action = decideAction(slot, vk_results, can_send);
                // アクション実行完了通知 → COOLDOWN遷移
                vision_engine_->notifyActionExecuted(device_id);
            } else if (!decision.should_act) {
                // 未確定 or COOLDOWN中 → WAIT
                if (!vk_results.empty()) {
                    action.type = AIAction::Type::WAIT;
                    action.reason = "VisionEngine: " +
                        std::string(visionStateToString(decision.state));
                } else {
                    // マッチなし
                    action.type = AIAction::Type::WAIT;
                    action.reason = "マッチなし";
                    idle_frames_++;
                    stats_.idle_frames = idle_frames_;
                }
            }
        } else {
            // VisionDecisionEngine未初期化時はフォールバック
            action = decideAction(slot, vk_results, can_send);
        }

        // 統計更新
        auto end = std::chrono::high_resolution_clock::now();
        double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        stats_.frames_processed++;
        stats_.avg_process_time_ms =
            (stats_.avg_process_time_ms * (stats_.frames_processed - 1) + elapsed)
            / stats_.frames_processed;

        // MatchResultEvent をEventBus発行（マッチ結果があれば）
        if (!vk_results.empty()) {
            mirage::MatchResultEvent evt;
            evt.device_id = device_id;
            evt.frame_id = stats_.frames_processed;
            evt.process_time_ms = elapsed;

            std::unordered_map<int, std::string> names_for_evt;
            {
                std::lock_guard<std::mutex> lock(names_mutex_);
                names_for_evt = id_to_name_;
            }
            evt.matches.reserve(vk_results.size());
            for (const auto& r : vk_results) {
                mirage::MatchResultEvent::Match m;
                auto it = names_for_evt.find(r.template_id);
                m.template_name = (it != names_for_evt.end()) ? it->second
                    : "tpl_" + std::to_string(r.template_id);
                m.x = r.x;
                m.y = r.y;
                m.score = r.score;
                m.template_id = r.template_id;
                m.template_width  = r.template_width;
                m.template_height = r.template_height;
                evt.matches.push_back(std::move(m));
            }
            mirage::bus().publish(evt);
        }

        return action;
    }

    std::vector<AIEngine::MatchRect> getLastMatches() const {
        std::lock_guard<std::mutex> lock(matches_mutex_);
        return last_matches_;
    }

    AIStats getStats() const { return stats_; }

    void resetStats() {
        int tpl = stats_.templates_loaded;
        stats_ = AIStats();
        stats_.templates_loaded = tpl;
    }

    void reset() {
        idle_frames_ = 0;
        stats_.idle_frames = 0;
        if (vision_engine_) vision_engine_->resetAll();
    }

    // VisionDecisionEngine GUI用アクセサ
    int getDeviceVisionState(const std::string& device_id) const {
        if (!vision_engine_) return 0; // IDLE
        return static_cast<int>(vision_engine_->getDeviceState(device_id));
    }

    void resetDeviceVision(const std::string& device_id) {
        if (vision_engine_) vision_engine_->resetDevice(device_id);
    }

    void resetAllVision() {
        if (vision_engine_) vision_engine_->resetAll();
    }

    AIEngine::VDEConfig getVDEConfig() const {
        AIEngine::VDEConfig result;
        if (vision_engine_) {
            auto& c = vision_engine_->config();
            result.confirm_count = c.confirm_count;
            result.cooldown_ms = c.cooldown_ms;
            result.debounce_window_ms = c.debounce_window_ms;
            result.error_recovery_ms = c.error_recovery_ms;
        }
        return result;
    }

    std::vector<std::pair<std::string, int>> getAllDeviceVisionStates() const {
        std::vector<std::pair<std::string, int>> result;
        if (!vision_engine_) return result;
        // device_states_はprivate — getDeviceStateを各既知デバイスに対して呼ぶ
        // GUIからはslot_0〜slot_9の固定range
        for (int i = 0; i < 10; ++i) {
            std::string dev = "slot_" + std::to_string(i);
            auto state = vision_engine_->getDeviceState(dev);
            if (state != VisionState::IDLE) {
                result.emplace_back(dev, static_cast<int>(state));
            }
        }
        return result;
    }

private:
    // =========================================================================
    // EventBus FrameReady ハンドラ
    // =========================================================================

    void onFrameReady(const mirage::FrameReadyEvent& evt) {
        if (!initialized_) return;

        // device_id から slot番号を推定（"slot_N" 形式）
        int slot = 0;
        if (evt.device_id.substr(0, 5) == "slot_") {
            try { slot = std::stoi(evt.device_id.substr(5)); } catch (...) {}
        }

        // processFrameは外部（gui_threads.cpp）から直接呼ばれるため、
        // EventBus経由では重複呼び出しを避ける。
        // EventBus購読は将来のパイプライン統合用に準備のみ。
    }

    // =========================================================================
    // アクション決定ロジック
    // =========================================================================

    AIAction decideAction(int slot,
                          const std::vector<mirage::vk::VkMatchResult>& results,
                          bool can_send) {
        AIAction action;

        std::unordered_map<int, std::string> names;
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            names = id_to_name_;
        }

        if (results.empty()) {
            // テンプレートマッチ失敗 → OCRフォールバック
#ifdef MIRAGE_OCR_ENABLED
            if (frame_analyzer_ && frame_analyzer_->isInitialized()) {
                std::string device_id = "slot_" + std::to_string(slot);
                auto ocr_action = tryOcrFallback(slot, device_id, can_send);
                if (ocr_action.type != AIAction::Type::NONE) {
                    return ocr_action;
                }
            }
#endif
            idle_frames_++;
            stats_.idle_frames = idle_frames_;
            action.type = AIAction::Type::WAIT;
            action.reason = "マッチなし (idle=" + std::to_string(idle_frames_) + ")";
            return action;
        }

        // 画面状態判定
        auto state = action_mapper_->classifyState(results, names);
        if (state == ActionMapper::ScreenState::LOADING) {
            action.type = AIAction::Type::WAIT;
            action.reason = "ローディング検出 — 待機";
            return action;
        }

        // ベストマッチを選択
        const auto& best = *std::max_element(results.begin(), results.end(),
            [](const auto& a, const auto& b) { return a.score < b.score; });

        // 送信可能チェック
        if (!can_send) {
            action.type = AIAction::Type::WAIT;
            action.reason = "送信不可 — 待機";
            return action;
        }

        // テンプレート名取得
        std::string tpl_name;
        auto it = names.find(best.template_id);
        if (it != names.end()) tpl_name = it->second;
        else tpl_name = "tpl_" + std::to_string(best.template_id);

        // アクション文字列を取得
        std::string action_str = action_mapper_->getAction(tpl_name);

        if (action_str.substr(0, 4) == "tap:") {
            action.type = AIAction::Type::TAP;
            action.template_id = action_str.substr(4);
            // center座標でタップ（left-top + size/2）
            action.x = best.center_x;
            action.y = best.center_y;
            action.confidence = best.score;
        } else if (action_str == "back") {
            action.type = AIAction::Type::BACK;
        }

        action.reason = "match=" + tpl_name + " score=" +
                        std::to_string(best.score);
        idle_frames_ = 0;
        stats_.idle_frames = 0;
        stats_.actions_executed++;

        // EventBus経由でコマンド発行（AI→CommandSender パイプライン）
        std::string device_id = "slot_" + std::to_string(slot);
        if (action.type == AIAction::Type::TAP) {
            mirage::TapCommandEvent evt;
            evt.device_id = device_id;
            evt.x = action.x;
            evt.y = action.y;
            evt.source = mirage::CommandSource::AI;
            mirage::bus().publish(evt);
            MLOG_DEBUG("ai", "EventBus TapCommand発行: device=%s (%d,%d) tpl=%s",
                       device_id.c_str(), action.x, action.y, tpl_name.c_str());
        } else if (action.type == AIAction::Type::BACK) {
            mirage::KeyCommandEvent evt;
            evt.device_id = device_id;
            evt.keycode = 4;  // KEYCODE_BACK
            evt.source = mirage::CommandSource::AI;
            mirage::bus().publish(evt);
            MLOG_DEBUG("ai", "EventBus KeyCommand(BACK)発行: device=%s", device_id.c_str());
        }

        return action;
    }

    // =========================================================================
    // OCRフォールバック — テンプレートマッチ失敗時にOCRでアクション決定
    // =========================================================================

#ifdef MIRAGE_OCR_ENABLED
    AIAction tryOcrFallback(int slot, const std::string& device_id, bool can_send) {
        AIAction action;
        action.type = AIAction::Type::NONE;

        if (!can_send) {
            action.type = AIAction::Type::WAIT;
            action.reason = "OCRフォールバック: 送信不可 — 待機";
            return action;
        }

        // 登録済みテキストキーワードを順に検索
        auto keywords = action_mapper_->getTextKeywords();
        if (keywords.empty()) {
            return action; // テキストアクション未登録 → NONE
        }

        for (const auto& keyword : keywords) {
            int cx = 0, cy = 0;
            if (frame_analyzer_->getTextCenter(device_id, keyword, cx, cy)) {
                std::string action_str = action_mapper_->getTextAction(keyword);

                if (action_str.substr(0, 4) == "tap:") {
                    action.type = AIAction::Type::TAP;
                    action.template_id = action_str.substr(4);
                    action.x = cx;
                    action.y = cy;
                } else if (action_str == "back") {
                    action.type = AIAction::Type::BACK;
                }

                action.reason = "OCR match=\"" + keyword + "\" action=" + action_str;
                idle_frames_ = 0;
                stats_.idle_frames = 0;
                stats_.actions_executed++;

                // OcrMatchResult イベント発行
                mirage::OcrMatchResult ocr_evt;
                ocr_evt.device_id = device_id;
                ocr_evt.text = keyword;
                ocr_evt.x = cx;
                ocr_evt.y = cy;
                // confidence は getTextCenter では取得不可なので 0 (将来拡張用)
                mirage::bus().publish(ocr_evt);

                // コマンドイベント発行
                if (action.type == AIAction::Type::TAP) {
                    mirage::TapCommandEvent evt;
                    evt.device_id = device_id;
                    evt.x = action.x;
                    evt.y = action.y;
                    evt.source = mirage::CommandSource::AI;
                    mirage::bus().publish(evt);
                    MLOG_INFO("ai", "OCRフォールバック TapCommand: device=%s (%d,%d) text=\"%s\"",
                              device_id.c_str(), cx, cy, keyword.c_str());
                } else if (action.type == AIAction::Type::BACK) {
                    mirage::KeyCommandEvent evt;
                    evt.device_id = device_id;
                    evt.keycode = 4;  // KEYCODE_BACK
                    evt.source = mirage::CommandSource::AI;
                    mirage::bus().publish(evt);
                    MLOG_INFO("ai", "OCRフォールバック KeyCommand(BACK): device=%s text=\"%s\"",
                              device_id.c_str(), keyword.c_str());
                }

                return action;
            }
        }

        return action; // キーワード未検出 → NONE
    }
#endif

    // =========================================================================
    // マッチ結果キャッシュ（オーバーレイ描画用）
    // =========================================================================

    void cacheMatches(const std::vector<mirage::vk::VkMatchResult>& vk_results) {
        std::unordered_map<int, std::string> names;
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            names = id_to_name_;
        }

        std::vector<AIEngine::MatchRect> rects;
        rects.reserve(vk_results.size());
        for (const auto& r : vk_results) {
            AIEngine::MatchRect rect;
            auto it = names.find(r.template_id);
            rect.template_id = (it != names.end()) ? it->second
                : "tpl_" + std::to_string(r.template_id);
            rect.label = rect.template_id;
            rect.x = r.x;
            rect.y = r.y;
            rect.w = r.template_width;
            rect.h = r.template_height;
            rect.center_x = r.center_x;
            rect.center_y = r.center_y;
            rect.score = r.score;
            rects.push_back(rect);
        }

        std::lock_guard<std::mutex> lock(matches_mutex_);
        last_matches_ = std::move(rects);
    }

    // =========================================================================
    // ファイルからテンプレート読み込み（TemplateStore経由 stb_image使用）
    // =========================================================================

    mirage::Result<void> addTemplateFromFile(const std::string& path, const std::string& name,
                                              int template_id) {
        if (!vk_matcher_) {
            return mirage::Err<void>("VulkanTemplateMatcher未初期化");
        }

        // TemplateStore経由: stb_imageでデコード → Gray8変換 → 内部保持
        if (template_store_) {
            auto loadResult = template_store_->loadFromFile(template_id, path);
            if (loadResult.is_err()) {
                return mirage::Err<void>("TemplateStore読込失敗: " + loadResult.error().message);
            }

            // TemplateStoreから取得してMatcherに登録
            auto* th = template_store_->get(template_id);
            if (!th || th->gray_data.empty()) {
                return mirage::Err<void>("Store内データが空");
            }

            auto addResult = vk_matcher_->addTemplate(
                name, th->gray_data.data(), th->w, th->h, "");
            if (addResult.is_err()) {
                return mirage::Err<void>("Matcher登録失敗: " + addResult.error().message);
            }
            int matcher_id = addResult.value();

            // matcher_idをTemplateHandleに記録（将来の参照用）
            // NOTE: TemplateHandle::matcher_idは直接書き込み不可（const get）
            // id_to_name_マッピングで管理

            {
                std::lock_guard<std::mutex> lock(names_mutex_);
                id_to_name_[matcher_id] = name;
            }
            action_mapper_->addTemplateAction(name, "tap:" + name);

            MLOG_DEBUG("ai", "テンプレート登録: name=%s store_id=%d matcher_id=%d %s",
                       name.c_str(), template_id, matcher_id, path.c_str());
            return mirage::Ok();
        }

        // TemplateStore未接続時: 直接stb_imageでデコード（フォールバック）
        int w = 0, h = 0, channels = 0;
        unsigned char* img = stbi_load(path.c_str(), &w, &h, &channels, 1);
        if (!img) {
            // RGBA → Gray フォールバック
            img = stbi_load(path.c_str(), &w, &h, &channels, 4);
            if (!img) {
                return mirage::Err<void>("stbi_load失敗: " + path);
            }
            // RGBA → Gray8 変換
            std::vector<uint8_t> gray((size_t)w * h);
            for (int i = 0; i < w * h; i++) {
                int y = (77 * img[i * 4 + 0] + 150 * img[i * 4 + 1]
                         + 29 * img[i * 4 + 2] + 128) >> 8;
                gray[i] = (uint8_t)std::clamp(y, 0, 255);
            }
            stbi_image_free(img);

            auto addResult = vk_matcher_->addTemplate(name, gray.data(), w, h, "");
            if (addResult.is_err()) {
                return mirage::Err<void>("Matcher登録失敗: " + addResult.error().message);
            }
            int matcher_id = addResult.value();
            {
                std::lock_guard<std::mutex> lock(names_mutex_);
                id_to_name_[matcher_id] = name;
            }
            action_mapper_->addTemplateAction(name, "tap:" + name);
            return mirage::Ok();
        }

        // Gray8直接読込成功
        auto addResult = vk_matcher_->addTemplate(name, img, w, h, "");
        stbi_image_free(img);
        if (addResult.is_err()) {
            return mirage::Err<void>("Matcher登録失敗: " + addResult.error().message);
        }
        int matcher_id = addResult.value();
        {
            std::lock_guard<std::mutex> lock(names_mutex_);
            id_to_name_[matcher_id] = name;
        }
        action_mapper_->addTemplateAction(name, "tap:" + name);
        return mirage::Ok();
    }

    // =========================================================================
    // メンバ変数
    // =========================================================================

    AIConfig config_;
    bool initialized_ = false;

    // TemplateStore（外部所有、non-owning）
    TemplateStore* template_store_ = nullptr;

    // FrameAnalyzer(OCR)（外部所有、non-owning）
#ifdef MIRAGE_OCR_ENABLED
    mirage::FrameAnalyzer* frame_analyzer_ = nullptr;
#endif

    // Vulkanバックエンド
    mirage::vk::VulkanContext* vk_ctx_ = nullptr;
    std::unique_ptr<mirage::vk::VulkanComputeProcessor> vk_processor_;
    std::unique_ptr<mirage::vk::VulkanTemplateMatcher> vk_matcher_;

    // アクション決定
    std::unique_ptr<ActionMapper> action_mapper_;
    std::unique_ptr<VisionDecisionEngine> vision_engine_;
    int idle_frames_ = 0;

    // テンプレートID→名前マッピング
    std::mutex names_mutex_;
    std::unordered_map<int, std::string> id_to_name_;

    // オーバーレイ用マッチ結果キャッシュ
    mutable std::mutex matches_mutex_;
    std::vector<AIEngine::MatchRect> last_matches_;

    // EventBus購読ハンドル
    mirage::SubscriptionHandle frame_sub_;

    // 統計
    AIStats stats_;
};

// =============================================================================
// AIEngine メソッド委譲
// =============================================================================

AIEngine::AIEngine() : impl_(std::make_unique<Impl>()) {}
AIEngine::~AIEngine() = default;

mirage::Result<void> AIEngine::initialize(const AIConfig& config,
                                          mirage::vk::VulkanContext* vk_ctx) {
    return impl_->initialize(config, vk_ctx);
}

void AIEngine::shutdown() { impl_->shutdown(); }

void AIEngine::setTemplateStore(TemplateStore* store) {
    impl_->setTemplateStore(store);
}

void AIEngine::setFrameAnalyzer(mirage::FrameAnalyzer* analyzer) {
    impl_->setFrameAnalyzer(analyzer);
}

mirage::Result<void> AIEngine::loadTemplatesFromDir(const std::string& dir) {
    return impl_->loadTemplatesFromDir(dir);
}

bool AIEngine::addTemplate(const std::string& name, const uint8_t* rgba, int w, int h) {
    return impl_->addTemplate(name, rgba, w, h);
}

void AIEngine::clearTemplates() { impl_->clearTemplates(); }

AIAction AIEngine::processFrame(int slot, const uint8_t* rgba, int width, int height) {
    if (!enabled_) {
        AIAction action;
        action.type = AIAction::Type::NONE;
        action.reason = "AI無効";
        return action;
    }

    bool can_send = can_send_callback_ ? can_send_callback_() : true;
    auto action = impl_->processFrame(slot, rgba, width, height, can_send);

    // アクション実行コールバック呼び出し
    if (action.type != AIAction::Type::NONE &&
        action.type != AIAction::Type::WAIT &&
        action_callback_) {
        action_callback_(slot, action);
    }

    return action;
}

std::vector<AIEngine::MatchRect> AIEngine::getLastMatches() const {
    if (!impl_) return {};
    return impl_->getLastMatches();
}

AIStats AIEngine::getStats() const { return impl_->getStats(); }
void AIEngine::resetStats() { impl_->resetStats(); }
void AIEngine::reset() { impl_->reset(); }

int AIEngine::getDeviceVisionState(const std::string& device_id) const {
    if (!impl_) return 0;
    return impl_->getDeviceVisionState(device_id);
}

void AIEngine::resetDeviceVision(const std::string& device_id) {
    if (impl_) impl_->resetDeviceVision(device_id);
}

void AIEngine::resetAllVision() {
    if (impl_) impl_->resetAllVision();
}

AIEngine::VDEConfig AIEngine::getVDEConfig() const {
    if (!impl_) return {};
    return impl_->getVDEConfig();
}

std::vector<std::pair<std::string, int>> AIEngine::getAllDeviceVisionStates() const {
    if (!impl_) return {};
    return impl_->getAllDeviceVisionStates();
}

} // namespace mirage::ai

#else // !USE_AI

#include "mirage_log.hpp"

namespace mirage::ai {

class AIEngine::Impl {};

AIEngine::AIEngine() {}
AIEngine::~AIEngine() = default;

mirage::Result<void> AIEngine::initialize(const AIConfig&, mirage::vk::VulkanContext*) {
    return mirage::Err<void>("AI未コンパイル (USE_AI未定義)");
}

void AIEngine::shutdown() {}

void AIEngine::setTemplateStore(TemplateStore*) {}
void AIEngine::setFrameAnalyzer(mirage::FrameAnalyzer*) {}

mirage::Result<void> AIEngine::loadTemplatesFromDir(const std::string&) {
    return mirage::Err<void>("AI未コンパイル");
}

bool AIEngine::addTemplate(const std::string&, const uint8_t*, int, int) { return false; }
void AIEngine::clearTemplates() {}

AIAction AIEngine::processFrame(int, const uint8_t*, int, int) {
    AIAction action;
    action.type = AIAction::Type::NONE;
    action.reason = "AI未コンパイル";
    return action;
}

std::vector<AIEngine::MatchRect> AIEngine::getLastMatches() const { return {}; }
AIStats AIEngine::getStats() const { return AIStats(); }
void AIEngine::resetStats() {}
void AIEngine::reset() {}

int AIEngine::getDeviceVisionState(const std::string&) const { return 0; }
void AIEngine::resetDeviceVision(const std::string&) {}
void AIEngine::resetAllVision() {}
AIEngine::VDEConfig AIEngine::getVDEConfig() const { return {}; }
std::vector<std::pair<std::string, int>> AIEngine::getAllDeviceVisionStates() const { return {}; }

} // namespace mirage::ai

#endif // USE_AI
