// =============================================================================
// test_c5_monitor_view.cpp — Phase C-5: Monitor Lane → RGBA → texture upload path
//
// Pipeline:
//   MonitorLaneClient (UDP :50202) → H264Decoder → RgbaCallback
//   → stageMonitorFrame() proxy counter → texture upload simulated
//
// This test validates the full C-5 display path WITHOUT a live GUI window.
// It uses MonitorLaneClient directly and counts RGBA callbacks,
// confirming the path that stageMonitorFrame would traverse.
//
// Usage:
//   test_c5_monitor_view.exe              # 30s
//   test_c5_monitor_view.exe --quick      # 15s
//   test_c5_monitor_view.exe --duration N
//
// Pass criteria (C-5 gate):
//   client_alive      : MonitorLaneClient::start() returned true
//   recv_ok           : frames_recv >= 1
//   decoded_ok        : frames_decoded >= 1
//   rgba_ok           : rgba_calls >= 1
//   stage_called      : rgba_calls >= 10  (callback fired repeatedly)
//   resolution_ok     : last_width==720 && last_height==1200
//   continuity_ok     : rgba_calls >= 0.9 * frames_decoded  (< 10% drop)
//   fps_ok            : avg rgba fps >= 28.0
// =============================================================================

#include "stream/monitor_lane_client.hpp"
#include "mirage_log.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace mirage::x1;
using namespace std::chrono;

// ── RGBA frame sample capture ───────────────────────────────────────────────

struct FrameSample {
    int     width  = 0;
    int     height = 0;
    bool    non_black = false;  // true if any pixel != 0
    uint64_t pts_us = 0;
};

static std::vector<FrameSample> g_samples;
static std::mutex               g_samples_mutex;
static std::atomic<int>         g_last_w{0};
static std::atomic<int>         g_last_h{0};

// ── fps tracker ────────────────────────────────────────────────────────────

static double measure_fps(const std::vector<double>& fps_samples) {
    if (fps_samples.empty()) return 0.0;
    double s = 0;
    for (auto v : fps_samples) s += v;
    return s / fps_samples.size();
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int duration_s = 30;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--quick"))                      duration_s = 15;
        else if (!strcmp(argv[i], "--duration") && i+1<argc) duration_s = atoi(argv[++i]);
    }

    printf("=== C-5 Monitor View Test (%ds) ===\n", duration_s);
    printf("  MonitorLaneClient :50202 → H264Decoder → RgbaCallback\n\n");

    // ── Build client ─────────────────────────────────────────────────────
    MonitorLaneClient client;
    client.set_device_id("test_c5_device");

    // per-second fps tracking
    std::atomic<uint64_t> rgba_this_sec{0};
    std::vector<double>   fps_samples;

    client.set_callback(
        [&](const std::string& /*id*/, const uint8_t* rgba, int w, int h, uint64_t pts) {
            rgba_this_sec++;
            g_last_w.store(w);
            g_last_h.store(h);

            // Capture first few samples for non-black check
            {
                std::lock_guard<std::mutex> lk(g_samples_mutex);
                if (g_samples.size() < 5) {
                    FrameSample s;
                    s.width  = w;
                    s.height = h;
                    s.pts_us = pts;
                    // Sample a few pixels for non-black check
                    size_t stride = (size_t)w * 4;
                    for (int py = 0; py < h && !s.non_black; py += h/8+1) {
                        for (int px = 0; px < w && !s.non_black; px += w/8+1) {
                            const uint8_t* p = rgba + py * stride + px * 4;
                            if (p[0]|p[1]|p[2]) s.non_black = true;
                        }
                    }
                    g_samples.push_back(s);
                }
            }
        });

    // ── Start ─────────────────────────────────────────────────────────────
    bool started = client.start(50202);
    if (!started) {
        fprintf(stderr, "ERROR: MonitorLaneClient::start(50202) failed\n");
        printf("\n[C5 Gate] C5_FAIL  (client start failed)\n");
        return 1;
    }
    printf("  MonitorLaneClient started (port 50202)\n\n");

    // ── Run for duration_s ────────────────────────────────────────────────
    auto t_start = steady_clock::now();
    auto t_next_sec = t_start + seconds(1);

    printf("  sec  recv    decoded  rgba    fps\n");
    printf("  ---  ------  -------  ------  -----\n");

    for (int sec = 0; sec < duration_s; ++sec) {
        std::this_thread::sleep_until(t_next_sec);
        t_next_sec += seconds(1);

        uint64_t rps = rgba_this_sec.exchange(0);
        fps_samples.push_back((double)rps);

        printf("  %3d  %-6llu  %-7llu  %-6llu  %.1f\n",
               sec+1,
               (unsigned long long)client.frames_recv(),
               (unsigned long long)client.frames_decoded(),
               (unsigned long long)client.rgba_calls(),
               (double)rps);
        fflush(stdout);
    }

    client.stop();

    // ── Results ───────────────────────────────────────────────────────────
    uint64_t total_recv    = client.frames_recv();
    uint64_t total_decoded = client.frames_decoded();
    uint64_t total_rgba    = client.rgba_calls();
    int      last_w        = g_last_w.load();
    int      last_h        = g_last_h.load();
    // Exclude first second (H.264 SPS/PPS sync — expected low fps)
    std::vector<double> stable_fps = fps_samples.size() > 1
        ? std::vector<double>(fps_samples.begin()+1, fps_samples.end())
        : fps_samples;
    double   avg_fps       = measure_fps(stable_fps);

    // continuity: rgba should be close to decoded (< 10% drop)
    double continuity = (total_decoded > 0)
                        ? (double)total_rgba / (double)total_decoded
                        : 0.0;

    // non-black check
    bool any_non_black = false;
    {
        std::lock_guard<std::mutex> lk(g_samples_mutex);
        for (auto& s : g_samples) if (s.non_black) { any_non_black = true; break; }
    }

    printf("\n--- C-5 Results ---\n");
    printf("  duration_s    : %d\n",   duration_s);
    printf("  recv_frames   : %llu\n", (unsigned long long)total_recv);
    printf("  decoded_frames: %llu\n", (unsigned long long)total_decoded);
    printf("  rgba_calls    : %llu\n", (unsigned long long)total_rgba);
    printf("  last_resolution: %dx%d\n", last_w, last_h);
    printf("  continuity    : %.4f  %s (need >= 0.90)\n",
           continuity, continuity >= 0.90 ? "PASS" : "FAIL");
    printf("  avg_rgba_fps  : %.2f   %s (need >= 28.0, excl. first sec)\n",
           avg_fps, avg_fps >= 28.0 ? "PASS" : "FAIL");
    printf("  non_black     : %s  (informational)\n",
           any_non_black ? "yes" : "no (all-black or no sample)");

    // ── Gate checks ───────────────────────────────────────────────────────
    bool p_alive      = started;
    bool p_recv       = total_recv    >= 1;
    bool p_decoded    = total_decoded >= 1;
    bool p_rgba       = total_rgba    >= 1;
    bool p_stage      = total_rgba    >= 10;  // callback fired repeatedly
    bool p_resolution = (last_w == 720 && last_h == 1200);
    bool p_continuity = continuity    >= 0.90;
    bool p_fps        = avg_fps       >= 28.0;

    printf("\n  client_alive  : %s\n",  p_alive      ? "PASS" : "FAIL");
    printf("  recv_ok       : %s  (%llu)\n",
           p_recv      ? "PASS" : "FAIL", (unsigned long long)total_recv);
    printf("  decoded_ok    : %s  (%llu)\n",
           p_decoded   ? "PASS" : "FAIL", (unsigned long long)total_decoded);
    printf("  rgba_ok       : %s  (%llu)\n",
           p_rgba      ? "PASS" : "FAIL", (unsigned long long)total_rgba);
    printf("  stage_called  : %s  (%llu >= 10)\n",
           p_stage     ? "PASS" : "FAIL", (unsigned long long)total_rgba);
    printf("  resolution_ok : %s  (%dx%d)\n",
           p_resolution? "PASS" : "FAIL", last_w, last_h);
    printf("  continuity_ok : %s  (%.4f)\n",
           p_continuity? "PASS" : "FAIL", continuity);
    printf("  fps_ok        : %s  (%.2f)\n",
           p_fps       ? "PASS" : "FAIL", avg_fps);

    bool all_pass = p_alive && p_recv && p_decoded && p_rgba &&
                    p_stage && p_resolution && p_continuity && p_fps;

    printf("\n[C5 Gate] %s\n", all_pass ? "C5_PASS" : "C5_FAIL");
    return all_pass ? 0 : 1;
}
