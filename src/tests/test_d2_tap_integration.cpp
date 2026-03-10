// =============================================================================
// test_d2_tap_integration.cpp — Phase D-3 gate: tap send integration
//
// Validates the complete Phase D-2 pipeline WITHOUT a live GUI:
//   CoordinateMapper (canonical coords)
//   → AdbTouchFallback::device_width/height (wm size)
//   → scale canonical → device coords
//   → AdbTouchFallback::tap (ADB shell input tap)
//
// Device: X1  192.168.0.3:5555
//
// Usage:
//   test_d2_tap_integration.exe            # full (3 taps + scale + outside)
//   test_d2_tap_integration.exe --dry-run  # skip actual tap, validate math only
//
// Pass criteria (D3 gate):
//   wm_size_ok      : device_width==1200 && device_height==2000
//   scale_center_ok : (300,500) [600x1000] -> (600,1000) [1200x2000]
//   scale_tl_ok     : (0,0)     -> (0,0)
//   scale_br_ok     : (599,999) -> (1198,1998)
//   outside_reject  : CoordinateMapper rejects out-of-PiP clicks
//   tap_center_ok   : tap(600,1000) rc=0
//   tap_tl_ok       : tap(0,0)     rc=0   (corner)
//   tap_br_ok       : tap(1198,1998) rc=0  (corner)
//   D3_PASS
// =============================================================================

#include "stream/coordinate_mapper.hpp"
#include "adb_touch_fallback.hpp"
#include "mirage_log.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <chrono>

using namespace mirage;
using namespace mirage::x1;
using namespace std::chrono;

// ── Test helper ─────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, bool cond, const char* detail = "") {
    if (cond) { printf("  PASS  %s%s%s\n", name, detail[0]?" ":"", detail); g_pass++; }
    else       { printf("  FAIL  %s%s%s  <<<\n", name, detail[0]?" ":"", detail); g_fail++; }
}

// ── Scale helper (mirrors hybrid_command_sender.cpp logic) ──────────────────

struct ScaleResult {
    int tap_x, tap_y;
};

static ScaleResult scale_canonical(int cx, int cy, int src_w, int src_h,
                                   int dev_w, int dev_h) {
    ScaleResult r;
    r.tap_x = (int)std::round(cx * (double)dev_w / src_w);
    r.tap_y = (int)std::round(cy * (double)dev_h / src_h);
    return r;
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void test_wm_size(AdbTouchFallback& fb, int& out_dev_w, int& out_dev_h) {
    printf("\n[wm_size] querying device screen resolution...\n");
    out_dev_w = fb.device_width();
    out_dev_h = fb.device_height();
    printf("  device_width  = %d  (expect 1200)\n", out_dev_w);
    printf("  device_height = %d  (expect 2000)\n", out_dev_h);
    check("wm_size: width==1200",  out_dev_w == 1200);
    check("wm_size: height==2000", out_dev_h == 2000);
}

static void test_scale_math(int dev_w, int dev_h) {
    printf("\n[scale_math] canonical 600x1000 -> %dx%d\n", dev_w, dev_h);

    // center (300,500) -> (600,1000)
    auto r = scale_canonical(300, 500, CANONICAL_W, CANONICAL_H, dev_w, dev_h);
    char buf[64]; snprintf(buf, sizeof(buf), "(%d,%d)->(%d,%d)", 300,500,r.tap_x,r.tap_y);
    check("scale_center", r.tap_x==600 && r.tap_y==1000, buf);

    // top-left (0,0) -> (0,0)
    r = scale_canonical(0, 0, CANONICAL_W, CANONICAL_H, dev_w, dev_h);
    snprintf(buf, sizeof(buf), "(0,0)->(%d,%d)", r.tap_x, r.tap_y);
    check("scale_tl", r.tap_x==0 && r.tap_y==0, buf);

    // bottom-right (599,999) -> (1198,1998)
    r = scale_canonical(599, 999, CANONICAL_W, CANONICAL_H, dev_w, dev_h);
    snprintf(buf, sizeof(buf), "(599,999)->(%d,%d)", r.tap_x, r.tap_y);
    check("scale_br", r.tap_x==1198 && r.tap_y==1998, buf);

    // quarter (150,250) -> (300,500)
    r = scale_canonical(150, 250, CANONICAL_W, CANONICAL_H, dev_w, dev_h);
    snprintf(buf, sizeof(buf), "(150,250)->(%d,%d)", r.tap_x, r.tap_y);
    check("scale_quarter", r.tap_x==300 && r.tap_y==500, buf);
}

static void test_outside_reject() {
    printf("\n[outside_reject] CoordinateMapper boundary checks\n");
    // Simulate a 120x200 PiP at (1569.5, 35.0) — values from live log
    float vx=1569.5f, vy=35.0f, vw=120.0f, vh=200.0f;

    auto try_map = [&](float mx, float my, bool expect_hit,
                       int exp_cx=0, int exp_cy=0) -> bool {
        int cx, cy;
        bool hit = CoordinateMapper::map(mx, my, vx, vy, vw, vh, cx, cy);
        if (!expect_hit) return !hit;
        return hit && cx==exp_cx && cy==exp_cy;
    };

    check("outside_left",    try_map(1560.0f, 135.0f, false));
    check("outside_right",   try_map(1695.0f, 135.0f, false));
    check("outside_above",   try_map(1629.0f,  30.0f, false));
    check("outside_below",   try_map(1629.0f, 240.0f, false));
    // Center of 120x200 PiP: (1629.5, 135.0) -> canonical (300,500)
    check("inside_center",   try_map(1629.5f, 135.0f, true, 300, 500));
}

static void test_tap_send(AdbTouchFallback& fb, bool dry_run,
                          int dev_w, int dev_h) {
    printf("\n[tap_send] %s\n", dry_run ? "(DRY RUN - skipping actual tap)" : "sending taps to X1");

    struct TapCase { int cx, cy; const char* label; };
    std::vector<TapCase> cases = {
        {300, 500, "center"},
        {  0,   0, "top-left (corner)"},
        {599, 999, "bot-right (corner)"},
    };

    for (auto& tc : cases) {
        auto r = scale_canonical(tc.cx, tc.cy, CANONICAL_W, CANONICAL_H, dev_w, dev_h);
        char name[80];
        snprintf(name, sizeof(name), "tap_%s canonical=(%d,%d) device=(%d,%d)",
                 tc.label, tc.cx, tc.cy, r.tap_x, r.tap_y);

        if (dry_run) {
            // In dry-run, just verify the scale math is valid
            bool valid = r.tap_x >= 0 && r.tap_x < dev_w &&
                         r.tap_y >= 0 && r.tap_y < dev_h;
            check(name, valid, "[dry-run: coords in range]");
        } else {
            bool ok = fb.tap(r.tap_x, r.tap_y);
            check(name, ok, ok ? "[sent]" : "[FAILED]");
            // Small delay between taps
            std::this_thread::sleep_for(std::chrono::milliseconds(600));
        }
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    bool dry_run = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--dry-run")) dry_run = true;
    }

    printf("=== D-3 Tap Integration Test%s ===\n",
           dry_run ? " (DRY RUN)" : "");
    printf("  Device: X1  192.168.0.3:5555\n");
    printf("  Canonical: %dx%d\n\n", CANONICAL_W, CANONICAL_H);

    // Build AdbTouchFallback pointed at X1
    AdbTouchFallback fb;
    fb.set_device("192.168.0.3:5555");
    fb.set_persistent_shell(false);  // Use per-command ADB for test clarity

    // --- Tests ---
    int dev_w = 0, dev_h = 0;
    test_wm_size(fb, dev_w, dev_h);

    // If wm size failed, use X1 known defaults for math tests
    if (dev_w <= 0) { dev_w = 1200; dev_h = 2000; printf("  (using X1 defaults 1200x2000)\n"); }

    test_scale_math(dev_w, dev_h);
    test_outside_reject();
    test_tap_send(fb, dry_run, dev_w, dev_h);

    printf("\n--- Results ---\n");
    printf("  PASS: %d\n", g_pass);
    printf("  FAIL: %d\n", g_fail);

    bool all_pass = (g_fail == 0);
    printf("\n[D3 Gate] %s\n", all_pass ? "D3_PASS" : "D3_FAIL");
    return all_pass ? 0 : 1;
}
