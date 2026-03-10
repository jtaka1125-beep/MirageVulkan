// =============================================================================
// CanonicalFrameProvider  v2  (+latency timestamps)
//
// X1SessionManager → SharedFrame → FrameReadyEvent(EventBus) のブリッジ。
// AI/マクロの唯一のフレーム入力源として機能する。
//
// 設計原則:
//   - CanonicalFrame の rgba は zero-copy で SharedFrame に渡す (shared_ptr再利用)
//   - device_id は常に "x1_canonical" — AIEngine側でフィルタリング可能
//   - 座標系は常に width=600, height=1000, origin=(0,0) — 表示倍率は絶対に混入しない
//   - X1SessionManager を内包し、外部は CanonicalFrameProvider だけを触れば良い
//
// タイムスタンプフィールド (SharedFrame):
//   pts_us         = Android capture time (Choreographer, monotonic us, 下位32bit)
//   decode_done_us = PC側: JPEG decode 完了 (steady_clock us)
//   publish_us     = PC側: EventBus publish 直前 (steady_clock us)
//   ★ decode_done_us → publish_us の差 = CanonicalFrame→SharedFrame 変換コスト
//
// 使い方:
//   CanonicalFrameProvider provider;
//   provider.start("192.168.0.3");   // X1のIP
//   // これ以降 AIEngine は EventBus 経由で canonical frames を受信する
// =============================================================================
#pragma once
#include "x1_protocol.hpp"
#include "canonical_frame.hpp"
#include "x1_session_manager.hpp"
#include "../event_bus.hpp"
#include "../mirage_log.hpp"

#include <atomic>
#include <string>
#include <cstdint>
#include <chrono>

namespace mirage::x1 {

// =============================================================================
// 座標系定数 — AI/マクロが参照する唯一の真実
// =============================================================================
struct CanonicalCoordSystem {
    static constexpr int  WIDTH    = CANONICAL_W;   // 600
    static constexpr int  HEIGHT   = CANONICAL_H;   // 1000
    static constexpr int  ORIGIN_X = 0;
    static constexpr int  ORIGIN_Y = 0;
    static constexpr const char* DEVICE_ID = "x1_canonical";

    // 座標検証: AI/マクロが送る座標がCanonical範囲内か確認
    static bool in_bounds(int x, int y) {
        return x >= ORIGIN_X && x < WIDTH &&
               y >= ORIGIN_Y && y < HEIGHT;
    }

    // 正規化 (0.0〜1.0)
    static float norm_x(int x) { return (float)x / WIDTH;  }
    static float norm_y(int y) { return (float)y / HEIGHT; }

    // 正規化→絶対座標 (Canonical基準)
    static int abs_x(float nx) { return (int)(nx * WIDTH);  }
    static int abs_y(float ny) { return (int)(ny * HEIGHT); }
};

// =============================================================================
// CanonicalFrameProvider
// =============================================================================
class CanonicalFrameProvider {
public:
    explicit CanonicalFrameProvider() = default;
    ~CanonicalFrameProvider() { stop(); }

    // コピー/ムーブ禁止 (SessionManagerを内包するため)
    CanonicalFrameProvider(const CanonicalFrameProvider&)            = delete;
    CanonicalFrameProvider& operator=(const CanonicalFrameProvider&) = delete;

    // ── 起動/停止 ──────────────────────────────────────────────────────────────

    bool start(const std::string& device_ip) {
        if (running_.load()) return true;

        device_ip_ = device_ip;
        frame_count_ = 0;

        session_.set_frame_callback(
            [this](CanonicalFrame frame) { on_canonical_frame(std::move(frame)); }
        );

        if (!session_.start(device_ip)) {
            MLOG_ERROR("canonical_provider", "failed to start X1SessionManager for %s",
                       device_ip.c_str());
            return false;
        }

        running_ = true;
        MLOG_INFO("canonical_provider", "started for %s  coord=%dx%d",
                  device_ip.c_str(), CanonicalCoordSystem::WIDTH, CanonicalCoordSystem::HEIGHT);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        session_.stop();
        MLOG_INFO("canonical_provider", "stopped  frames_delivered=%llu",
                  (unsigned long long)frame_count_.load());
    }

    // ── 状態 ───────────────────────────────────────────────────────────────────

    bool is_running()   const { return running_.load(); }
    bool is_connected() const { return session_.is_connected(); }
    uint64_t frame_count() const { return frame_count_.load(); }

    // ── 制御コマンド (X1SessionManager パススルー) ─────────────────────────────

    bool send_idr()              { return session_.send_idr(); }
    bool send_pres_on()          { return session_.send_pres_on(); }
    bool send_pres_off()         { return session_.send_pres_off(); }
    bool send_bitrate(int kbps)  { return session_.send_bitrate(kbps); }

    // ── 座標系アクセサ ─────────────────────────────────────────────────────────

    static constexpr int  coord_width()   { return CanonicalCoordSystem::WIDTH;  }
    static constexpr int  coord_height()  { return CanonicalCoordSystem::HEIGHT; }
    static constexpr int  coord_origin_x(){ return CanonicalCoordSystem::ORIGIN_X; }
    static constexpr int  coord_origin_y(){ return CanonicalCoordSystem::ORIGIN_Y; }
    static const char*    device_id()     { return CanonicalCoordSystem::DEVICE_ID; }

    static bool coord_in_bounds(int x, int y) {
        return CanonicalCoordSystem::in_bounds(x, y);
    }

    // recv/assembler stats (drop diagnostics)
    using RecvStats = X1SessionManager::RecvStats;
    RecvStats get_recv_stats() const { return session_.get_recv_stats(); }

    // alive/dead monitoring ─────────────────────────────────────────────────
    // Returns true if a frame was received within the last `timeout_ms` ms.
    // Use for C-0 gate validation and watchdog loops.
    static constexpr uint64_t DEFAULT_ALIVE_TIMEOUT_MS = 3000; // 3s = ~90 missed frames @ 30fps

    bool is_alive(uint64_t timeout_ms = DEFAULT_ALIVE_TIMEOUT_MS) const {
        uint64_t last = last_frame_us_.load();
        if (last == 0) return false;  // no frame ever received
        uint64_t elapsed_ms = (now_us() - last) / 1000;
        return elapsed_ms < timeout_ms;
    }

    // Returns ms since last frame, or UINT64_MAX if none received yet.
    uint64_t last_frame_age_ms() const {
        uint64_t last = last_frame_us_.load();
        if (last == 0) return UINT64_MAX;
        return (now_us() - last) / 1000;
    }

private:
    X1SessionManager      session_;
    std::string           device_ip_;
    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> last_frame_us_{0};   // last frame received timestamp (steady_clock us)

    static uint64_t now_us() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // CanonicalFrame を SharedFrame に変換して EventBus に発火
    void on_canonical_frame(CanonicalFrame frame) {
        if (!frame.valid()) return;

        // zero-copy: CanonicalFrame の rgba (shared_ptr) をそのまま SharedFrame に渡す
        auto sf = std::make_shared<mirage::SharedFrame>();
        sf->rgba           = frame.rgba;
        sf->width          = (int)frame.width;    // 常に 600
        sf->height         = (int)frame.height;   // 常に 1000
        sf->frame_id       = frame.frame_id;
        sf->pts_us         = frame.pts_us;
        sf->decode_done_us = frame.recv_us;        // PC: JPEG decode 完了時刻
        sf->device_id      = CanonicalCoordSystem::DEVICE_ID;  // "x1_canonical" 固定

        // publish_us: EventBus に渡す直前の PC monotonic 時刻
        sf->publish_us = now_us();

        mirage::FrameReadyEvent evt;
        evt.device_id = CanonicalCoordSystem::DEVICE_ID;
        evt.frame     = sf;
        evt.rgba_data = nullptr;            // legacy path 使わない
        evt.width     = sf->width;
        evt.height    = sf->height;
        evt.frame_id  = sf->frame_id;

        mirage::bus().publish(evt);
        frame_count_.fetch_add(1);
        last_frame_us_.store(now_us());
    }
};

} // namespace mirage::x1
