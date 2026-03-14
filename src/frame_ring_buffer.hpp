#pragma once
// =============================================================================
// FrameRingBuffer — Lock-based SPSC ring buffer for decoded video frames
// =============================================================================
// A-1タスク: current_shared_frame_の上書きモデルを複数スロット保持に置換。
// 主目的: drop可視化・可観測性強化・GUI/Layer2消費タイミングの安定化。
//
// 設計判断 (CURRENT_TRUTH.md準拠):
//   - mutex分離方式（lock-freeは必要時のみ切替検討）
//   - ring size = 4 (default)
//   - FrameBufferPoolは既存を活用（このクラスでは所有しない）
//   - Copy #1 (decode→SharedFrame memcpy) は残る（UnifiedDecoder I/F制約）
// =============================================================================

#include <memory>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <array>
#include <vector>

// Forward declaration
namespace mirage {
struct SharedFrame;
}

namespace gui {

// Observability metrics for a single frame
struct FrameMetrics {
    uint64_t frame_id    = 0;
    int64_t  decode_ts   = 0;  // steady_clock microseconds
    int64_t  enqueue_ts  = 0;  // steady_clock microseconds
    int64_t  upload_ts   = 0;  // set by VulkanTexture::update()
};

class FrameRingBuffer {
public:
    static constexpr size_t DEFAULT_CAPACITY = 4;

    explicit FrameRingBuffer(size_t capacity = DEFAULT_CAPACITY);
    ~FrameRingBuffer() = default;

    // Non-copyable, non-movable
    FrameRingBuffer(const FrameRingBuffer&) = delete;
    FrameRingBuffer& operator=(const FrameRingBuffer&) = delete;

    // --- Producer (decode thread) ---

    // Push a new frame. If buffer is full, oldest frame is overwritten (dropped).
    // decode_ts should be set by caller before push.
    void push(std::shared_ptr<mirage::SharedFrame> frame, int64_t decode_ts = 0);

    // --- Consumer (GUI thread / Layer2) ---

    // Pop the latest frame (skips intermediate frames, which count as dropped).
    // Returns nullptr if no new frame available.
    // This is the primary consumption path — GUI always wants the newest frame.
    std::shared_ptr<mirage::SharedFrame> pop_latest(FrameMetrics* metrics_out = nullptr);

    // Pop the next frame in order (FIFO). Returns nullptr if empty.
    // Use this for Layer2 sequential processing if needed.
    std::shared_ptr<mirage::SharedFrame> pop_next(FrameMetrics* metrics_out = nullptr);

    // --- Observability ---

    uint64_t dropped_frames() const { return dropped_frames_.load(std::memory_order_relaxed); }
    uint64_t total_pushed()   const { return total_pushed_.load(std::memory_order_relaxed); }
    uint64_t total_popped()   const { return total_popped_.load(std::memory_order_relaxed); }
    size_t   current_count()  const;  // Number of frames currently in buffer
    size_t   capacity()       const { return capacity_; }

    // Record upload timestamp for the most recently popped frame
    void record_upload_ts(int64_t upload_ts);

    // Get metrics for the most recently popped frame
    FrameMetrics last_pop_metrics() const;

    // Reset all counters (for testing)
    void reset_stats();

    // Utility: current steady_clock in microseconds
    static int64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    struct Slot {
        std::shared_ptr<mirage::SharedFrame> frame;
        FrameMetrics metrics;
    };

    const size_t capacity_;
    std::vector<Slot> slots_;
    size_t head_ = 0;  // Next write position
    size_t tail_ = 0;  // Next read position
    size_t count_ = 0; // Current number of frames in buffer

    mutable std::mutex mutex_;  // Separate from frame_mtx_ in MirrorReceiver

    // Observability counters
    std::atomic<uint64_t> dropped_frames_{0};
    std::atomic<uint64_t> total_pushed_{0};
    std::atomic<uint64_t> total_popped_{0};

    // Last popped frame metrics (for upload_ts recording)
    FrameMetrics last_pop_metrics_;
};

} // namespace gui
