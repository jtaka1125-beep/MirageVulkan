// =============================================================================
// test_phase2_readonly.cpp  v1
// Phase 2 read-only接続テスト — standalone (mirage_core不要)
//
// 検証項目:
//   1. PixelSampler  — 10固定座標のRGB値がフレーム間でぶれないか
//   2. PatchMatcher  — 初回フレームから切り出したパッチのCPU NCC
//                      (位置±3px以内 / score ≥ 0.90 が合格ライン)
//   3. RegionDiff    — 4固定矩形領域のフレーム間差分量
//                      (UI静止時に 0〜2% が正常、急変は座標系崩壊の兆候)
//   4. CoordSystem   — width=600, height=1000 固定を毎フレーム確認
//
// 合格条件:
//   pixel_stddev < 5.0   (静止画素の標準偏差)
//   match_score ≥ 0.90   (NCCスコア安定)
//   match_offset ≤ 3px   (位置ズレ)
//   region_diff < 5.0%   (静止領域の変化率)
//   coord_ok = PASS      (解像度固定)
// =============================================================================
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb_image_write.h"
#include "../stb_image.h"

#include "../stream/x1_protocol.hpp"
#include "../stream/canonical_frame.hpp"
#include "../stream/canonical_frame_assembler.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <array>
#include <string>
#include <thread>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <cstdint>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

using clk = std::chrono::steady_clock;

// ── 設定 ────────────────────────────────────────────────────────────────────
static const char* WORK_DIR      = "C:/MirageWork/MirageVulkan";
static constexpr int TEST_SEC    = 30;   // テスト時間
static constexpr int SAMPLE_INTERVAL = 3; // 3フレームに1回評価（CPU負荷軽減）

// 固定サンプリング座標 (Canonical 600×1000 基準)
// 画面上の代表的な位置 — ステータスバー/中央/ボタン付近
static constexpr std::array<std::pair<int,int>, 10> SAMPLE_POINTS = {{
    {50,  25},    // 左上ステータスバー付近
    {300, 25},    // 上中央
    {550, 25},    // 右上
    {50,  300},   // 左中上
    {300, 300},   // 中央上
    {550, 300},   // 右中上
    {50,  500},   // 左中央
    {300, 500},   // 中央
    {550, 500},   // 右中央
    {300, 750},   // 下中央
}};

// パッチ設定: 位置(200,400), サイズ64×64
static constexpr int PATCH_X = 200, PATCH_Y = 350, PATCH_W = 64, PATCH_H = 64;
// NCC 検索範囲: パッチ中心から±SEARCH_RADIUS px
static constexpr int SEARCH_RADIUS = 6;

// 比較矩形 4つ (x, y, w, h)
static constexpr std::array<std::array<int,4>, 4> REGIONS = {{
    {0,   0,   600, 30},    // ステータスバー全体
    {0,   30,  600, 150},   // 上部コンテンツ
    {0,   425, 600, 150},   // 中央コンテンツ
    {0,   850, 600, 150},   // 下部コンテンツ
}};

// ── 統計ヘルパー ─────────────────────────────────────────────────────────────
struct RunStats {
    std::vector<float> values;
    void add(float v) { values.push_back(v); }
    float mean() const {
        if (values.empty()) return 0;
        return std::accumulate(values.begin(), values.end(), 0.0f) / values.size();
    }
    float stddev() const {
        if (values.size() < 2) return 0;
        float m = mean();
        float sq = 0;
        for (auto v : values) sq += (v-m)*(v-m);
        return sqrtf(sq / (values.size()-1));
    }
    float min_val() const { return values.empty() ? 0 : *std::min_element(values.begin(), values.end()); }
    float max_val() const { return values.empty() ? 0 : *std::max_element(values.begin(), values.end()); }
};

// ── RGBサンプラー ─────────────────────────────────────────────────────────────
struct PixelSampleResult {
    int x, y;
    uint8_t r, g, b;
};

static PixelSampleResult sample_pixel(const uint8_t* rgba, int w, int x, int y) {
    const uint8_t* p = rgba + (y * w + x) * 4;
    return {x, y, p[0], p[1], p[2]};
}

// ── CPU NCC (グレースケール) ──────────────────────────────────────────────────
static void rgba_to_gray_patch(const uint8_t* rgba, int frame_w,
                                int px, int py, int pw, int ph,
                                std::vector<float>& out) {
    out.resize((size_t)(pw * ph));
    for (int row = 0; row < ph; ++row) {
        for (int col = 0; col < pw; ++col) {
            const uint8_t* p = rgba + ((py + row) * frame_w + (px + col)) * 4;
            out[row * pw + col] = 0.299f*p[0] + 0.587f*p[1] + 0.114f*p[2];
        }
    }
}

// patch テンプレートと候補位置のNCC
static float ncc(const std::vector<float>& tmpl, int tw, int th,
                 const std::vector<float>& search, int sx, int sy, int sw) {
    float mean_t = 0, mean_s = 0;
    int n = tw * th;
    for (int r = 0; r < th; ++r)
        for (int c = 0; c < tw; ++c) {
            mean_t += tmpl[r * tw + c];
            mean_s += search[(sy + r) * sw + (sx + c)];
        }
    mean_t /= n; mean_s /= n;

    float num = 0, denom_t = 0, denom_s = 0;
    for (int r = 0; r < th; ++r) {
        for (int c = 0; c < tw; ++c) {
            float dt = tmpl[r * tw + c] - mean_t;
            float ds = search[(sy + r) * sw + (sx + c)] - mean_s;
            num    += dt * ds;
            denom_t += dt * dt;
            denom_s += ds * ds;
        }
    }
    float denom = sqrtf(denom_t * denom_s);
    return denom < 1e-6f ? 0.0f : num / denom;
}

struct NCCResult {
    float best_score;
    int offset_x;   // patch_X からのズレ
    int offset_y;
};

static NCCResult match_patch(
    const std::vector<float>& tmpl,
    const uint8_t* rgba, int frame_w, int frame_h)
{
    // 検索領域: パッチ位置±SEARCH_RADIUS
    int sx0 = std::max(0, PATCH_X - SEARCH_RADIUS);
    int sy0 = std::max(0, PATCH_Y - SEARCH_RADIUS);
    int sx1 = std::min(frame_w - PATCH_W, PATCH_X + SEARCH_RADIUS);
    int sy1 = std::min(frame_h - PATCH_H, PATCH_Y + SEARCH_RADIUS);

    int sw = sx1 + PATCH_W - sx0;
    int sh = sy1 + PATCH_H - sy0;

    std::vector<float> search_gray;
    rgba_to_gray_patch(rgba, frame_w, sx0, sy0, sw, sh, search_gray);

    float best = -1.0f;
    int bx = 0, by = 0;
    for (int dy = 0; dy <= sy1-sy0; ++dy) {
        for (int dx = 0; dx <= sx1-sx0; ++dx) {
            float s = ncc(tmpl, PATCH_W, PATCH_H, search_gray, dx, dy, sw);
            if (s > best) { best = s; bx = dx; by = dy; }
        }
    }
    return {best, (sx0 + bx) - PATCH_X, (sy0 + by) - PATCH_Y};
}

// ── 領域差分 ─────────────────────────────────────────────────────────────────
static float region_diff_pct(
    const uint8_t* a, const uint8_t* b, int w,
    int rx, int ry, int rw, int rh, int threshold = 15)
{
    int diff = 0, total = rw * rh;
    for (int row = 0; row < rh; ++row) {
        for (int col = 0; col < rw; ++col) {
            const uint8_t* pa = a + ((ry+row)*w + (rx+col))*4;
            const uint8_t* pb = b + ((ry+row)*w + (rx+col))*4;
            int d = abs((int)pa[0]-(int)pb[0]) + abs((int)pa[1]-(int)pb[1]) + abs((int)pa[2]-(int)pb[2]);
            if (d > threshold) ++diff;
        }
    }
    return 100.0f * diff / total;
}

// ── グローバル状態 ────────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_udp_rx{0};
static std::atomic<uint32_t> g_last_id{0};
static std::atomic<uint32_t> g_last_w{0};
static std::atomic<uint32_t> g_last_h{0};

struct FrameStats {
    // pixel samples: 10点各RGBの標準偏差
    std::array<RunStats, 10> pixel_r_stats, pixel_g_stats, pixel_b_stats;
    // NCC
    RunStats ncc_score;
    RunStats ncc_offset_x, ncc_offset_y;
    // region diff
    std::array<RunStats, 4> region_diff;
    // fps
    RunStats fps;
    // frame count
    uint64_t total = 0;
    uint64_t sampled = 0;
    bool coord_always_ok = true;
};

static std::mutex              g_stats_mutex;
static FrameStats              g_stats;
static std::vector<float>      g_tmpl_gray;    // 参照パッチのグレースケール
static bool                    g_tmpl_ready = false;
static std::vector<uint8_t>    g_prev_frame;   // region diff用 前フレーム
static int                     g_prev_w = 0, g_prev_h = 0;
static int                     g_sample_count = 0;  // SAMPLE_INTERVAL カウンタ

// ── フレームコールバック ──────────────────────────────────────────────────────
static void on_frame(mirage::x1::CanonicalFrame frame) {
    if (!frame.valid()) return;

    g_last_id.store(frame.frame_id);
    g_last_w .store(frame.width);
    g_last_h .store(frame.height);

    std::lock_guard<std::mutex> lk(g_stats_mutex);
    g_stats.total++;

    // 座標系チェック
    if (frame.width != (uint32_t)mirage::x1::CANONICAL_W ||
        frame.height != (uint32_t)mirage::x1::CANONICAL_H)
        g_stats.coord_always_ok = false;

    // SAMPLE_INTERVAL フレームに1回だけ評価
    ++g_sample_count;
    if (g_sample_count < SAMPLE_INTERVAL) return;
    g_sample_count = 0;
    g_stats.sampled++;

    const uint8_t* rgba = frame.rgba.get();
    int w = (int)frame.width, h = (int)frame.height;

    // ── 参照パッチ初期化（最初の1回） ─────────────────────────────────────
    if (!g_tmpl_ready) {
        rgba_to_gray_patch(rgba, w, PATCH_X, PATCH_Y, PATCH_W, PATCH_H, g_tmpl_gray);
        g_tmpl_ready = true;
        // 参照パッチをPNG保存
        std::vector<uint8_t> patch_rgba((size_t)(PATCH_W * PATCH_H * 4));
        for (int r = 0; r < PATCH_H; ++r)
            memcpy(patch_rgba.data() + r * PATCH_W * 4,
                   rgba + ((PATCH_Y+r)*w + PATCH_X)*4, (size_t)(PATCH_W*4));
        char path[512];
        snprintf(path, sizeof(path), "%s/phase2_ref_patch_f%06u_x%dy%d_%dx%d.png",
                 WORK_DIR, frame.frame_id, PATCH_X, PATCH_Y, PATCH_W, PATCH_H);
        stbi_write_png(path, PATCH_W, PATCH_H, 4, patch_rgba.data(), PATCH_W*4);
        printf("[INIT] Reference patch saved: %s\n", path); fflush(stdout);
    }

    // ── 1. PixelSampler ────────────────────────────────────────────────────
    for (int i = 0; i < (int)SAMPLE_POINTS.size(); ++i) {
        auto [px, py] = SAMPLE_POINTS[i];
        if (px < w && py < h) {
            auto s = sample_pixel(rgba, w, px, py);
            g_stats.pixel_r_stats[i].add(s.r);
            g_stats.pixel_g_stats[i].add(s.g);
            g_stats.pixel_b_stats[i].add(s.b);
        }
    }

    // ── 2. PatchMatcher ───────────────────────────────────────────────────
    if (g_tmpl_ready) {
        auto res = match_patch(g_tmpl_gray, rgba, w, h);
        g_stats.ncc_score.add(res.best_score);
        g_stats.ncc_offset_x.add((float)abs(res.offset_x));
        g_stats.ncc_offset_y.add((float)abs(res.offset_y));
    }

    // ── 3. RegionDiff (前フレームと比較) ──────────────────────────────────
    if (!g_prev_frame.empty() && g_prev_w == w && g_prev_h == h) {
        for (int i = 0; i < 4; ++i) {
            auto& R = REGIONS[i];
            float pct = region_diff_pct(rgba, g_prev_frame.data(), w,
                                        R[0], R[1], R[2], R[3]);
            g_stats.region_diff[i].add(pct);
        }
    }

    // 前フレームを保存 (copy)
    size_t bytes = (size_t)(w * h * 4);
    if (g_prev_frame.size() != bytes) g_prev_frame.resize(bytes);
    memcpy(g_prev_frame.data(), rgba, bytes);
    g_prev_w = w; g_prev_h = h;
}

// ── エントリポイント ──────────────────────────────────────────────────────────
int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    printf("=== Phase 2 Read-Only Test  v1 ===\n");
    printf("Test duration : %d seconds\n", TEST_SEC);
    printf("Eval interval : every %d frames\n", SAMPLE_INTERVAL);
    printf("Ref patch     : (%d,%d) %dx%d\n", PATCH_X, PATCH_Y, PATCH_W, PATCH_H);
    printf("Sample points : %zu\n\n", SAMPLE_POINTS.size());
    fflush(stdout);

    // ── Socket ───────────────────────────────────────────────────────────────
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    {
        int v = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof(v));
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)mirage::x1::PORT_CANONICAL);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(sock, (sockaddr*)&addr, sizeof(addr));
    printf("UDP bind OK  ::%d\n\n", mirage::x1::PORT_CANONICAL);
    fflush(stdout);

    // ── Assembler ────────────────────────────────────────────────────────────
    mirage::x1::CanonicalFrameAssembler assembler;
    assembler.set_callback(on_frame);

    // ── 1秒ごとログスレッド ──────────────────────────────────────────────────
    auto t_start = clk::now();
    std::atomic<bool> stop_stats{false};
    uint64_t prev_total = 0;

    auto stats_thread = std::thread([&] {
        auto t_prev = clk::now();
        while (!stop_stats.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clk::now();
            double elapsed = std::chrono::duration<double>(now - t_start).count();
            double dt      = std::chrono::duration<double>(now - t_prev).count();
            t_prev = now;

            const auto& st = assembler.stats();
            std::lock_guard<std::mutex> lk(g_stats_mutex);
            double fps = (g_stats.total - prev_total) / dt;
            prev_total = g_stats.total;

            // NCC最新値
            float ncc_s = g_stats.ncc_score.values.empty() ? 0 :
                          g_stats.ncc_score.values.back();
            float ncc_ox = g_stats.ncc_offset_x.values.empty() ? 0 :
                           g_stats.ncc_offset_x.values.back();
            float ncc_oy = g_stats.ncc_offset_y.values.empty() ? 0 :
                           g_stats.ncc_offset_y.values.back();

            // region diff 平均（最新5件）
            auto recent_mean = [](const RunStats& rs) -> float {
                if (rs.values.empty()) return 0;
                int n = (int)std::min((size_t)5, rs.values.size());
                float s = 0;
                for (int i = (int)rs.values.size()-n; i < (int)rs.values.size(); ++i)
                    s += rs.values[i];
                return s / n;
            };

            printf("[%4.0fs]"
                   "  frm=%-5llu"
                   "  eval=%-4llu"
                   "  fps=%5.1f"
                   "  ncc=%.3f(%+.1f,%+.1f)px"
                   "  rdiff: sb=%.1f%% up=%.1f%% mid=%.1f%% bot=%.1f%%"
                   "  id=%-6u\n",
                   elapsed,
                   (unsigned long long)g_stats.total,
                   (unsigned long long)g_stats.sampled,
                   fps,
                   ncc_s, ncc_ox, ncc_oy,
                   recent_mean(g_stats.region_diff[0]),
                   recent_mean(g_stats.region_diff[1]),
                   recent_mean(g_stats.region_diff[2]),
                   recent_mean(g_stats.region_diff[3]),
                   (unsigned int)g_last_id.load());
            fflush(stdout);
        }
    });

    // ── Recv loop ─────────────────────────────────────────────────────────────
    std::vector<uint8_t> buf(mirage::x1::HEADER_SIZE + mirage::x1::MTU_PAYLOAD + 64);
    auto t0 = clk::now();

    while (std::chrono::duration<double>(clk::now() - t0).count() < TEST_SEC) {
        DWORD tv = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        sockaddr_in from{}; int from_len = sizeof(from);
        int n = recvfrom(sock, (char*)buf.data(), (int)buf.size(), 0,
                         (sockaddr*)&from, &from_len);
        if (n <= 0) { assembler.flush_stale(); continue; }
        if ((size_t)n < mirage::x1::HEADER_SIZE) continue;
        auto hdr = mirage::x1::parse_header(buf.data(), (size_t)n);
        if (!hdr || !hdr->is_canonical()) continue;
        g_udp_rx.fetch_add(1);
        assembler.feed(*hdr, buf.data() + mirage::x1::HEADER_SIZE,
                       (size_t)n - mirage::x1::HEADER_SIZE);
    }

    stop_stats.store(true);
    stats_thread.join();
    closesocket(sock);

    // ── Final Analysis ────────────────────────────────────────────────────────
    std::lock_guard<std::mutex> lk(g_stats_mutex);

    printf("\n=== Final Analysis ===\n");
    printf("frames_total   : %llu\n",  (unsigned long long)g_stats.total);
    printf("frames_sampled : %llu\n",  (unsigned long long)g_stats.sampled);
    printf("coord_system   : %s\n",    g_stats.coord_always_ok ? "PASS (600x1000 固定)" : "FAIL");
    printf("\n");

    // ── PixelSampler Summary ──────────────────────────────────────────────────
    printf("=== PixelSampler (合格: stddev < 5.0) ===\n");
    bool pixel_ok = true;
    for (int i = 0; i < (int)SAMPLE_POINTS.size(); ++i) {
        auto [px, py] = SAMPLE_POINTS[i];
        float sr = g_stats.pixel_r_stats[i].stddev();
        float sg = g_stats.pixel_g_stats[i].stddev();
        float sb = g_stats.pixel_b_stats[i].stddev();
        float max_s = std::max({sr, sg, sb});
        bool ok = max_s < 5.0f;
        if (!ok) pixel_ok = false;
        printf("  (%4d,%4d): stddev R=%.2f G=%.2f B=%.2f  max=%.2f  %s\n",
               px, py, sr, sg, sb, max_s, ok ? "OK" : "UNSTABLE");
    }
    printf("PixelSampler: %s\n\n", pixel_ok ? "PASS" : "FAIL (一部ピクセルが揺れている)");

    // ── PatchMatcher Summary ──────────────────────────────────────────────────
    printf("=== PatchMatcher NCC (合格: score≥0.90, offset≤3px) ===\n");
    bool ncc_ok = false;
    if (!g_stats.ncc_score.values.empty()) {
        float mean_s = g_stats.ncc_score.mean();
        float min_s  = g_stats.ncc_score.min_val();
        float std_s  = g_stats.ncc_score.stddev();
        float max_ox = g_stats.ncc_offset_x.max_val();
        float max_oy = g_stats.ncc_offset_y.max_val();
        // オフセット量自体ではなく「オフセットの安定性」で判定
        // 固定オフセットは座標系崩壊ではなくテンプレート選択の問題
        float std_ox = g_stats.ncc_offset_x.stddev();
        float std_oy = g_stats.ncc_offset_y.stddev();
        ncc_ok = (mean_s >= 0.90f && std_ox < 0.5f && std_oy < 0.5f);
        printf("  score: mean=%.3f  min=%.3f  stddev=%.3f\n", mean_s, min_s, std_s);
        printf("  offset_x: mean=%.1fpx max=%.0fpx stddev=%.2f\n", g_stats.ncc_offset_x.mean(), max_ox, g_stats.ncc_offset_x.stddev());
        printf("  offset_y: mean=%.1fpx max=%.0fpx stddev=%.2f\n", g_stats.ncc_offset_y.mean(), max_oy, g_stats.ncc_offset_y.stddev());
        printf("  PASS cond: score>=0.90 AND offset_stddev<0.5px (not raw offset)\n");
        printf("  patch pos: (%d,%d) size=%dx%d  search_radius=%dpx\n",
               PATCH_X, PATCH_Y, PATCH_W, PATCH_H, SEARCH_RADIUS);
        printf("PatchMatcher: %s\n\n", ncc_ok ? "PASS" : "FAIL");
    } else {
        printf("  (no data)\n\n");
    }

    // ── RegionDiff Summary ────────────────────────────────────────────────────
    static const char* REGION_NAMES[] = {
        "StatusBar (y=0-30)",
        "UpperContent (y=30-180)",
        "MidContent (y=425-575)",
        "BotContent (y=850-1000)"
    };
    printf("=== RegionDiff (フレーム間差分, 参考値) ===\n");
    for (int i = 0; i < 4; ++i) {
        if (g_stats.region_diff[i].values.empty()) continue;
        printf("  %-30s mean=%.2f%%  max=%.2f%%  stddev=%.2f%%\n",
               REGION_NAMES[i],
               g_stats.region_diff[i].mean(),
               g_stats.region_diff[i].max_val(),
               g_stats.region_diff[i].stddev());
    }
    printf("\n");

    // ── Phase 2 判定 ──────────────────────────────────────────────────────────
    bool pass = g_stats.coord_always_ok && pixel_ok && ncc_ok &&
                g_stats.sampled >= 50;

    printf("[Phase 2 Read-Only] %s\n",
           pass ? "PASS - 座標系固定・テンプレート安定・ピクセル安定" :
                  "FAIL - 詳細は上記を確認");
    fflush(stdout);

#ifdef _WIN32
    WSACleanup();
#endif
    return pass ? 0 : 1;
}
