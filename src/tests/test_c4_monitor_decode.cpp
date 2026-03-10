// =============================================================================
// test_c4_monitor_decode.cpp — Phase C-4: H.264 software decode confirmation
//
// Pipeline: MonitorReceiver → MonitorDecoder (libavcodec / H.264 SW)
//
// Usage:
//   test_c4_monitor_decode.exe              # 60s
//   test_c4_monitor_decode.exe --quick      # 15s
//   test_c4_monitor_decode.exe --duration N
//
// Pass criteria (C-4 gate):
//   - decode_ok              : decoded_frames >= 1
//   - resolution_ok          : last_width==720 && last_height==1200
//   - fail_ratio_ok          : decode_fail / decoded_frames < 0.01  (< 1%)
//   - keyframe_recovery_ok   : keyframes_decoded >= 1
//   - decode_fps_ok          : avg decode fps >= 28.0
// =============================================================================

#include "stream/monitor_receiver.hpp"
#include "stream/monitor_decoder.hpp"
#include "mirage_log.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace mirage::x1;
using namespace std::chrono;

// ── Thermal ────────────────────────────────────────────────────────────────

static int thermal_query() {
    FILE* p = _popen(
        "C:\\Users\\jun\\.local\\bin\\platform-tools\\adb.exe "
        "-s 192.168.0.3:5555 shell "
        "\"dumpsys thermalservice 2>/dev/null | head -60\"", "r");
    if (!p) return 0;
    int mx = 0; char ln[256]; int n = 0;
    while (fgets(ln, sizeof(ln), p) && n++ < 60) {
        const char* a = strstr(ln, "Thermal Status: ");
        if (a) mx = std::max(mx, atoi(a+16));
        const char* b = strstr(ln, "mStatus=");
        if (b) mx = std::max(mx, atoi(b+8));
    }
    _pclose(p); return mx;
}
static const char* th_name(int t) {
    if (t==0) return "NOMINAL"; if (t==1) return "LIGHT";
    if (t==2) return "MODERATE"; if (t==3) return "SEVERE";
    return (t>=4) ? "CRITICAL" : "UNKNOWN";
}

// ── Main ───────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int duration_s = 60;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--quick"))                      duration_s = 15;
        else if (!strcmp(argv[i], "--duration") && i+1<argc) duration_s = atoi(argv[++i]);
    }

    printf("=== C-4 Monitor Decode Test (%ds) ===\n", duration_s);
    printf("  MonitorReceiver :50202 → MonitorDecoder (libavcodec H.264 SW)\n\n");

    // ── Open decoder ────────────────────────────────────────────────────
    MonitorDecoder decoder;
    if (!decoder.open()) {
        fprintf(stderr, "ERROR: MonitorDecoder::open() failed\n");
        return 1;
    }

    // per-second decoded-frame counter
    std::atomic<uint64_t> decoded_this_sec{0};
    std::atomic<uint64_t> recv_this_sec{0};

    std::atomic<bool>     first_key_seen{false};
    std::atomic<uint64_t> post_key_errors{0};  // errors AFTER first keyframe

    decoder.set_callback([&](MonitorDecoder::DecodedFrame df) {
        decoded_this_sec.fetch_add(1, std::memory_order_relaxed);
        if (df.is_keyframe && !first_key_seen.load())
            first_key_seen.store(true);
    });

    // ── Start receiver ──────────────────────────────────────────────────
    MonitorReceiver receiver;
    receiver.set_callback([&](MonitorFrame f) {
        recv_this_sec.fetch_add(1, std::memory_order_relaxed);
        auto prev_errs = decoder.get_stats().send_errors
                       + decoder.get_stats().recv_errors
                       + decoder.get_stats().parse_errors;
        decoder.decode(f);
        if (first_key_seen.load()) {
            auto new_errs = decoder.get_stats().send_errors
                          + decoder.get_stats().recv_errors
                          + decoder.get_stats().parse_errors;
            if (new_errs > prev_errs)
                post_key_errors.fetch_add(new_errs - prev_errs, std::memory_order_relaxed);
        }
    });

    if (!receiver.start(50202)) {
        fprintf(stderr, "ERROR: cannot bind :50202\n");
        decoder.close();
        return 1;
    }

    // ── Per-second loop ─────────────────────────────────────────────────
    const auto t_start = steady_clock::now();
    const auto t_end   = t_start + seconds(duration_s);

    int    thermal_severe  = 0;
    int    thermal_tick    = 0;
    int    last_thermal    = 0;
    std::vector<double> dec_fps_samples;
    dec_fps_samples.reserve(duration_s);

    while (steady_clock::now() < t_end) {
        std::this_thread::sleep_for(seconds(1));
        int elapsed = (int)duration_cast<seconds>(steady_clock::now()-t_start).count();

        uint64_t d = decoded_this_sec.exchange(0, std::memory_order_relaxed);
        uint64_t r = recv_this_sec.exchange(0, std::memory_order_relaxed);
        if (r > 0) dec_fps_samples.push_back((double)d);

        auto ds = decoder.get_stats();
        auto rs = receiver.get_stats();

        if (++thermal_tick >= 10) {
            thermal_tick = 0;
            last_thermal = thermal_query();
            if (last_thermal >= 3) ++thermal_severe;
        }

        printf("[C4] t=%3ds  recv=%2llu  dec=%2llu  "
               "total_dec=%5llu  keys=%3llu  "
               "err(send=%llu recv=%llu parse=%llu)  "
               "res=%dx%d  thermal=%-8s\n",
               elapsed,
               (unsigned long long)r, (unsigned long long)d,
               (unsigned long long)ds.frames_decoded,
               (unsigned long long)ds.keyframes_decoded,
               (unsigned long long)ds.send_errors,
               (unsigned long long)ds.recv_errors,
               (unsigned long long)ds.parse_errors,
               ds.last_width, ds.last_height,
               th_name(last_thermal));
    }

    // ── Stop ────────────────────────────────────────────────────────────
    receiver.stop();
    decoder.close();

    // ── Final stats ─────────────────────────────────────────────────────
    auto ds = decoder.get_stats();
    auto rs = receiver.get_stats();

    double avg_dec_fps = 0.0;
    if (!dec_fps_samples.empty()) {
        // Skip first sample (startup latency before first keyframe)
        auto begin = dec_fps_samples.size() > 1
                     ? dec_fps_samples.begin() + 1
                     : dec_fps_samples.begin();
        size_t count = dec_fps_samples.end() - begin;
        if (count > 0) {
            avg_dec_fps = std::accumulate(begin, dec_fps_samples.end(), 0.0)
                          / (double)count;
        }
    }

    uint64_t total_errors     = ds.send_errors + ds.recv_errors + ds.parse_errors;
    uint64_t startup_errors   = total_errors - post_key_errors.load();
    uint64_t steady_errors    = post_key_errors.load();
    // fail_ratio uses only post-keyframe errors (startup PPS-not-found is expected)
    double fail_ratio = (ds.frames_decoded > 0)
        ? (double)steady_errors / (double)(ds.frames_decoded + steady_errors) : 1.0;

    printf("\n=== C-4 Gate Summary ===\n");
    printf("  duration_s          : %d\n",  duration_s);
    printf("  recv_frames_total   : %llu\n",(unsigned long long)rs.delivered);
    printf("  decoded_frames      : %llu\n",(unsigned long long)ds.frames_decoded);
    printf("  keyframes_decoded   : %llu\n",(unsigned long long)ds.keyframes_decoded);
    printf("  startup_errors      : %llu  (pre-SPS, expected)\n",(unsigned long long)startup_errors);
    printf("  steady_errors       : %llu  (post-keyframe)\n",(unsigned long long)steady_errors);
    printf("  send_errors_total   : %llu\n",(unsigned long long)ds.send_errors);
    printf("  recv_errors         : %llu\n",(unsigned long long)ds.recv_errors);
    printf("  parse_errors        : %llu\n",(unsigned long long)ds.parse_errors);
    printf("  fail_ratio (steady) : %.4f  %s (need < 0.01)\n",
           fail_ratio, fail_ratio < 0.01 ? "PASS" : "FAIL");
    printf("  last_resolution     : %dx%d  %s (need 720x1200)\n",
           ds.last_width, ds.last_height,
           (ds.last_width==720 && ds.last_height==1200) ? "PASS" : "FAIL");
    printf("  avg_dec_fps         : %.2f   %s (need >=28.0)\n",
           avg_dec_fps, avg_dec_fps >= 28.0 ? "PASS" : "FAIL");
    printf("  thermal_severe      : %d     %s\n",
           thermal_severe, thermal_severe==0 ? "PASS" : "FAIL");

    // Gate
    bool p_decode   = ds.frames_decoded >= 1;
    bool p_res      = (ds.last_width==720 && ds.last_height==1200);
    bool p_fail     = fail_ratio < 0.01;
    bool p_keys     = ds.keyframes_decoded >= 1;
    bool p_fps      = avg_dec_fps >= 28.0;
    bool p_thermal  = thermal_severe == 0;

    printf("\n  decode_ok           : %s  (%llu frames)\n",
           p_decode ? "PASS":"FAIL", (unsigned long long)ds.frames_decoded);
    printf("  resolution_ok       : %s  (%dx%d)\n",
           p_res ? "PASS":"FAIL", ds.last_width, ds.last_height);
    printf("  fail_ratio_ok       : %s  (%.4f)\n",
           p_fail ? "PASS":"FAIL", fail_ratio);
    printf("  keyframe_recovery_ok: %s  (%llu keys)\n",
           p_keys ? "PASS":"FAIL", (unsigned long long)ds.keyframes_decoded);
    printf("  decode_fps_ok       : %s  (%.2f fps)\n",
           p_fps ? "PASS":"FAIL", avg_dec_fps);

    bool all_pass = p_decode && p_res && p_fail && p_keys && p_fps && p_thermal;
    printf("\n[C4 Gate] %s\n", all_pass ? "C4_PASS" : "C4_FAIL");
    return all_pass ? 0 : 1;
}
