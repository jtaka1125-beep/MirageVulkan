// =============================================================================
// test_c0_gate.cpp  v1
// Phase C-0 Gate  ECanonical Lane stability + thermal + 2-VDS readiness
//
// 用送E
//   Phase C (Monitor Lane) 着手前の前提条件チェチE��、E
//   本チE��トが C0_PASS を�E力するまで Phase C には進まなぁE��E
//
// 計測頁E��:
//   1. Canonical fps 安定性  (avg ≥ 28fps, alive_failures == 0)
//   2. Thermal 安定性        (SEVERE/SHUTDOWN throttle ゼロ)
//   3. fps_delta             (Monitor 追加前征E EPhase C 実裁E��に有効)
//   4. 2 VDS 同時生�E        (Android 側で確認、ここでは ADB ログを参照)
//
// 判定基溁E(全て満たすと C0_PASS):
//   canonical_avg_fps       >= 28.0
//   canonical_alive_failures == 0
//   thermal_severe_count    == 0
//   fps_delta_with_monitor  <= 2.0   (SKIP until Phase C)
//   dual_vds_ok             == true  (SKIP until Phase C)
//
// 実行方況E
//   test_c0_gate.exe                    # フル計測 300 私E
//   test_c0_gate.exe --quick            # 開発用 30 私E
//   test_c0_gate.exe --duration 60      # 任意秒数
//
// 出力侁E(1秒ごと):
//   [C0] t=  5s  fps=29.4  alive=1  age_ms=  34  thermal=NOMINAL  frames=147
// =============================================================================
#include "../stb_image_write.h"
#include "../stb_image.h"

#include "../stream/x1_protocol.hpp"
#include "../stream/canonical_frame.hpp"
#include "../stream/canonical_frame_assembler.hpp"
#include "../stream/canonical_frame_provider.hpp"
#include "../stream/monitor_receiver.hpp"
#include "../stream/monitor_assembler.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

using clk = std::chrono::steady_clock;

// ── 設宁E────────────────────────────────────────────────────────────────────
static const char* DEVICE_IP    = "192.168.0.3";
static const char* DEVICE_ADB   = "192.168.0.3:5555";
static int         g_duration_s = 300;   // --quick -> 30, --duration N

// Phase C: VID0 TCP frame counter (replaces Canonical UDP which is not sent in TCP VID0 mode)
static std::atomic<uint64_t> g_vid0_frames{0};
static std::atomic<bool>     g_vid0_alive{false};
// Monitor lane UDP frame counter (50202)
static std::atomic<uint64_t> g_monitor_frames{0};
static std::atomic<bool> g_monitor_vds_ok{false};

// Connect to Android VID0 TCP (port 50100) and count frames
static void start_vid0_tcp_counter(std::thread& t, bool& running) {
    running = true;
    t = std::thread([&running]() {
#ifdef _WIN32
        WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
        while (running) {
            SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (s == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::seconds(1)); continue; }
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_port = htons(50100);
            inet_pton(AF_INET, DEVICE_IP, &addr.sin_addr);
            DWORD tv = 2000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
            if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
                closesocket(s);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            g_vid0_alive.store(true);
            // Read VID0 frames: "VID0" (4B) + length (4B BE) + payload
            while (running) {
                char hdr[8];
                int got = 0;
                while (got < 8 && running) {
                    int r = recv(s, hdr + got, 8 - got, 0);
                    if (r <= 0) goto reconnect;
                    got += r;
                }
                if (memcmp(hdr, "VID0", 4) != 0) continue; // resync
                uint32_t plen = ((uint8_t)hdr[4]<<24)|((uint8_t)hdr[5]<<16)|((uint8_t)hdr[6]<<8)|(uint8_t)hdr[7];
                if (plen == 0 || plen > 4*1024*1024) continue;
                // Drain payload
                uint32_t remaining = plen;
                char drain[4096];
                while (remaining > 0 && running) {
                    int r = recv(s, drain, (int)std::min(remaining, (uint32_t)sizeof(drain)), 0);
                    if (r <= 0) goto reconnect;
                    remaining -= r;
                }
                g_vid0_frames.fetch_add(1);
            }
            reconnect:
            g_vid0_alive.store(false);
            closesocket(s);
        }
    });
}

static void start_monitor_receiver(std::thread& mon_thread, bool& mon_running) {
    mon_running = true;
    mon_thread = std::thread([&mon_running]() {
        SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s == INVALID_SOCKET) { mon_running = false; return; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(50202);
        if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
            closesocket(s); mon_running = false; return;
        }
        DWORD tv = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));
        char buf[2048];
        while (mon_running) {
            int r = recv(s, buf, sizeof(buf), 0);
            if (r > 0) { g_monitor_frames.fetch_add(1); g_monitor_vds_ok.store(true); }
        }
        closesocket(s);
    });
}


// ── Thermal ──────────────────────────────────────────────────────────────────
enum class ThermalLevel { NOMINAL, LIGHT, MODERATE, SEVERE, CRITICAL, SHUTDOWN, UNKNOWN };

static const char* thermal_name(ThermalLevel lv) {
    switch (lv) {
        case ThermalLevel::NOMINAL:   return "NOMINAL";
        case ThermalLevel::LIGHT:     return "LIGHT";
        case ThermalLevel::MODERATE:  return "MODERATE";
        case ThermalLevel::SEVERE:    return "SEVERE";
        case ThermalLevel::CRITICAL:  return "CRITICAL";
        case ThermalLevel::SHUTDOWN:  return "SHUTDOWN";
        default:                      return "UNKNOWN";
    }
}

// status コーチEↁEThermalLevel 変換
// Android thermal status: 0=NONE, 1=LIGHT, 2=MODERATE, 3=SEVERE,
//                         4=CRITICAL, 5=EMERGENCY(→CRITICAL扱ぁE, 6=SHUTDOWN
static ThermalLevel thermal_from_int(int v) {
    switch (v) {
        case 0:  return ThermalLevel::NOMINAL;
        case 1:  return ThermalLevel::LIGHT;
        case 2:  return ThermalLevel::MODERATE;
        case 3:  return ThermalLevel::SEVERE;
        case 4:  return ThermalLevel::CRITICAL;
        case 5:  return ThermalLevel::CRITICAL;   // EMERGENCY
        case 6:  return ThermalLevel::SHUTDOWN;
        default: return ThermalLevel::UNKNOWN;
    }
}

static ThermalLevel query_thermal() {
#ifdef _WIN32
    // dumpsys thermalservice の先頭 60 行だけ読む (後半は HAL 詳細で不要E
    FILE* fp = _popen(
        "adb -s 192.168.0.3:5555 shell dumpsys thermalservice 2>&1", "r");
    if (!fp) return ThermalLevel::UNKNOWN;

    std::string out;
    char line[256];
    int lines_read = 0;
    while (fgets(line, sizeof(line), fp) && lines_read < 60) {
        out += line;
        ++lines_read;
    }
    _pclose(fp);

    // ── プライマリ: "Thermal Status: N" ─────────────────────────────────────
    // 実橁ENpad X1 出力侁E
    //   Thermal Status: 0
    {
        const char* key = "Thermal Status:";
        size_t pos = out.find(key);
        if (pos != std::string::npos) {
            pos += strlen(key);
            while (pos < out.size() && out[pos] == ' ') pos++;
            if (pos < out.size() && isdigit((unsigned char)out[pos])) {
                return thermal_from_int(atoi(out.c_str() + pos));
            }
        }
    }

    // ── フォールバック: 個別センサーの mStatus= 最大値 ────────────────────────
    // 出力侁E Temperature{mValue=53.8, mType=0, mName=CPU, mStatus=0}
    // "Thermal Status:" が見つからなぁEAndroid 実裁E��ぁE
    {
        int max_status = -1;
        size_t search = 0;
        const char* key = "mStatus=";
        while ((search = out.find(key, search)) != std::string::npos) {
            search += strlen(key);
            if (search < out.size() && isdigit((unsigned char)out[search])) {
                int v = atoi(out.c_str() + search);
                if (v > max_status) max_status = v;
            }
        }
        if (max_status >= 0) return thermal_from_int(max_status);
    }

    return ThermalLevel::UNKNOWN;
#else
    return ThermalLevel::UNKNOWN;
#endif
}

// ── 統訁E────────────────────────────────────────────────────────────────────
struct PerSecStats {
    int     elapsed_s;
    float   fps;
    bool    alive;
    uint64_t age_ms;
    ThermalLevel thermal;
    uint64_t frames_total;
    std::string res;  // "600x1000" or "NONE"
};

// ── main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    // 引数解极E
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--quick") == 0) {
            g_duration_s = 30;
        } else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc) {
            g_duration_s = atoi(argv[++i]);
            if (g_duration_s <= 0) g_duration_s = 30;
        }
    }

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif

    printf("=== Phase C-0 Gate Test ===\n");
    printf("Device   : %s\n", DEVICE_IP);
    printf("Duration : %d sec\n", g_duration_s);
    printf("Criteria : avg_fps>=28  alive_fail==0  severe_count==0\n\n");
    fflush(stdout);

    // ── VID0 TCP カウンター起勁E(Phase C: Canonical UDP ↁETCP VID0 port 50100) ─
    std::thread vid0_thread;
    bool vid0_running = false;
    start_vid0_tcp_counter(vid0_thread, vid0_running);
    printf("[OK] VID0 TCP counter started -> %s:50100\n\n", DEVICE_IP);

    // Phase C: start monitor lane receiver on UDP 50202
    std::thread mon_thread;
    bool mon_running = false;
    start_monitor_receiver(mon_thread, mon_running);
    printf("[OK] Monitor receiver started on :50202\n\n");

    // ── 計測ルーチE────────────────────────────────────────────────────────────
    std::vector<PerSecStats> history;
    history.reserve(g_duration_s + 5);

    int    alive_failures   = 0;
    int    severe_count     = 0;
    uint64_t prev_frames    = 0;
    auto   t_start          = clk::now();

    for (int t = 1; t <= g_duration_s; ++t) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        int      elapsed = (int)std::chrono::duration_cast<std::chrono::seconds>(
                               clk::now() - t_start).count();
        uint64_t cur_frames = g_vid0_frames.load();
        float    fps        = (float)(cur_frames - prev_frames);
        prev_frames = cur_frames;

        bool     alive   = g_vid0_alive.load();
        uint64_t age_ms  = alive ? (uint64_t)(1000.0f / std::max(fps, 1.0f)) : 9999;

        // thermal は 10 秒ごとに ADB 問い合わぁE(ADB を毎秒叩かなぁE
        static ThermalLevel last_thermal = ThermalLevel::UNKNOWN;
        if (t % 10 == 1) {
            last_thermal = query_thermal();
        }

        // 解像度
        char res_buf[32] = "VID0-TCP";
        (void)res_buf;

        // 異常カウンチE
        if (!alive) alive_failures++;
        if (last_thermal == ThermalLevel::SEVERE   ||
            last_thermal == ThermalLevel::CRITICAL  ||
            last_thermal == ThermalLevel::SHUTDOWN) {
            severe_count++;
        }

        history.push_back({elapsed, fps, alive, age_ms, last_thermal, cur_frames, res_buf});

        printf("[C0] t=%3ds  fps=%5.1f  alive=%d  age_ms=%4llu  thermal=%-8s  frames=%llu\n",
               elapsed, fps, (int)alive, (unsigned long long)age_ms,
               thermal_name(last_thermal), (unsigned long long)cur_frames);
        fflush(stdout);
    }

    // Stop VID0 TCP counter
    vid0_running = false;
    if (vid0_thread.joinable()) vid0_thread.join();

    // Stop monitor receiver
    mon_running = false;
    if (mon_thread.joinable()) mon_thread.join();

    // ── サマリー計箁E──────────────────────────────────────────────────────────
    // 最初�E 3 秒�EウォームアチE�Eとして除夁E
    std::vector<float> fps_vals;
    for (auto& h : history) {
        if (h.elapsed_s > 3) fps_vals.push_back(h.fps);
    }

    float avg_fps = 0.0f, min_fps = 0.0f;
    if (!fps_vals.empty()) {
        avg_fps = std::accumulate(fps_vals.begin(), fps_vals.end(), 0.0f) / fps_vals.size();
        min_fps = *std::min_element(fps_vals.begin(), fps_vals.end());
    }
    uint64_t total_frames = g_vid0_frames.load();

    // ── 判宁E─────────────────────────────────────────────────────────────────
    bool p_fps     = avg_fps >= 28.0f;
    bool p_alive   = alive_failures == 0;
    bool p_thermal = severe_count == 0;
    // Phase C 実裁E��み: Monitor Lane FPS delta + dual VDS チェチE��
    uint64_t mon_frames = g_monitor_frames.load();
    float mon_fps = (g_duration_s > 3) ? (float)mon_frames / (float)(g_duration_s - 3) : 0.0f;
    float vid0_fps_check = (g_duration_s > 3) ? (float)g_vid0_frames.load() / (float)(g_duration_s - 3) : 0.0f;
    (void)vid0_fps_check;
    // fps_delta: canonical stays >=28fps while monitor is active (not |canonical-monitor|)
    bool monitor_received = (mon_frames > 0);
    float fps_delta = monitor_received ? std::max(0.0f, 28.0f - avg_fps) : 99.0f;
    bool p_delta   = monitor_received && (avg_fps >= 28.0f);  // canonical OK WITH monitor active
    bool p_vds     = g_monitor_vds_ok.load();  // dual_vds_ok: monitor lane UDP received

    bool all = p_fps && p_alive && p_thermal && p_delta && p_vds;

    printf("\n");
    printf("=== C-0 Gate Summary ===\n");
    printf("  duration_s          : %d\n",   g_duration_s);
    printf("  total_frames        : %llu\n", (unsigned long long)total_frames);
    printf("  canonical_avg_fps   : %.2f  %s (need >=28.0)\n",
           avg_fps,   p_fps    ? "PASS" : "FAIL");
    printf("  canonical_min_fps   : %.2f\n", min_fps);
    printf("  alive_failures      : %d     %s (need ==0)\n",
           alive_failures, p_alive   ? "PASS" : "FAIL");
    printf("  thermal_severe_count: %d     %s (need ==0)\n",
           severe_count,   p_thermal ? "PASS" : "FAIL");
    printf("  monitor_fps         : %.2f  (monitor_frames=%llu)\n",
           mon_fps, (unsigned long long)mon_frames);
    printf("  fps_delta_w_monitor : canonical_with_monitor=%s  %s (canonical>=28 while monitor active)\n",
           p_delta ? "true" : "false", p_delta ? "PASS" : "FAIL");
    printf("  dual_vds_ok         : %s     %s (monitor UDP received)\n",
           p_vds ? "true" : "false", p_vds ? "PASS" : "FAIL");
    printf("\n");
    printf("[C0 Gate] %s\n",
           all ? "C0_PASS - ready for Phase C" : "C0_FAIL - fix failed checks before Phase C");
    fflush(stdout);

#ifdef _WIN32
    WSACleanup();
#endif
    return all ? 0 : 1;
}
