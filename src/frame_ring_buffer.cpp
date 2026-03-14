// =============================================================================
// FrameRingBuffer — Implementation
// =============================================================================
#include "frame_ring_buffer.hpp"
#include "event_bus.hpp"  // SharedFrame full definition
#include "mirage_log.hpp"

namespace gui {

FrameRingBuffer::FrameRingBuffer(size_t capacity)
    : capacity_(capacity > 0 ? capacity : DEFAULT_CAPACITY)
    , slots_(capacity_) {
    MLOG_INFO("ringbuf", "FrameRingBuffer created: capacity=%zu", capacity_);
}

void FrameRingBuffer::push(std::shared_ptr<mirage::SharedFrame> frame, int64_t decode_ts) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == capacity_) {
        // Buffer full — overwrite oldest (tail), count as dropped
        tail_ = (tail_ + 1) % capacity_;
        dropped_frames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        count_++;
    }

    auto& slot = slots_[head_];
    slot.frame = std::move(frame);
    slot.metrics.frame_id   = slot.frame ? slot.frame->frame_id : 0;
    slot.metrics.decode_ts  = decode_ts;
    slot.metrics.enqueue_ts = now_us();
    slot.metrics.upload_ts  = 0;

    head_ = (head_ + 1) % capacity_;
    total_pushed_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<mirage::SharedFrame> FrameRingBuffer::pop_latest(FrameMetrics* metrics_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return nullptr;
    }

    // Skip to the newest frame (head - 1), count skipped as dropped
    size_t skipped = count_ - 1;
    if (skipped > 0) {
        dropped_frames_.fetch_add(skipped, std::memory_order_relaxed);
    }

    // Newest frame is at (head_ - 1 + capacity_) % capacity_
    size_t newest = (head_ + capacity_ - 1) % capacity_;
    auto frame = std::move(slots_[newest].frame);
    last_pop_metrics_ = slots_[newest].metrics;

    if (metrics_out) {
        *metrics_out = last_pop_metrics_;
    }

    // Clear all slots
    for (size_t i = 0; i < count_; ++i) {
        size_t idx = (tail_ + i) % capacity_;
        slots_[idx].frame.reset();
    }

    tail_ = head_;
    count_ = 0;
    total_popped_.fetch_add(1, std::memory_order_relaxed);

    return frame;
}

std::shared_ptr<mirage::SharedFrame> FrameRingBuffer::pop_next(FrameMetrics* metrics_out) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return nullptr;
    }

    auto frame = std::move(slots_[tail_].frame);
    last_pop_metrics_ = slots_[tail_].metrics;

    if (metrics_out) {
        *metrics_out = last_pop_metrics_;
    }

    slots_[tail_].frame.reset();
    tail_ = (tail_ + 1) % capacity_;
    count_--;
    total_popped_.fetch_add(1, std::memory_order_relaxed);

    return frame;
}

size_t FrameRingBuffer::current_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

void FrameRingBuffer::record_upload_ts(int64_t upload_ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_pop_metrics_.upload_ts = upload_ts;
}

FrameMetrics FrameRingBuffer::last_pop_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_pop_metrics_;
}

void FrameRingBuffer::reset_stats() {
    dropped_frames_.store(0, std::memory_order_relaxed);
    total_pushed_.store(0, std::memory_order_relaxed);
    total_popped_.store(0, std::memory_order_relaxed);
}

} // namespace gui
