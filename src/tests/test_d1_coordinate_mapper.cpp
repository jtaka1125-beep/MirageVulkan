// =============================================================================
// test_d1_coordinate_mapper.cpp — Phase D-1: CoordinateMapper unit tests
//
// No Android device needed — pure math validation.
//
// Test cases:
//   corners     : top-left, top-right, bottom-left, bottom-right
//   center      : exact center of PiP
//   outside     : rejected (8 points around boundary)
//   boundary    : edge pixels inside vs. exactly-on-edge
//   scaled_view : different PiP sizes (small / large / non-square aspect)
//   fp_robustness: floating-point near-edge values
//
// Pass criteria (D1 gate):
//   all_corners_ok  : 4 corner mappings match expected canonical coords
//   center_ok       : center maps to (299, 499)
//   outside_rejected: all 8 outside points return false
//   boundary_ok     : last-pixel-inside accepted, first-outside rejected
//   scaled_ok       : mapping correct after resize
//   D1_PASS
// =============================================================================

#include "stream/coordinate_mapper.hpp"
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>

using namespace mirage::x1;

// ── Test helper ───────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, bool cond) {
    if (cond) {
        printf("  PASS  %s\n", name);
        g_pass++;
    } else {
        printf("  FAIL  %s  <<<\n", name);
        g_fail++;
    }
}

static void check_map(const char* name,
                      float mx, float my,
                      float vx, float vy, float vw, float vh,
                      bool expect_hit,
                      int expect_cx = 0, int expect_cy = 0) {
    int cx = -1, cy = -1;
    bool hit = CoordinateMapper::map(mx, my, vx, vy, vw, vh, cx, cy);

    if (!expect_hit) {
        bool ok = !hit;
        if (!ok) printf("  FAIL  %s  got hit=true  cx=%d cy=%d  <<<\n", name, cx, cy);
        else     printf("  PASS  %s  (rejected as expected)\n", name);
        ok ? g_pass++ : g_fail++;
        return;
    }

    bool ok = hit && (cx == expect_cx) && (cy == expect_cy);
    if (!ok) {
        printf("  FAIL  %s  expected (%d,%d) got (%d,%d) hit=%d  <<<\n",
               name, expect_cx, expect_cy, cx, cy, (int)hit);
        g_fail++;
    } else {
        printf("  PASS  %s  → (%d,%d)\n", name, cx, cy);
        g_pass++;
    }
}

// ── Tests ─────────────────────────────────────────────────────────────────

static void test_corners() {
    printf("\n[corners] view=(100,200) 300×500\n");
    float vx=100, vy=200, vw=300, vh=500;

    // Top-left pixel
    check_map("top-left",       100.0f, 200.0f, vx,vy,vw,vh, true,   0,   0);
    // Top-right: last pixel in x direction = vx + vw - 1 pixel
    // nx = (399.99 - 100) / 300 ≈ 0.9999 → cx = 599
    check_map("top-right",      399.5f, 200.0f, vx,vy,vw,vh, true, 599,   0);
    // Bottom-left
    check_map("bottom-left",    100.0f, 699.5f, vx,vy,vw,vh, true,   0, 999);
    // Bottom-right
    check_map("bottom-right",   399.5f, 699.5f, vx,vy,vw,vh, true, 599, 999);
}

static void test_center() {
    printf("\n[center] view=(0,0) 600×1000\n");
    // Exact 1:1 mapping — center pixel of view = (299,499) in canonical
    float vx=0, vy=0, vw=600, vh=1000;
    // Center of view = (300, 500) → nx=0.5 → cx = 300 → floor → 300? No:
    //   cx = (int)(0.5f * 600) = 300  → but that's inside [0,599] ✓
    // Exact center of canonical = 300,500 (not 299,499)
    check_map("center(300,500)", 300.0f, 500.0f, vx,vy,vw,vh, true, 300, 500);
    // Top-left pixel
    check_map("pixel(0,0)",       0.0f,   0.0f, vx,vy,vw,vh, true,   0,   0);
    // Pixel (599,999) — last canonical pixel: mouse=(599.5, 999.5)
    check_map("pixel(599,999)", 599.5f, 999.5f, vx,vy,vw,vh, true, 599, 999);
}

static void test_outside_rejected() {
    printf("\n[outside] view=(100,200) 300×500\n");
    float vx=100, vy=200, vw=300, vh=500;

    check_map("left-of",     99.9f, 350.0f, vx,vy,vw,vh, false);
    check_map("right-of",   400.1f, 350.0f, vx,vy,vw,vh, false);
    check_map("above",      200.0f, 199.9f, vx,vy,vw,vh, false);
    check_map("below",      200.0f, 700.1f, vx,vy,vw,vh, false);
    check_map("top-left-out", 99.0f, 199.0f, vx,vy,vw,vh, false);
    check_map("top-right-out",400.5f,199.0f, vx,vy,vw,vh, false);
    check_map("bot-left-out", 99.0f, 701.0f, vx,vy,vw,vh, false);
    check_map("bot-right-out",400.5f,701.0f, vx,vy,vw,vh, false);
    // Exactly on right edge (>= vx+vw) → rejected
    check_map("exact-right-edge", 400.0f, 300.0f, vx,vy,vw,vh, false);
    // Exactly on bottom edge (>= vy+vh) → rejected
    check_map("exact-bottom-edge",200.0f, 700.0f, vx,vy,vw,vh, false);
}

static void test_boundary() {
    printf("\n[boundary] view=(0,0) 600×1000\n");
    float vx=0, vy=0, vw=600, vh=1000;

    // Last pixel inside: x=599.9 → nx=0.9998 → cx=599
    check_map("last-x-inside",  599.9f,   0.0f, vx,vy,vw,vh, true, 599, 0);
    // First pixel outside: x=600.0 → rejected
    check_map("first-x-outside",600.0f,   0.0f, vx,vy,vw,vh, false);
    // Last y inside: y=999.9 → ny=0.9999 → cy=999
    check_map("last-y-inside",    0.0f, 999.9f, vx,vy,vw,vh, true, 0, 999);
    // First y outside
    check_map("first-y-outside",  0.0f,1000.0f, vx,vy,vw,vh, false);
}

static void test_scaled_views() {
    printf("\n[scaled] various PiP sizes\n");

    // Small PiP (150×250) at (50,50): half resolution
    {
        float vx=50, vy=50, vw=150, vh=250;
        // top-left
        check_map("small:top-left",  50.0f,  50.0f, vx,vy,vw,vh, true, 0, 0);
        // center: (125, 175) → nx=0.5 → cx=300
        check_map("small:center",   125.0f, 175.0f, vx,vy,vw,vh, true, 300, 500);
        // outside
        check_map("small:outside",   49.0f,  50.0f, vx,vy,vw,vh, false);
    }

    // Large PiP (900×1500) at (0,0): 1.5× resolution
    {
        float vx=0, vy=0, vw=900, vh=1500;
        // top-left
        check_map("large:top-left",   0.0f,   0.0f, vx,vy,vw,vh, true, 0, 0);
        // center: (450,750) → cx=300, cy=500
        check_map("large:center",   450.0f, 750.0f, vx,vy,vw,vh, true, 300, 500);
        // bottom-right: (899.9, 1499.9)
        check_map("large:bot-right",899.9f,1499.9f, vx,vy,vw,vh, true, 599, 999);
    }

    // Non-integer PiP (240×400) at (12.5, 33.3)
    {
        float vx=12.5f, vy=33.3f, vw=240.0f, vh=400.0f;
        // top-left
        check_map("frac:top-left",  12.5f, 33.3f, vx,vy,vw,vh, true, 0, 0);
        // center
        check_map("frac:center",   132.5f,233.3f, vx,vy,vw,vh, true, 300, 500);
    }
}

static void test_update_view_api() {
    printf("\n[update_view API]\n");
    CoordinateMapper m;

    // Before update — not valid
    check("not-valid-before-update", !m.is_valid());

    m.update_view(0.0f, 0.0f, 600.0f, 1000.0f);
    check("valid-after-update", m.is_valid());

    int cx, cy;
    bool hit = m.map_to_canonical(300.0f, 500.0f, cx, cy);
    check("member:center-hit",     hit);
    check("member:center-cx==300", cx == 300);
    check("member:center-cy==500", cy == 500);

    hit = m.map_to_canonical(-1.0f, 500.0f, cx, cy);
    check("member:outside-rejected", !hit);

    // Resize
    m.update_view(0.0f, 0.0f, 300.0f, 500.0f);
    hit = m.map_to_canonical(150.0f, 250.0f, cx, cy);
    check("member:after-resize-center", hit && cx==300 && cy==500);
}

static void test_zero_size() {
    printf("\n[zero-size guard]\n");
    int cx, cy;
    bool hit = CoordinateMapper::map(100.0f, 100.0f, 100.0f, 100.0f, 0.0f, 100.0f, cx, cy);
    check("zero-width-rejected", !hit);
    hit = CoordinateMapper::map(100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 0.0f, cx, cy);
    check("zero-height-rejected", !hit);

    CoordinateMapper m;
    m.update_view(0, 0, 0, 100);
    hit = m.map_to_canonical(0, 0, cx, cy);
    check("member:zero-width-rejected", !hit);
}

// ── Main ──────────────────────────────────────────────────────────────────

int main() {
    printf("=== D-1 CoordinateMapper Unit Tests ===\n");
    printf("  Canonical frame: %dx%d\n\n", CANONICAL_W, CANONICAL_H);

    test_corners();
    test_center();
    test_outside_rejected();
    test_boundary();
    test_scaled_views();
    test_update_view_api();
    test_zero_size();

    printf("\n--- Results ---\n");
    printf("  PASS: %d\n", g_pass);
    printf("  FAIL: %d\n", g_fail);

    bool all_pass = (g_fail == 0);
    printf("\n[D1 Gate] %s\n", all_pass ? "D1_PASS" : "D1_FAIL");
    return all_pass ? 0 : 1;
}
