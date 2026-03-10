// =============================================================================
// test_latency.cpp  v1
// Canonical Lane end-to-end レイテンシ計測テスト
//
// 計測ポイント (PC monotonic 基準):
//   T1 = decode_done_us   : JPEG decode 完了 (assembler内)
//   T2 = publish_us       : EventBus publish 直前
//   T3 = ai_recv_us       : AIEngine::onFrameReady() 受信
//   T4 = ai_done_us       : processFrameAsync 完了コールバック (近似)
//
// 表示:
//   T2-T1 = SharedFrame 生成コスト  (数μs であるべき)
//   T3-T2 = EventBus dispatch 遅延 (数μs〜数十μs)
//   T3-T1 = decode→AI受信 合計
//
// Android側 (参考、absolute比較不可):
//   encode_done_ms が pts_us[63:32] に埋め込まれている
//   → フレームごとの encode_done_ms 差分で encode 間隔を確認
// =============================================================================
#include "stream/canonical_frame_provider.hpp"
#include "ai_engine.hpp"
#include "event_bus.hpp"
#include "mirage_log.hpp"
#include "vulkan/vulkan_context.hpp"

#include <stdio.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>

using clk = std::chrono::steady_clock;

static constexpr int TEST_SEC       = 20;
static constexpr const char* DEVICE_IP = "192.168.0.3";

struct LatencySample {
    uint64_t decode_done_us;   // T1
    uint64_t publish_us;       // T2
    uint64_t ai_recv_us;       // T3 (EventBus subscriber)
    uint64_t frame_id;
    uint32_t encode_done_ms_delta; // encode_done_ms の差分 (前フレームとの間隔 ms)
};

static std::mutex          g_mtx;
static std::vector<LatencySample> g_samples;
static std::atomic<uint64_t> g_can_events{0};
static std::atomic<uint64_t> g_leg_events{0};
static uint32_t            g_prev_encode_done_ms = 0;
static mirage::SubscriptionHandle g_sub;

static uint64_t now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            clk::now().time_since_epoch()).count());
}

int main() {
    printf("=== Canonical Lane Latency Measurement ===\n");
    printf("Device   : %s\n", DEVICE_IP);
    printf("Duration : %d sec\n\n", TEST_SEC);
    fflush(stdout);

    // ── Vulkan ───────────────────────────────────────────────────────────────
    auto vk_ctx = std::make_unique<mirage::vk::VulkanContext>();
    if (!vk_ctx->initialize("LatencyTest")) {
        fprintf(stderr, "[FAIL] Vulkan\n"); return 1;
    }
    printf("[OK] Vulkan initialized\n");

    // ── AIEngine (最小構成、テンプレートなし) ─────────────────────────────────
    auto ai = std::make_unique<mirage::ai::AIEngine>();
    mirage::ai::AIConfig cfg;
    cfg.templates_dir    = "templates";
    cfg.subscribe_events = true;
    cfg.max_idle_frames  = 9999;
    auto res = ai->initialize(cfg, vk_ctx.get());
    if (!res.is_ok()) {
        fprintf(stderr, "[FAIL] AIEngine: %s\n", res.error().message.c_str());
        vk_ctx->shutdown(); return 1;
    }
    ai->setCanonicalOnly(true);
    ai->setAsyncMode(true);
    printf("[OK] AIEngine ready (no templates, latency-only mode)\n");

    // ── EventBus 計測サブスクライバ ──────────────────────────────────────────
    g_sub = mirage::bus().subscribe<mirage::FrameReadyEvent>(
        [](const mirage::FrameReadyEvent& evt) {
            const uint64_t t3 = now_us();
            if (evt.device_id == "x1_canonical") {
                g_can_events++;
                if (evt.frame) {
                    LatencySample s;
                    s.decode_done_us = evt.frame->decode_done_us;
                    s.publish_us     = evt.frame->publish_us;
                    s.ai_recv_us     = t3;
                    s.frame_id       = evt.frame->frame_id;

                    // encode_done_ms は pts_us[63:32] に埋まっている (before assembler stripped it)
                    // ※ assembler で pts_us = original & 0xFFFFFFFF に変換済みなので
                    //   ここでは直接取れない → フレーム間隔で確認
                    s.encode_done_ms_delta = 0;

                    std::lock_guard<std::mutex> lk(g_mtx);
                    // サンプル数を制限 (メモリ節約)
                    if ((int)g_samples.size() < 1000) {
                        g_samples.push_back(s);
                    }
                }
            } else {
                g_leg_events++;
            }
        });
    printf("[OK] EventBus subscriber ready\n\n");

    // ── CanonicalFrameProvider ────────────────────────────────────────────────
    auto prov = std::make_unique<mirage::x1::CanonicalFrameProvider>();
    if (!prov->start(DEVICE_IP)) {
        fprintf(stderr, "[FAIL] CanonicalFrameProvider\n");
        ai->shutdown(); vk_ctx->shutdown(); return 1;
    }
    printf("[OK] CanonicalFrameProvider started\n");
    printf("Collecting samples for %d seconds...\n\n", TEST_SEC);
    fflush(stdout);

    std::this_thread::sleep_for(std::chrono::seconds(TEST_SEC));

    prov->stop();
    g_sub = mirage::SubscriptionHandle{};
    ai->shutdown();
    vk_ctx->shutdown();

    // ── 統計計算 ──────────────────────────────────────────────────────────────
    std::lock_guard<std::mutex> lk(g_mtx);
    const int N = (int)g_samples.size();
    if (N < 10) {
        printf("[WARN] Too few samples (%d), check connection\n", N);
        return 1;
    }

    // 各区間を収集
    std::vector<double> dt_t2_t1, dt_t3_t2, dt_t3_t1;
    for (auto& s : g_samples) {
        if (s.decode_done_us == 0 || s.publish_us == 0 || s.ai_recv_us == 0) continue;
        dt_t2_t1.push_back((double)(s.publish_us     - s.decode_done_us));
        dt_t3_t2.push_back((double)(s.ai_recv_us     - s.publish_us));
        dt_t3_t1.push_back((double)(s.ai_recv_us     - s.decode_done_us));
    }

    // フレーム間隔 (fps 確認)
    std::vector<double> frame_intervals;
    for (int i = 1; i < N; ++i) {
        if (g_samples[i].decode_done_us > g_samples[i-1].decode_done_us) {
            double iv = (double)(g_samples[i].decode_done_us - g_samples[i-1].decode_done_us);
            if (iv < 200000.0) // 外れ値除去 (200ms以上は除外)
                frame_intervals.push_back(iv);
        }
    }

    auto stats = [](const std::vector<double>& v, const char* name, const char* unit) {
        if (v.empty()) { printf("  %s: (no data)\n", name); return; }
        double sum = 0; for (auto x : v) sum += x;
        double mean = sum / v.size();
        double sq = 0; for (auto x : v) sq += (x - mean)*(x - mean);
        double sd = sqrt(sq / v.size());
        auto sorted = v; std::sort(sorted.begin(), sorted.end());
        double p50 = sorted[sorted.size()*50/100];
        double p95 = sorted[sorted.size()*95/100];
        double p99 = sorted[sorted.size()*99/100];
        printf("  %-28s  n=%-4d  mean=%6.1f  sd=%5.1f  p50=%6.1f  p95=%6.1f  p99=%6.1f  [%s]\n",
               name, (int)v.size(), mean, sd, p50, p95, p99, unit);
    };

    printf("=== Latency Results (%d samples) ===\n\n", N);
    printf("--- PC-side intervals (monotonic, reliable) ---\n");
    stats(dt_t2_t1,    "T2-T1 decode→publish",     "us");
    stats(dt_t3_t2,    "T3-T2 publish→ai_recv",    "us");
    stats(dt_t3_t1,    "T3-T1 decode→ai_recv",     "us");

    printf("\n--- Frame intervals (decode_done_us delta) ---\n");
    stats(frame_intervals, "frame interval",            "us");
    if (!frame_intervals.empty()) {
        double sum = 0; for (auto x : frame_intervals) sum += x;
        double mean_iv = sum / frame_intervals.size();
        printf("  avg FPS (from intervals)  : %.1f fps\n", 1e6 / mean_iv);
    }

    printf("\n--- Summary ---\n");
    printf("  can_events  : %llu\n", (unsigned long long)g_can_events.load());
    printf("  leg_events  : %llu (should be 0)\n", (unsigned long long)g_leg_events.load());
    printf("  samples     : %d\n", N);

    // Assembler drop stats
    auto rs = prov->get_recv_stats();
    printf("\n--- Assembler Stats (drop diagnosis) ---\n");
    printf("  delivered              : %llu\n", (unsigned long long)rs.delivered);
    printf("  incomplete_evicted     : %llu (UDP frag loss)\n", (unsigned long long)rs.incomplete_frames_evicted);
    printf("  dropped_old            : %llu (OOO reorder)\n", (unsigned long long)rs.dropped_old);
    printf("  decode_failed          : %llu\n", (unsigned long long)rs.frames_decode_failed);
    printf("  fragments_received     : %llu\n", (unsigned long long)rs.fragments_received);
    printf("  fragments_expected     : %llu\n", (unsigned long long)rs.fragments_expected);
    if (rs.fragments_expected > 0) {
        double loss_rate = 100.0 * (1.0 - (double)rs.fragments_received / rs.fragments_expected);
        printf("  fragment loss rate     : %.2f%%\n", loss_rate);
        printf("  avg frags/frame        : %.1f\n",
               rs.delivered > 0 ? (double)rs.fragments_received / rs.delivered : 0.0);
    }
    uint64_t android_sent = rs.delivered + rs.incomplete_frames_evicted;
    printf("  est Android FPS        : %.1f  PC FPS: %.1f  drop: %.1f%%\n",
           android_sent / (double)TEST_SEC,
           rs.delivered / (double)TEST_SEC,
           android_sent > 0 ? 100.0 * rs.incomplete_frames_evicted / android_sent : 0.0);

    printf("\nNote: End-to-end (Android capture → PC AI) requires Android-PC clock sync.\n");
    printf("      T3-T1 measures PC-internal path only (decode_done → AI callback).\n");
    printf("      For Android→PC latency, see encode_done_ms delta + JPEG size in logcat.\n");

    fflush(stdout);
    return (g_leg_events.load() == 0) ? 0 : 1;
}
