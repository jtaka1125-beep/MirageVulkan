#pragma once
// =============================================================================
// MirageSystem - RTT Tracker (Header-Only)
// =============================================================================
// PING/PONG RTT計測、レイテンシ統計、遅延分類を提供する
// AtomicEMA: lock-freeな指数移動平均（atomic CAS使用）
// LatencyHistogram: レイテンシ分布バケット
// RttTracker: RTT計測 + 遅延分類（good/warning/critical）+ 統計
// =============================================================================

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace mirage {

// =============================================================================
// AtomicEMA - lock-freeな指数移動平均
// =============================================================================
// atomic CASループでスレッドセーフにEMAを更新する。
// alpha: 新しい値の重み（0.0〜1.0、大きいほど追従が速い）
// =============================================================================
class AtomicEMA {
public:
    explicit AtomicEMA(double alpha = 0.1) : alpha_(alpha), value_(0.0) {}

    // 新しい値でEMAを更新（lock-free CASループ）
    void update(double new_value) {
        double old_val, new_val;
        do {
            old_val = value_.load(std::memory_order_relaxed);
            if (old_val == 0.0) {
                new_val = new_value;  // 初回値はそのまま採用
            } else {
                new_val = old_val * (1.0 - alpha_) + new_value * alpha_;
            }
        } while (!value_.compare_exchange_weak(old_val, new_val,
                                                std::memory_order_release,
                                                std::memory_order_relaxed));
    }

    // 現在のEMA値を取得
    double get() const {
        return value_.load(std::memory_order_acquire);
    }

    // リセット
    void reset() {
        value_.store(0.0, std::memory_order_release);
    }

private:
    double alpha_;
    std::atomic<double> value_;
};

// =============================================================================
// LatencyHistogram - レイテンシ分布バケット
// =============================================================================
// 9段階のバケットでレイテンシ分布を記録する。
// スレッドセーフ（atomic操作のみ）。
// =============================================================================
class LatencyHistogram {
public:
    static constexpr size_t NUM_BUCKETS = 9;

    // バケット境界値（ms）: [0,5), [5,10), [10,20), [20,50), [50,100),
    //                       [100,200), [200,500), [500,1000), [1000,+∞)
    static constexpr double BUCKET_BOUNDS[NUM_BUCKETS] = {
        5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 1e9
    };

    // レイテンシ値を記録
    void record(double ms) {
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            if (ms < BUCKET_BOUNDS[i]) {
                buckets_[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        buckets_[NUM_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
    }

    // 指定パーセンタイルの推定値を返す（0〜100）
    double percentile(double p) const {
        uint64_t total = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            total += buckets_[i].load(std::memory_order_relaxed);
        }
        if (total == 0) return 0.0;

        uint64_t target = static_cast<uint64_t>(std::ceil(total * p / 100.0));
        uint64_t cumulative = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            cumulative += buckets_[i].load(std::memory_order_relaxed);
            if (cumulative >= target) {
                return BUCKET_BOUNDS[i];
            }
        }
        return BUCKET_BOUNDS[NUM_BUCKETS - 1];
    }

    // 特定バケットのカウント取得
    uint64_t bucket_count(size_t idx) const {
        if (idx >= NUM_BUCKETS) return 0;
        return buckets_[idx].load(std::memory_order_relaxed);
    }

    // 全サンプル数
    uint64_t total_count() const {
        uint64_t total = 0;
        for (size_t i = 0; i < NUM_BUCKETS; ++i) {
            total += buckets_[i].load(std::memory_order_relaxed);
        }
        return total;
    }

    // リセット
    void reset() {
        for (auto& b : buckets_) b.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<uint64_t>, NUM_BUCKETS> buckets_{};
};

// C++17 inline constexpr で ODR問題を回避
inline constexpr double LatencyHistogram::BUCKET_BOUNDS[];

// =============================================================================
// RttTracker - RTT計測 + 遅延分類 + 統計
// =============================================================================
// PING送信時刻を記録し、PONG受信時にRTTを算出する。
// 遅延を good / warning / critical に分類する。
// =============================================================================
class RttTracker {
public:
    // 遅延レベル分類
    enum class Level { GOOD, WARNING, CRITICAL };

    // 遅延しきい値（ms）
    static constexpr double WARNING_THRESHOLD_MS  = 50.0;
    static constexpr double CRITICAL_THRESHOLD_MS = 200.0;

    // PING送信を記録
    void record_ping_sent(uint16_t seq) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_pings_[seq] = std::chrono::steady_clock::now();

        // 古いエントリを削除（30秒超）
        auto now = std::chrono::steady_clock::now();
        for (auto it = pending_pings_.begin(); it != pending_pings_.end(); ) {
            auto age_s = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second).count();
            if (age_s > 30) {
                it = pending_pings_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // PONG受信を記録し、RTT(ms)を返す。対応するPINGがなければnullopt
    std::optional<double> record_pong_recv(uint16_t ack_seq) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_pings_.find(ack_seq);
        if (it == pending_pings_.end()) {
            return std::nullopt;
        }

        auto now = std::chrono::steady_clock::now();
        double rtt_ms = std::chrono::duration_cast<std::chrono::microseconds>(
            now - it->second).count() / 1000.0;
        pending_pings_.erase(it);
        return rtt_ms;
    }

    // RTT値でEMA・ヒストグラム・min/maxを更新する
    void update(double rtt_ms) {
        avg_rtt_.update(rtt_ms);
        histogram_.record(rtt_ms);

        // min更新（CASループ）
        double old_min = min_rtt_.load(std::memory_order_relaxed);
        while (rtt_ms < old_min) {
            if (min_rtt_.compare_exchange_weak(old_min, rtt_ms,
                    std::memory_order_release, std::memory_order_relaxed))
                break;
        }

        // max更新（CASループ）
        double old_max = max_rtt_.load(std::memory_order_relaxed);
        while (rtt_ms > old_max) {
            if (max_rtt_.compare_exchange_weak(old_max, rtt_ms,
                    std::memory_order_release, std::memory_order_relaxed))
                break;
        }

        sample_count_.fetch_add(1, std::memory_order_relaxed);
    }

    // 現在の遅延レベルを判定
    Level classify() const {
        double avg = avg_rtt_.get();
        if (avg >= CRITICAL_THRESHOLD_MS) return Level::CRITICAL;
        if (avg >= WARNING_THRESHOLD_MS)  return Level::WARNING;
        return Level::GOOD;
    }

    // 遅延レベルを文字列で取得
    static const char* level_str(Level lv) {
        switch (lv) {
            case Level::GOOD:     return "good";
            case Level::WARNING:  return "warning";
            case Level::CRITICAL: return "critical";
        }
        return "unknown";
    }

    // アクセサ
    double avg_rtt_ms() const { return avg_rtt_.get(); }
    double min_rtt_ms() const { return min_rtt_.load(std::memory_order_acquire); }
    double max_rtt_ms() const { return max_rtt_.load(std::memory_order_acquire); }
    uint64_t sample_count() const { return sample_count_.load(std::memory_order_relaxed); }

    const LatencyHistogram& histogram() const { return histogram_; }

    double p50() const { return histogram_.percentile(50); }
    double p95() const { return histogram_.percentile(95); }
    double p99() const { return histogram_.percentile(99); }

    // 保留中のPINGをクリア
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_pings_.clear();
    }

    // 全統計リセット
    void reset() {
        clear();
        avg_rtt_.reset();
        histogram_.reset();
        min_rtt_.store(999999.0, std::memory_order_release);
        max_rtt_.store(0.0, std::memory_order_release);
        sample_count_.store(0, std::memory_order_relaxed);
    }

private:
    std::mutex mutex_;
    std::unordered_map<uint16_t, std::chrono::steady_clock::time_point> pending_pings_;

    AtomicEMA avg_rtt_{0.1};
    LatencyHistogram histogram_;
    std::atomic<double> min_rtt_{999999.0};
    std::atomic<double> max_rtt_{0.0};
    std::atomic<uint64_t> sample_count_{0};
};

} // namespace mirage
