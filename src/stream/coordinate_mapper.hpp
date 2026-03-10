// =============================================================================
// coordinate_mapper.hpp — Phase D: PiP click → Canonical coordinate conversion
//
// Responsibility:
//   Given the PiP draw rect (ImGui screen coords) and a mouse position,
//   convert to Canonical frame coordinates (0..CANONICAL_W-1, 0..CANONICAL_H-1).
//
// Canonical frame: 600×1000  (fixed, matches Canonical Lane)
// Monitor frame  : 720×1200  (aspect 3:5, same ratio as Canonical 600×1000)
//
// Because Monitor(3:5) == Canonical(3:5), no letterbox correction is needed.
// The mapping is a simple linear scale from PiP pixel space to canonical space.
//
// Usage:
//   CoordinateMapper mapper;
//   mapper.update_view(pip_x, pip_y, pip_w, pip_h);
//
//   int cx, cy;
//   if (mapper.map_to_canonical(mouse_x, mouse_y, cx, cy)) {
//       // cx in [0, 599], cy in [0, 999]
//       send_tap(cx, cy);
//   }
// =============================================================================
#pragma once

#include <cmath>

namespace mirage::x1 {

// Canonical frame dimensions (fixed)
constexpr int CANONICAL_W = 600;
constexpr int CANONICAL_H = 1000;

class CoordinateMapper {
public:
    CoordinateMapper() = default;

    // ── View rect ─────────────────────────────────────────────────────────
    // Call this every frame after drawing the PiP image (ImGui screen coords).
    //   view_x, view_y : top-left of the drawn image in screen space
    //   view_w, view_h : rendered size of the PiP image in pixels
    void update_view(float view_x, float view_y, float view_w, float view_h) {
        view_x_ = view_x;
        view_y_ = view_y;
        view_w_ = view_w;
        view_h_ = view_h;
        valid_  = (view_w > 0.0f && view_h > 0.0f);
    }

    // ── Coordinate conversion ──────────────────────────────────────────────
    // Returns true if mouse is inside the PiP rect.
    // out_cx, out_cy are canonical coords (0..CANONICAL_W-1, 0..CANONICAL_H-1).
    bool map_to_canonical(float mouse_x, float mouse_y,
                          int& out_cx, int& out_cy) const {
        if (!valid_) return false;

        // Reject outside
        if (mouse_x < view_x_ || mouse_x >= view_x_ + view_w_) return false;
        if (mouse_y < view_y_ || mouse_y >= view_y_ + view_h_) return false;

        // Normalize to [0, 1) within PiP
        float nx = (mouse_x - view_x_) / view_w_;
        float ny = (mouse_y - view_y_) / view_h_;

        // Scale to canonical
        float cx = nx * (float)CANONICAL_W;
        float cy = ny * (float)CANONICAL_H;

        // Floor and clamp (guard against fp edge cases)
        out_cx = clamp_int((int)cx, 0, CANONICAL_W - 1);
        out_cy = clamp_int((int)cy, 0, CANONICAL_H - 1);
        return true;
    }

    // Convenience: same but takes (view_x, view_y, view_w, view_h) explicitly.
    // Useful for unit tests without calling update_view first.
    static bool map(float mouse_x, float mouse_y,
                    float view_x, float view_y,
                    float view_w, float view_h,
                    int& out_cx, int& out_cy) {
        if (view_w <= 0.0f || view_h <= 0.0f) return false;
        if (mouse_x < view_x || mouse_x >= view_x + view_w) return false;
        if (mouse_y < view_y || mouse_y >= view_y + view_h) return false;

        float nx = (mouse_x - view_x) / view_w;
        float ny = (mouse_y - view_y) / view_h;
        out_cx = clamp_int((int)(nx * (float)CANONICAL_W), 0, CANONICAL_W - 1);
        out_cy = clamp_int((int)(ny * (float)CANONICAL_H), 0, CANONICAL_H - 1);
        return true;
    }

    // ── State ──────────────────────────────────────────────────────────────
    bool  is_valid()  const { return valid_; }
    float view_x()    const { return view_x_; }
    float view_y()    const { return view_y_; }
    float view_w()    const { return view_w_; }
    float view_h()    const { return view_h_; }

private:
    static int clamp_int(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    float view_x_ = 0.0f;
    float view_y_ = 0.0f;
    float view_w_ = 0.0f;
    float view_h_ = 0.0f;
    bool  valid_  = false;
};

} // namespace mirage::x1
