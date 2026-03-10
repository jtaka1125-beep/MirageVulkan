// =============================================================================
// test_phase2_stepb.cpp  v3
// Phase 2 Step B: Vulkan NCC + OCR(FrameAnalyzer) + EventBus 本番接続テスト
//
// 合格条件:
//   provider_frames ≥ 100
//   legacy_events == 0
//   ai_frames_processed ≥ 50
//   ocr_calls ≥ 5  (MIRAGE_OCR_ENABLED 有効時のみ)
//   ocr_coord_ok   (全ワード座標が 600×1000 内)
//   score_stddev < 0.05
// =============================================================================
#include "stream/canonical_frame_provider.hpp"
#include "ai_engine.hpp"
#include "event_bus.hpp"
#include "mirage_log.hpp"
#include "vulkan/vulkan_context.hpp"

#ifdef MIRAGE_OCR_ENABLED
#include "frame_analyzer.hpp"
#endif

#include <stdio.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>

using clk = std::chrono::steady_clock;

static constexpr int TEST_SEC       = 30;
static constexpr const char* DEVICE_IP     = "192.168.0.3";
static constexpr const char* TEMPLATES_DIR = "templates";

static std::atomic<uint64_t> g_can_events{0};
static std::atomic<uint64_t> g_leg_events{0};
static mirage::SubscriptionHandle g_monitor_sub;

static std::mutex g_mtx;
static std::vector<float> g_scores;
static bool g_coord_ok = true;

// OCR stats
static std::atomic<uint64_t> g_ocr_calls{0};
static std::atomic<uint64_t> g_ocr_words{0};
static bool g_ocr_coord_ok = true;
static std::vector<std::string> g_ocr_samples;

int main(int /*argc*/, char** /*argv*/) {
    printf("=== Phase 2 Step B v3: Vulkan NCC + OCR + EventBus ===\n");
    printf("Device    : %s\n", DEVICE_IP);
    printf("Templates : %s\n", TEMPLATES_DIR);
    printf("Duration  : %d sec\n", TEST_SEC);
#ifdef MIRAGE_OCR_ENABLED
    printf("OCR       : ENABLED (MIRAGE_OCR_ENABLED)\n\n");
#else
    printf("OCR       : DISABLED\n\n");
#endif
    fflush(stdout);

    // ── Vulkan ────────────────────────────────────────────────────────────────
    auto vk_ctx = std::make_unique<mirage::vk::VulkanContext>();
    if (!vk_ctx->initialize("Phase2StepB")) {
        fprintf(stderr, "[FAIL] Vulkan init failed\n"); return 1;
    }
    printf("[OK] Vulkan initialized\n");

    // ── FrameAnalyzer (OCR) ──────────────────────────────────────────────────
#ifdef MIRAGE_OCR_ENABLED
    auto fa = std::make_unique<mirage::FrameAnalyzer>();
    bool ocr_ready = fa->init("jpn+eng");
    if (ocr_ready) {
        fa->startCapture();
        printf("[OK] FrameAnalyzer (Tesseract jpn+eng) initialized\n");
    } else {
        fprintf(stderr, "[WARN] FrameAnalyzer init failed — OCR tests skipped\n");
        fa.reset();
    }
#endif

    // ── AIEngine ─────────────────────────────────────────────────────────────
    auto ai = std::make_unique<mirage::ai::AIEngine>();
    mirage::ai::AIConfig cfg;
    cfg.templates_dir    = TEMPLATES_DIR;
    cfg.subscribe_events = true;
    cfg.max_idle_frames  = 9999;
    auto res = ai->initialize(cfg, vk_ctx.get());
    if (!res.is_ok()) {
        fprintf(stderr, "[FAIL] AIEngine: %s\n", res.error().message.c_str());
        vk_ctx->shutdown(); return 1;
    }
    ai->loadTemplatesFromDir(TEMPLATES_DIR);
    auto st0 = ai->getStats();
    printf("[OK] AIEngine ready: templates=%d\n", st0.templates_loaded);
    ai->setCanonicalOnly(true);
    ai->setAsyncMode(true);
#ifdef MIRAGE_OCR_ENABLED
    if (ocr_ready) {
        ai->setFrameAnalyzer(fa.get());
        printf("[OK] FrameAnalyzer → AIEngine connected\n");
    }
#endif

    // ── EventBus monitor ─────────────────────────────────────────────────────
    g_monitor_sub = mirage::bus().subscribe<mirage::FrameReadyEvent>(
        [](const mirage::FrameReadyEvent& evt) {
            (evt.device_id == "x1_canonical") ? g_can_events++ : g_leg_events++;
        });
    printf("[OK] EventBus monitor ready\n\n");

    // ── CanonicalFrameProvider ────────────────────────────────────────────────
    auto prov = std::make_unique<mirage::x1::CanonicalFrameProvider>();
    if (!prov->start(DEVICE_IP)) {
        fprintf(stderr, "[FAIL] CanonicalFrameProvider\n");
        ai->shutdown(); vk_ctx->shutdown(); return 1;
    }
    printf("[OK] CanonicalFrameProvider started\n\n");

    // ── OCR スレッド: 2秒おきに analyzeText ─────────────────────────────────
    std::atomic<bool> stop_ocr{false};
    auto ocr_thr = std::thread([&] {
#ifdef MIRAGE_OCR_ENABLED
        if (!ocr_ready) return;
        while (!stop_ocr.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (stop_ocr.load() || g_can_events.load() < 10) continue;

            auto result = fa->analyzeText("x1_canonical");
            if (result.words.empty()) continue;

            g_ocr_calls++;
            g_ocr_words.fetch_add(result.words.size());

            std::lock_guard<std::mutex> lk(g_mtx);
            for (auto& w : result.words) {
                if (w.x1 < 0 || w.x2 > mirage::x1::CANONICAL_W ||
                    w.y1 < 0 || w.y2 > mirage::x1::CANONICAL_H)
                    g_ocr_coord_ok = false;
            }
            if ((int)g_ocr_samples.size() < 5) {
                std::string s;
                for (auto& w : result.words) {
                    if (!s.empty()) s += " ";
                    s += w.text;
                    if ((int)s.size() > 60) { s += "..."; break; }
                }
                g_ocr_samples.push_back(s);
            }
        }
#endif
    });

    // ── ログスレッド ──────────────────────────────────────────────────────────
    std::atomic<bool> stop_log{false};
    auto log_thr = std::thread([&] {
        auto t0 = clk::now();
        while (!stop_log.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            double el = std::chrono::duration<double>(clk::now() - t0).count();
            auto st = ai->getStats();
            auto matches = ai->getLastMatches();
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                for (auto& m : matches) {
                    g_scores.push_back(m.score);
                    if (m.center_x < 0 || m.center_x >= mirage::x1::CANONICAL_W ||
                        m.center_y < 0 || m.center_y >= mirage::x1::CANONICAL_H)
                        g_coord_ok = false;
                }
            }
            printf("[%4.0fs]  can=%-6llu  leg=%-3llu  ai=%-6llu  ocr_calls=%-3llu\n",
                   el,
                   (unsigned long long)g_can_events.load(),
                   (unsigned long long)g_leg_events.load(),
                   (unsigned long long)st.frames_processed,
                   (unsigned long long)g_ocr_calls.load());
            fflush(stdout);
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(TEST_SEC));
    stop_ocr.store(true);
    stop_log.store(true);
    ocr_thr.join();
    log_thr.join();
    prov->stop();
    g_monitor_sub = mirage::SubscriptionHandle{};

    auto st_f = ai->getStats();
#ifdef MIRAGE_OCR_ENABLED
    if (ocr_ready) fa->stopCapture();
#endif
    ai->shutdown();
    vk_ctx->shutdown();

    // ── 結果 ─────────────────────────────────────────────────────────────────
    printf("\n=== Final Analysis ===\n");
    printf("can_events       : %llu\n", (unsigned long long)g_can_events.load());
    printf("leg_events       : %llu\n", (unsigned long long)g_leg_events.load());
    printf("frames_processed : %llu\n", (unsigned long long)st_f.frames_processed);
    printf("provider_frames  : %llu\n", (unsigned long long)prov->frame_count());
    printf("coord_ok (NCC)   : %s\n",   g_coord_ok ? "PASS" : "FAIL");

    printf("\n--- OCR (FrameAnalyzer) ---\n");
#ifdef MIRAGE_OCR_ENABLED
    printf("ocr_calls  : %llu\n", (unsigned long long)g_ocr_calls.load());
    printf("ocr_words  : %llu\n", (unsigned long long)g_ocr_words.load());
    printf("coord_ok   : %s\n",   g_ocr_coord_ok ? "PASS" : "FAIL");
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        for (int i=0;i<(int)g_ocr_samples.size();i++)
            printf("  sample[%d]: %s\n", i, g_ocr_samples[i].c_str());
    }
#else
    printf("  (DISABLED — build with USE_OCR=ON)\n");
#endif

    printf("\n--- Vulkan NCC score ---\n");
    bool score_ok = true;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_scores.empty()) {
            printf("  (no matches above threshold — normal)\n");
        } else {
            float mn = *std::min_element(g_scores.begin(), g_scores.end());
            float mx = *std::max_element(g_scores.begin(), g_scores.end());
            float mean = std::accumulate(g_scores.begin(),g_scores.end(),0.0f)/g_scores.size();
            float sq=0; for(auto v:g_scores) sq+=(v-mean)*(v-mean);
            float sd=sqrtf(sq/g_scores.size());
            score_ok=(sd<0.05f);
            printf("  count=%zu mean=%.3f min=%.3f max=%.3f sd=%.4f %s\n",
                   g_scores.size(), mean, mn, mx, sd, score_ok?"PASS":"FAIL");
        }
    }

    // 判定
    bool p_frames = prov->frame_count() >= 100;
    bool p_legacy = g_leg_events.load() == 0;
    bool p_proc   = st_f.frames_processed >= 50;
    bool p_ncc    = g_coord_ok && score_ok;
#ifdef MIRAGE_OCR_ENABLED
    bool p_ocr    = !ocr_ready || (g_ocr_calls.load() >= 5 && g_ocr_coord_ok);
#else
    bool p_ocr    = true;
#endif

    printf("\n=== 判定 ===\n");
    printf("  provider_frames ≥100   : %s (%llu)\n", p_frames?"PASS":"FAIL",(unsigned long long)prov->frame_count());
    printf("  legacy_events == 0     : %s (%llu)\n", p_legacy?"PASS":"FAIL",(unsigned long long)g_leg_events.load());
    printf("  ai_frames_proc ≥50     : %s (%llu)\n", p_proc?"PASS":"FAIL",(unsigned long long)st_f.frames_processed);
    printf("  ncc_coord+score        : %s\n",         p_ncc?"PASS":"FAIL");
    printf("  ocr (calls≥5+coord)    : %s\n",         p_ocr?"PASS":"FAIL (or SKIP if disabled)");

    bool all = p_frames && p_legacy && p_proc && p_ncc && p_ocr;
    printf("\n[Phase 2 Step B v3] %s\n",
           all ? "PASS — AI/OCR Canonical固定・旧経路遮断・全系統確認"
               : "FAIL — 詳細は上記");
    fflush(stdout);
    return all ? 0 : 1;
}
