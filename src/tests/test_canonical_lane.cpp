// =============================================================================
// test_canonical_lane.cpp  v4.1
// Canonical Lane 単独検証ドライバ
//
// 1秒ごとログ:
//   udp  frames_announced  delivered  jfail  smm  dropped_old
//   incomplete_evicted  unresolved  fps  last_frame_id  last_pts_us
//
// unresolved = frames_announced - delivered - incomplete_frames_evicted
//              - frames_decode_failed - decoded_size_mismatch
//
// PNG: canonical_f<id>_<w>x<h>_<pts_us>.png
// crop PNG: crop_f<id>_x100y100_100x100_crc<CRC32>.png
// =============================================================================
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../stb_image_write.h"

#include "../stream/x1_protocol.hpp"
#include "../stream/canonical_frame.hpp"
#include "../stream/canonical_frame_assembler.hpp"
#include "../mirage_log.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

using clk = std::chrono::steady_clock;

// ── CRC32 (ISO 3309) ──────────────────────────────────────────────────────────
static uint32_t crc32_buf(const uint8_t* data, size_t len) {
    static uint32_t table[256] = {};
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// ── crop helper ───────────────────────────────────────────────────────────────
static std::vector<uint8_t> extract_crop(
    const uint8_t* rgba, int w,
    int cx=100, int cy=100, int cw=100, int ch=100)
{
    std::vector<uint8_t> out((size_t)(cw * ch * 4));
    for (int row = 0; row < ch; ++row)
        memcpy(out.data() + row * cw * 4,
               rgba + ((cy + row) * w + cx) * 4,
               (size_t)(cw * 4));
    return out;
}

// ── Global state ──────────────────────────────────────────────────────────────
static std::atomic<uint64_t> g_udp_rx   {0};
static std::atomic<uint32_t> g_last_id  {0};
static std::atomic<uint64_t> g_last_pts {0};
static std::atomic<uint32_t> g_last_w   {0};
static std::atomic<uint32_t> g_last_h   {0};

struct SavedFrame {
    uint32_t frame_id;
    uint64_t pts_us;
    uint32_t width, height;
    std::vector<uint8_t> crop_rgba;
    uint32_t crop_crc32;
};

static std::mutex              g_save_mutex;
static int                     g_saved  = 0;
constexpr int                  SAVE_MAX = 5;
static std::vector<SavedFrame> g_saved_frames;

static const char* WORK_DIR = "C:/MirageWork/MirageVulkan";

// ── Frame callback ────────────────────────────────────────────────────────────
static void on_frame(mirage::x1::CanonicalFrame frame) {
    if (!frame.valid()) return;  // assemblerでdecode_failed計上済み

    g_last_id .store(frame.frame_id);
    g_last_pts.store(frame.pts_us);
    g_last_w  .store(frame.width);
    g_last_h  .store(frame.height);

    std::lock_guard<std::mutex> lk(g_save_mutex);
    if (g_saved >= SAVE_MAX) return;

    // フルフレームPNG
    char full_path[512];
    snprintf(full_path, sizeof(full_path),
             "%s/canonical_f%06u_%ux%u_%llu.png",
             WORK_DIR, frame.frame_id,
             frame.width, frame.height,
             (unsigned long long)frame.pts_us);
    int ok = stbi_write_png(full_path,
        (int)frame.width, (int)frame.height, 4,
        frame.rgba.get(), (int)frame.width * 4);

    // crop: (100,100)-(200,200) = 100×100
    constexpr int CX=100, CY=100, CW=100, CH=100;
    auto crop = extract_crop(frame.rgba.get(), (int)frame.width, CX, CY, CW, CH);
    uint32_t crc = crc32_buf(crop.data(), crop.size());

    // crop PNG名: crop_f<id>_x<CX>y<CY>_<CW>x<CH>_crc<CRC32>.png
    char crop_path[512];
    snprintf(crop_path, sizeof(crop_path),
             "%s/crop_f%06u_x%dy%d_%dx%d_crc%08X.png",
             WORK_DIR, frame.frame_id, CX, CY, CW, CH, crc);
    stbi_write_png(crop_path, CW, CH, 4, crop.data(), CW * 4);

    printf("[SAVE] #%d  id=%-6u  %ux%u  pts=%.3fs  full=%s\n"
           "       crop: %s  crc32=%08X\n",
           g_saved + 1,
           frame.frame_id, frame.width, frame.height,
           frame.pts_us / 1e6,
           ok ? "OK" : "FAIL",
           crop_path, crc);
    fflush(stdout);

    SavedFrame sf;
    sf.frame_id   = frame.frame_id;
    sf.pts_us     = frame.pts_us;
    sf.width      = frame.width;
    sf.height     = frame.height;
    sf.crop_rgba  = std::move(crop);
    sf.crop_crc32 = crc;
    g_saved_frames.push_back(std::move(sf));
    g_saved++;
}

// ── unresolved helper ─────────────────────────────────────────────────────────
static int64_t calc_unresolved(const mirage::x1::CanonicalFrameAssembler::Stats& st) {
    int64_t u = (int64_t)st.frames_announced
              - (int64_t)st.delivered
              - (int64_t)st.incomplete_frames_evicted
              - (int64_t)st.frames_decode_failed
              - (int64_t)st.decoded_size_mismatch;
    // 正値のみ表示 (負 = まだバッファ中のフレームが含まれる場合)
    return u < 0 ? 0 : u;
}

// ── Entry point ───────────────────────────────────────────────────────────────
int main(int /*argc*/, char** /*argv*/) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n"); return 1;
    }
#endif

    const int port = mirage::x1::PORT_CANONICAL;  // 50201

    printf("=== Canonical Lane Test v4.1 ===\n");
    printf("UDP port : %d\n", port);
    printf("Target   : %dx%d @ %dfps\n\n",
           mirage::x1::CANONICAL_W, mirage::x1::CANONICAL_H,
           mirage::x1::CANONICAL_FPS);
    fflush(stdout);

    // ── Socket ───────────────────────────────────────────────────────────────
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { fprintf(stderr, "socket() failed\n"); return 1; }
    {
        int v = 4 * 1024 * 1024;
        setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&v, sizeof(v));
    }
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "bind() failed WSA=%d\n", WSAGetLastError());
        closesocket(sock); return 1;
    }
    printf("UDP bind OK  ::%d\n\n", port);
    fflush(stdout);

    // ── Assembler ────────────────────────────────────────────────────────────
    mirage::x1::CanonicalFrameAssembler assembler;
    assembler.set_callback(on_frame);

    // ── 1秒ごとStatsスレッド ─────────────────────────────────────────────────
    auto t_start = clk::now();
    std::atomic<bool> stop_stats{false};
    uint64_t prev_delivered = 0;

    auto stats_thread = std::thread([&] {
        auto t_prev = clk::now();
        while (!stop_stats.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto now = clk::now();
            double elapsed = std::chrono::duration<double>(now - t_start).count();
            double dt      = std::chrono::duration<double>(now - t_prev).count();
            t_prev = now;

            const auto& st = assembler.stats();
            double fps = (st.delivered - prev_delivered) / dt;
            prev_delivered = st.delivered;

            printf("[%4.0fs]"
                   "  udp=%-7llu"
                   "  ann=%-5llu"
                   "  cmp=%-5llu"
                   "  jfail=%-3llu"
                   "  smm=%-3llu"
                   "  dold=%-4llu"
                   "  evict=%-4llu"
                   "  unres=%-4lld"
                   "  fps=%5.1f"
                   "  id=%-6u"
                   "  pts=%.3fs\n",
                   elapsed,
                   (unsigned long long)g_udp_rx.load(),
                   (unsigned long long)st.frames_announced,
                   (unsigned long long)st.delivered,
                   (unsigned long long)st.frames_decode_failed,
                   (unsigned long long)st.decoded_size_mismatch,
                   (unsigned long long)st.dropped_old,
                   (unsigned long long)st.incomplete_frames_evicted,
                   (long long)calc_unresolved(st),
                   fps,
                   (unsigned int)g_last_id.load(),
                   g_last_pts.load() / 1e6);

            uint32_t w = g_last_w.load(), h = g_last_h.load();
            if (st.delivered > 0 &&
                (w != (uint32_t)mirage::x1::CANONICAL_W ||
                 h != (uint32_t)mirage::x1::CANONICAL_H))
                printf("  [!!] RES MISMATCH: %ux%u  expected %dx%d\n",
                       w, h, mirage::x1::CANONICAL_W, mirage::x1::CANONICAL_H);

            fflush(stdout);
        }
    });

    // ── Recv loop (60秒) ──────────────────────────────────────────────────────
    std::vector<uint8_t> buf(mirage::x1::HEADER_SIZE + mirage::x1::MTU_PAYLOAD + 64);
    auto t0 = clk::now();

    while (std::chrono::duration<double>(clk::now() - t0).count() < 60.0) {
        DWORD tv = 500;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        sockaddr_in from{};
        int from_len = sizeof(from);
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

    // ── Final Summary ─────────────────────────────────────────────────────────
    const auto& st = assembler.stats();

    printf("\n=== Final Summary ===\n");
    printf("udp_packets_received      : %llu\n", (unsigned long long)g_udp_rx.load());
    printf("frames_announced          : %llu\n", (unsigned long long)st.frames_announced);
    printf("frames_completed(delivered): %llu\n",(unsigned long long)st.delivered);
    printf("jpeg_decode_failed        : %llu\n", (unsigned long long)st.frames_decode_failed);
    printf("decoded_size_mismatch     : %llu\n", (unsigned long long)st.decoded_size_mismatch);
    printf("dropped_old               : %llu\n", (unsigned long long)st.dropped_old);
    printf("incomplete_frames_evicted : %llu\n", (unsigned long long)st.incomplete_frames_evicted);
    printf("unresolved_frames         : %lld\n", (long long)calc_unresolved(st));
    printf("fragments_received        : %llu\n", (unsigned long long)st.fragments_received);
    printf("fragments_expected        : %llu\n", (unsigned long long)st.fragments_expected);
    printf("frag_loss_rate            : %.4f%%\n",
           st.fragments_expected > 0
           ? 100.0 * (1.0 - (double)st.fragments_received / st.fragments_expected)
           : 0.0);
    printf("avg_fps                   : %.1f\n", st.delivered / 60.0);
    printf("last_res                  : %ux%u\n",
           (unsigned)g_last_w.load(), (unsigned)g_last_h.load());
    printf("res_ok                    : %s\n",
           (g_last_w.load() == (uint32_t)mirage::x1::CANONICAL_W &&
            g_last_h.load() == (uint32_t)mirage::x1::CANONICAL_H) ? "PASS" : "FAIL");
    printf("PNG_saved                 : %d\n", g_saved);

    // ── Crop CRC32 Analysis ───────────────────────────────────────────────────
    if ((int)g_saved_frames.size() >= 2) {
        printf("\n=== Crop CRC32 Analysis (x100,y100 / 100x100) ===\n");
        const auto& ref = g_saved_frames[0];
        printf("Reference: f%06u  crc=%08X\n", ref.frame_id, ref.crop_crc32);
        for (size_t i = 1; i < g_saved_frames.size(); ++i) {
            const auto& f = g_saved_frames[i];
            int diff_px = 0;
            for (size_t b = 0; b < f.crop_rgba.size(); b += 4) {
                int dr = abs((int)f.crop_rgba[b]   - (int)ref.crop_rgba[b]);
                int dg = abs((int)f.crop_rgba[b+1] - (int)ref.crop_rgba[b+1]);
                int db = abs((int)f.crop_rgba[b+2] - (int)ref.crop_rgba[b+2]);
                if (dr + dg + db > 15) ++diff_px;
            }
            const char* label =
                diff_px == 0   ? "IDENTICAL" :
                diff_px < 100  ? "NEAR-IDENTICAL" :
                                 "CONTENT-CHANGED";
            printf("  f%06u  crc=%08X  diff_px=%d/10000  %s%s\n",
                   f.frame_id, f.crop_crc32, diff_px, label,
                   f.crop_crc32 == ref.crop_crc32 ? " [crc_match]" : "");
        }
        printf("\nCrop resolution / size check:\n");
        for (const auto& f : g_saved_frames)
            printf("  f%06u: %ux%u  crop_bytes=%zu  %s\n",
                   f.frame_id, f.width, f.height, f.crop_rgba.size(),
                   (f.width  == (uint32_t)mirage::x1::CANONICAL_W &&
                    f.height == (uint32_t)mirage::x1::CANONICAL_H)
                   ? "res=PASS" : "res=FAIL");
    }

    // ── Phase 1 合否判定 ──────────────────────────────────────────────────────
    bool pass =
        g_last_w.load() == (uint32_t)mirage::x1::CANONICAL_W &&
        g_last_h.load() == (uint32_t)mirage::x1::CANONICAL_H &&
        st.delivered > 100 &&
        st.frames_decode_failed    == 0 &&
        st.decoded_size_mismatch   == 0 &&
        st.incomplete_frames_evicted == 0 &&
        g_saved == SAVE_MAX;

    printf("\n[Phase 1] %s\n", pass ? "PASS" : "FAIL");
    fflush(stdout);

#ifdef _WIN32
    WSACleanup();
#endif
    return pass ? 0 : 1;
}
