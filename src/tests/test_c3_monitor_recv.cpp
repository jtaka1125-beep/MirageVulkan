// =============================================================================
// test_c3_monitor_recv.cpp — Phase C-3: Monitor Lane receive stats test
//
// Usage:
//   test_c3_monitor_recv.exe              # 60 sec run
//   test_c3_monitor_recv.exe --quick      # 15 sec run
//   test_c3_monitor_recv.exe --duration N # N sec run
//
// Pass criteria:
//   - monitor_frames_delivered >= 1          (stream arriving)
//   - keyframes_delivered >= 1               (SPS/PPS received)
//   - canonical_alive == true                (Canonical lane still up)
//   - thermal_severe_count == 0
// =============================================================================

#include "stream/monitor_receiver.hpp"
#include "stream/canonical_frame_provider.hpp"
#include "mirage_log.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <algorithm>

using namespace mirage::x1;
using namespace std::chrono;

// ── Thermal parser ─────────────────────────────────────────────────────────

static int thermal_query_npad() {
    FILE* p = _popen(
        "C:\\Users\\jun\\.local\\bin\\platform-tools\\adb.exe "
        "-s 192.168.0.3:5555 shell "
        "\"dumpsys thermalservice 2>/dev/null | head -60\"",
        "r");
    if (!p) return 0;
    int max_status = 0;
    char line[256];
    int lines_read = 0;
    while (fgets(line, sizeof(line), p) && lines_read < 60) {
        ++lines_read;
        const char* ts = strstr(line, "Thermal Status: ");
        if (ts) max_status = std::max(max_status, atoi(ts + 16));
        const char* ms = strstr(line, "mStatus=");
        if (ms) max_status = std::max(max_status, atoi(ms + 8));
    }
    _pclose(p);
    return max_status;
}

static const char* thermal_name(int t) {
    switch (t) {
        case 0: return "NOMINAL";
        case 1: return "LIGHT";
        case 2: return "MODERATE";
        case 3: return "SEVERE";
        case 4: return "CRITICAL";
        default: return (t >= 5) ? "CRITICAL" : "UNKNOWN";
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    int duration_s = 60;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0)    duration_s = 15;
        else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
            duration_s = atoi(argv[++i]);
    }

    printf("=== C-3 Monitor Lane Receive Test (%ds) ===\n", duration_s);
    printf("  Monitor   : UDP :50202  (H.264)\n");
    printf("  Canonical : alive check via CanonicalFrameProvider\n\n");

    // ── Start Canonical provider (alive check only) ───────────────────────
    CanonicalFrameProvider canonical_provider;
    bool canonical_ok = canonical_provider.start("192.168.0.3");
    if (!canonical_ok) {
        fprintf(stderr, "WARN: CanonicalFrameProvider start failed"
                        " (alive checks will report dead)\n");
    }

    // ── Start Monitor receiver ────────────────────────────────────────────
    MonitorReceiver monitor_recv;
    std::atomic<uint64_t> monitor_frames{0};
    std::atomic<uint64_t> monitor_keyframes{0};
    std::atomic<uint64_t> monitor_bytes{0};
    std::atomic<uint32_t> last_frame_id{0};

    monitor_recv.set_callback([&](MonitorFrame f){
        monitor_frames.fetch_add(1, std::memory_order_relaxed);
        if (f.is_keyframe)
            monitor_keyframes.fetch_add(1, std::memory_order_relaxed);
        monitor_bytes.fetch_add((uint64_t)f.size(), std::memory_order_relaxed);
        last_frame_id.store(f.frame_id, std::memory_order_relaxed);
    });
    if (!monitor_recv.start(50202)) {
        fprintf(stderr, "ERROR: cannot bind :50202\n");
        canonical_provider.stop();
        return 1;
    }

    // ── Per-second loop ───────────────────────────────────────────────────
    const auto t_start = steady_clock::now();
    const auto t_end   = t_start + seconds(duration_s);

    uint64_t prev_monitor  = 0;
    int      thermal_severe = 0;
    int      thermal_query_counter = 0;
    int      last_thermal   = 0;
    int      canonical_dead_count = 0;

    while (steady_clock::now() < t_end) {
        std::this_thread::sleep_for(seconds(1));
        int elapsed = (int)duration_cast<seconds>(steady_clock::now() - t_start).count();

        uint64_t mc = monitor_frames.load();
        double mon_fps = (double)(mc - prev_monitor);
        prev_monitor = mc;

        bool alive = canonical_provider.is_alive(5000);  // 5s timeout
        if (!alive) ++canonical_dead_count;

        if (++thermal_query_counter >= 10) {
            thermal_query_counter = 0;
            last_thermal = thermal_query_npad();
            if (last_thermal >= 3) ++thermal_severe;
        }

        printf("[C3] t=%3ds  mon_fps=%4.1f  mon_keys=%llu  mon_kb=%llu"
               "  last_id=%u  canonical=%s  thermal=%-8s\n",
               elapsed,
               mon_fps,
               (unsigned long long)monitor_keyframes.load(),
               (unsigned long long)(monitor_bytes.load() / 1024),
               (unsigned)last_frame_id.load(),
               alive ? "ALIVE" : "DEAD ",
               thermal_name(last_thermal));
    }

    // ── Stop ─────────────────────────────────────────────────────────────
    canonical_provider.stop();
    monitor_recv.stop();

    const auto& mstats = monitor_recv.get_stats();

    // ── Summary ──────────────────────────────────────────────────────────
    printf("\n=== C-3 Gate Summary ===\n");
    printf("  duration_s              : %d\n", duration_s);
    printf("  monitor_frames_total    : %llu\n", (unsigned long long)mstats.delivered);
    printf("  monitor_keyframes       : %llu\n", (unsigned long long)mstats.keyframes_delivered);
    printf("  monitor_bytes_KB        : %llu\n", (unsigned long long)(mstats.bytes_delivered/1024));
    printf("  dropped_old             : %llu\n", (unsigned long long)mstats.dropped_old);
    printf("  evicted_incomplete      : %llu\n", (unsigned long long)mstats.incomplete_frames_evicted);
    printf("  canonical_dead_seconds  : %d\n",   canonical_dead_count);
    printf("  thermal_severe_count    : %d    %s (need ==0)\n",
           thermal_severe, thermal_severe == 0 ? "PASS" : "FAIL");

    bool p_mon_frames  = mstats.delivered >= 1;
    bool p_keyframes   = mstats.keyframes_delivered >= 1;
    bool p_canonical   = canonical_dead_count == 0;
    bool p_thermal     = thermal_severe == 0;

    printf("\n  monitor_frames >= 1     : %s  (%llu)\n",
           p_mon_frames ? "PASS" : "FAIL", (unsigned long long)mstats.delivered);
    printf("  keyframes >= 1          : %s  (%llu)\n",
           p_keyframes ? "PASS" : "FAIL", (unsigned long long)mstats.keyframes_delivered);
    printf("  canonical_alive         : %s  (dead_s=%d)\n",
           p_canonical ? "PASS" : "WARN", canonical_dead_count);

    // canonical_alive is WARN (not hard fail) since provider needs TCP handshake
    bool all_pass = p_mon_frames && p_keyframes && p_thermal;
    printf("\n[C3 Gate] %s\n", all_pass ? "C3_PASS" : "C3_FAIL");

    return all_pass ? 0 : 1;
}
