#pragma once
// =============================================================================
// UiFinder - マルチ戦略UI要素検索
// MirageComplete/src/ui_finder から移行 (EventBus/Result統合)
// =============================================================================
// 検索順序（AUTOモード）:
//   1. resource-id（uiautomator XML経由、最速）
//   2. テキスト内容（uiautomator XML経由）
//   3. OCR（FrameAnalyzer経由、要MIRAGE_OCR_ENABLED）
//   4. 座標テーブル（デバイス固有フォールバック）
// =============================================================================

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>
#include <cstdint>

#include "../result.hpp"
#include "../event_bus.hpp"

namespace mirage::ai {

// =========================================================================
// UI要素検索結果
// =========================================================================

struct UiElement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::string resource_id;
    std::string text;
    std::string class_name;
    bool clickable = false;
    bool enabled = true;

    // タップ用の中心座標
    int center_x() const { return x + width / 2; }
    int center_y() const { return y + height / 2; }
};

// =========================================================================
// 検索戦略
// =========================================================================

enum class SearchStrategy {
    RESOURCE_ID,      // resource-idで検索（最速）
    TEXT,             // テキスト内容で検索
    OCR,              // OCRテキスト認識で検索
    COORDINATE_TABLE, // 事前定義座標テーブル
    AUTO              // 全戦略を順に試行
};

// =========================================================================
// 座標テーブルエントリ（フォールバック用）
// =========================================================================

struct CoordinateEntry {
    std::string key;          // 識別子（例: "accessibility_switch"）
    std::string device_model; // デバイスモデル（例: "Npad X1"）
    int x = 0;
    int y = 0;
    std::string description;
};

// =========================================================================
// EventBus連携イベント
// =========================================================================

// UI要素検索リクエスト（外部モジュール → UiFinder）
struct UiFindRequestEvent : mirage::Event {
    std::string device_id;
    std::string identifier;
    SearchStrategy strategy = SearchStrategy::AUTO;
    int timeout_ms = 5000;
    uint64_t request_id = 0; // レスポンス照合用
};

// UI要素検索結果（UiFinder → 外部モジュール）
struct UiFindResultEvent : mirage::Event {
    uint64_t request_id = 0;
    bool found = false;
    UiElement element;
    std::string error;
};

// =========================================================================
// UiFinder - マルチ戦略UI要素検索
// =========================================================================

class UiFinder {
public:
    using AdbExecutor = std::function<std::string(const std::string& cmd)>;

    UiFinder();
    ~UiFinder();

    // コピー禁止
    UiFinder(const UiFinder&) = delete;
    UiFinder& operator=(const UiFinder&) = delete;

    // --- 設定 ---

    /// ADBコマンド実行関数を設定（"adb <cmd>"を実行してstdoutを返す）
    void set_adb_executor(AdbExecutor executor);

    /// デバイスモデルを設定（座標テーブル照合用）
    void set_device_model(const std::string& model);

    /// デバイスIDを設定（OCR検索用）
    void set_device_id(const std::string& id);

    /// EventBusのサブスクリプションを開始
    void subscribe_events();

    // --- 検索 ---

    /// UI要素を検索（メイン検索API）
    Result<UiElement> find(
        const std::string& identifier,
        SearchStrategy strategy = SearchStrategy::AUTO,
        int timeout_ms = 5000
    );

    /// resource-idで検索
    Result<UiElement> find_by_resource_id(const std::string& resource_id);

    /// テキスト内容で検索
    Result<UiElement> find_by_text(const std::string& text, bool partial_match = false);

    /// OCRで検索（要MIRAGE_OCR_ENABLED）
    Result<UiElement> find_by_ocr(const std::string& text, const std::string& device_id);

    /// 座標テーブルから検索
    Result<UiElement> find_from_table(const std::string& key);

    // --- 座標テーブル管理 ---

    void add_coordinate_entry(const CoordinateEntry& entry);
    Result<void> load_coordinate_table(const std::string& json_path);
    Result<void> save_coordinate_table(const std::string& json_path);

    // --- uiautomator ---

    /// UIヒエラルキーXMLダンプを取得
    Result<std::string> dump_ui_hierarchy();

private:
    mutable std::mutex mutex_;
    AdbExecutor adb_executor_;
    std::vector<CoordinateEntry> coordinate_table_;
    std::string device_model_;
    std::string device_id_;

    // EventBusサブスクリプション（RAII）
    std::vector<mirage::SubscriptionHandle> subscriptions_;

    // --- 内部ヘルパー ---

    // uiautomator XMLからUI要素リストをパース
    std::vector<UiElement> parse_ui_dump(const std::string& xml);

    // bounds文字列 "[x1,y1][x2,y2]" をパース
    static bool parse_bounds(const std::string& bounds, int& x, int& y, int& w, int& h);

    // EventBusリクエストハンドラ
    void on_find_request(const UiFindRequestEvent& event);
};

} // namespace mirage::ai
