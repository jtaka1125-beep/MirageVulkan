// =============================================================================
// MirageSystem - Event Bus
// =============================================================================
// Thread-safe, type-erased publish/subscribe event system.
// Decouples receivers, GUI, and command senders via events.
// Usage:
//   auto sub = mirage::bus().subscribe<FrameReadyEvent>([](const auto& e) { ... });
//   mirage::bus().publish(FrameReadyEvent{...});
// =============================================================================
#pragma once
#include <functional>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <typeindex>
#include <memory>
#include <string>
#include <cstdint>
#include <atomic>
#include <algorithm>
#include "mirage_log.hpp"

namespace mirage {

// =============================================================================
// Event Types
// =============================================================================

struct Event {
    virtual ~Event() = default;
};

// Device lifecycle
struct DeviceConnectedEvent : Event {
    std::string device_id;
    std::string display_name;
    std::string connection_type; // "usb", "wifi", "slot"
};

struct DeviceDisconnectedEvent : Event {
    std::string device_id;
};

// Frame delivery
struct FrameReadyEvent : Event {
    std::string device_id;
    const uint8_t* rgba_data = nullptr;
    int width = 0;
    int height = 0;
    uint64_t frame_id = 0;
};

// Device status
struct DeviceStatusEvent : Event {
    std::string device_id;
    int status = 0; // maps to DeviceStatus enum
    float fps = 0;
    float latency_ms = 0;
    float bandwidth_mbps = 0;
};

// コマンドソース種別（AI自動/ユーザ手動/マクロ）
enum class CommandSource { AI, USER, MACRO };

// Commands (GUI/AI/Macro -> backend)
struct TapCommandEvent : Event {
    std::string device_id;
    int x = 0, y = 0;
    CommandSource source = CommandSource::USER;
};

struct SwipeCommandEvent : Event {
    std::string device_id;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int duration_ms = 300;
    CommandSource source = CommandSource::USER;
};

struct KeyCommandEvent : Event {
    std::string device_id;
    int keycode = 0;
    CommandSource source = CommandSource::USER;
};

// Learning mode (GUI -> AI)
struct LearningStartEvent : Event {
    std::string device_id;
    std::string name_stem;         // テンプレート名のベース（例: "home_button"）
    int roi_x = 0, roi_y = 0;     // ROI左上座標（フレーム座標系 px）
    int roi_w = 0, roi_h = 0;     // ROIサイズ（px）
};

struct LearningCaptureEvent : Event {
    bool ok = false;
    std::string error;
    std::string device_id;
    std::string name_stem;
    int template_id = -1;
    int w = 0, h = 0;
    std::string saved_file_rel;    // manifest相対パス
};

// AI テンプレートマッチング結果
struct MatchResultEvent : Event {
    std::string device_id;
    struct Match {
        std::string template_name;
        int x = 0, y = 0;
        float score = 0.0f;
        int template_id = -1;
        // テンプレートサイズ情報
        int template_width = 0;
        int template_height = 0;
    };
    std::vector<Match> matches;
    uint64_t frame_id = 0;
    double process_time_ms = 0.0;
};

// OCR テキスト認識結果（AIEngine → GUI/ログ用）
struct OcrMatchResult : Event {
    std::string device_id;
    std::string text;          // 認識テキスト
    int x = 0, y = 0;         // テキスト中心座標
    float confidence = 0.0f;   // 信頼度 (0-100)
};

// AI 状態遷移イベント（VisionDecisionEngine → GUI/ログ用）
struct StateChangeEvent : Event {
    std::string device_id;
    int old_state = 0;         // VisionState enum値
    int new_state = 0;         // VisionState enum値
    std::string template_id;   // 関連テンプレートID（空の場合あり）
    int64_t timestamp = 0;     // steady_clock epoch ms
};

// System
struct ShutdownEvent : Event {};

struct LogEvent : Event {
    int level = 1;
    std::string message;
    std::string source;
};

// =============================================================================
// SubscriptionHandle - RAII unsubscribe
// =============================================================================

class SubscriptionHandle {
public:
    SubscriptionHandle() = default;
    explicit SubscriptionHandle(std::function<void()> unsub) : unsub_(std::move(unsub)) {}
    ~SubscriptionHandle() { if (unsub_) unsub_(); }

    SubscriptionHandle(SubscriptionHandle&& o) noexcept : unsub_(std::move(o.unsub_)) { o.unsub_ = nullptr; }
    SubscriptionHandle& operator=(SubscriptionHandle&& o) noexcept {
        if (unsub_) unsub_();
        unsub_ = std::move(o.unsub_);
        o.unsub_ = nullptr;
        return *this;
    }
    SubscriptionHandle(const SubscriptionHandle&) = delete;
    SubscriptionHandle& operator=(const SubscriptionHandle&) = delete;

    void release() { unsub_ = nullptr; } // detach: subscription lives forever

private:
    std::function<void()> unsub_;
};

// =============================================================================
// EventBus - Thread-safe publish/subscribe
// =============================================================================

class EventBus {
public:
    using HandlerId = uint64_t;

    template<typename T>
    SubscriptionHandle subscribe(std::function<void(const T&)> handler) {
        static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

        std::lock_guard<std::mutex> lock(mutex_);
        auto id = next_id_++;
        auto key = std::type_index(typeid(T));

        handlers_[key].push_back({id, [handler](const Event& e) {
            handler(static_cast<const T&>(e));
        }});

        MLOG_DEBUG("eventbus", "Subscribed handler %llu for %s",
                   (unsigned long long)id, typeid(T).name());

        return SubscriptionHandle([this, key, id]() {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = handlers_.find(key);
            if (it != handlers_.end()) {
                auto& vec = it->second;
                vec.erase(std::remove_if(vec.begin(), vec.end(),
                    [id](const HandlerEntry& h) { return h.id == id; }), vec.end());
            }
        });
    }

    template<typename T>
    void publish(const T& event) {
        static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");

        std::vector<HandlerEntry> snapshot;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto key = std::type_index(typeid(T));
            auto it = handlers_.find(key);
            if (it != handlers_.end()) {
                snapshot = it->second;
            }
        }

        for (auto& entry : snapshot) {
            try {
                entry.fn(event);
            } catch (const std::exception& e) {
                MLOG_ERROR("eventbus", "Handler %llu threw: %s",
                           (unsigned long long)entry.id, e.what());
            }
        }
    }

    template<typename T>
    bool has_subscribers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::type_index(typeid(T));
        auto it = handlers_.find(key);
        return it != handlers_.end() && !it->second.empty();
    }

private:
    struct HandlerEntry {
        HandlerId id;
        std::function<void(const Event&)> fn;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::type_index, std::vector<HandlerEntry>> handlers_;
    HandlerId next_id_ = 1;
};

// Global event bus singleton
inline EventBus& bus() {
    static EventBus instance;
    return instance;
}

} // namespace mirage
