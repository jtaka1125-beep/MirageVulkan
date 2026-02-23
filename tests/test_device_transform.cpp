// =============================================================================
// Unit tests for DeviceTransform (src/device_transform.hpp/.cpp)
// GPU不要 — 座標変換・回転・スケーリングのCPU純粋ロジックテスト
// =============================================================================
#include <gtest/gtest.h>
#include <cmath>
#include "device_transform.hpp"

using namespace mirage::gui;

static constexpr float kEps = 0.5f;  // pixel tolerance

// ---------------------------------------------------------------------------
// T-1: アイデンティティ変換 (rotation=0, 同解像度)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, IdentityTransform) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 0;
    t.recalculate();

    EXPECT_TRUE(t.is_identity());

    float nx, ny;
    t.video_to_native(540, 960, nx, ny);
    EXPECT_NEAR(nx, 540, kEps);
    EXPECT_NEAR(ny, 960, kEps);
}

// ---------------------------------------------------------------------------
// T-2: 原点 (0,0) は変換後も (0,0)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, OriginMapsToOrigin) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 0;
    t.recalculate();

    float nx, ny;
    t.video_to_native(0, 0, nx, ny);
    EXPECT_NEAR(nx, 0, kEps);
    EXPECT_NEAR(ny, 0, kEps);
}

// ---------------------------------------------------------------------------
// T-3: 180度回転 — 中心点が中心に戻る
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, Rotation180CenterStaysCenter) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 180;
    t.recalculate();

    float cx = (1080 - 1) / 2.0f;
    float cy = (1920 - 1) / 2.0f;
    float nx, ny;
    t.video_to_native(cx, cy, nx, ny);
    EXPECT_NEAR(nx, cx, 1.0f);
    EXPECT_NEAR(ny, cy, 1.0f);
}

// ---------------------------------------------------------------------------
// T-4: 90度回転 — ビデオ縦横が入れ替わる
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, Rotation90SwapsDims) {
    DeviceTransform t;
    t.native_w = 1920; t.native_h = 1080;  // landscape native
    t.video_w  = 1080; t.video_h  = 1920;  // portrait video
    t.rotation = 90;
    t.recalculate();

    // After rotation 90°: rotated dims = (1920, 1080) == native
    // scale should be ~1.0
    EXPECT_NEAR(t.scale_x, 1.0f, 0.01f);
    EXPECT_NEAR(t.scale_y, 1.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// T-5: ラウンドトリップ (rotation=0) — video→native→video が元に戻る
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, RoundtripRotation0) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 0;
    t.recalculate();

    float nx, ny, vx2, vy2;
    t.video_to_native(300, 700, nx, ny);
    t.native_to_video(nx, ny, vx2, vy2);
    EXPECT_NEAR(vx2, 300, kEps);
    EXPECT_NEAR(vy2, 700, kEps);
}

// ---------------------------------------------------------------------------
// T-6: ラウンドトリップ (rotation=90)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, RoundtripRotation90) {
    DeviceTransform t;
    t.native_w = 1920; t.native_h = 1080;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 90;
    t.recalculate();

    float nx, ny, vx2, vy2;
    t.video_to_native(200, 400, nx, ny);
    t.native_to_video(nx, ny, vx2, vy2);
    EXPECT_NEAR(vx2, 200, kEps);
    EXPECT_NEAR(vy2, 400, kEps);
}

// ---------------------------------------------------------------------------
// T-7: ラウンドトリップ (rotation=180)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, RoundtripRotation180) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 180;
    t.recalculate();

    float nx, ny, vx2, vy2;
    t.video_to_native(100, 300, nx, ny);
    t.native_to_video(nx, ny, vx2, vy2);
    EXPECT_NEAR(vx2, 100, kEps);
    EXPECT_NEAR(vy2, 300, kEps);
}

// ---------------------------------------------------------------------------
// T-8: ラウンドトリップ (rotation=270)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, RoundtripRotation270) {
    DeviceTransform t;
    t.native_w = 1920; t.native_h = 1080;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 270;
    t.recalculate();

    float nx, ny, vx2, vy2;
    t.video_to_native(500, 800, nx, ny);
    t.native_to_video(nx, ny, vx2, vy2);
    EXPECT_NEAR(vx2, 500, kEps);
    EXPECT_NEAR(vy2, 800, kEps);
}

// ---------------------------------------------------------------------------
// T-9: スケールダウン — ビデオが native より大きい
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, ScaleDown) {
    DeviceTransform t;
    t.native_w = 540; t.native_h = 960;
    t.video_w  = 1080; t.video_h  = 1920;
    t.rotation = 0;
    t.recalculate();

    EXPECT_NEAR(t.scale_x, 0.5f, 0.01f);
    EXPECT_NEAR(t.scale_y, 0.5f, 0.01f);
}

// ---------------------------------------------------------------------------
// T-10: スケールアップ — ビデオが native より小さい
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, ScaleUp) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 540; t.video_h  = 960;
    t.rotation = 0;
    t.recalculate();

    EXPECT_NEAR(t.scale_x, 2.0f, 0.01f);
    EXPECT_NEAR(t.scale_y, 2.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// T-11: ナビゲーションバートリム — 高さが少しだけ小さい場合は1:1維持
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, NavBarTrimKeepsIdentity) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1080; t.video_h  = 1828;  // ~92px nav bar trimmed
    t.rotation = 0;
    t.nav_bar_tolerance_px = 200;
    t.recalculate();

    EXPECT_NEAR(t.scale_x, 1.0f, 0.01f);
    EXPECT_NEAR(t.scale_y, 1.0f, 0.01f);
    EXPECT_NEAR(t.offset_x, 0.0f, 0.01f);
    EXPECT_NEAR(t.offset_y, 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// T-12: ゼロサイズ — クラッシュせずにデフォルト返す
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, ZeroSizeNocrash) {
    DeviceTransform t;
    t.native_w = 0; t.native_h = 0;
    t.video_w  = 0; t.video_h  = 0;
    t.rotation = 0;
    t.recalculate();

    EXPECT_NEAR(t.scale_x, 1.0f, 0.01f);
    EXPECT_NEAR(t.scale_y, 1.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// T-13: アスペクト比違い (letterbox) — オフセットが付く
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, LetterboxOffsetNonZero) {
    DeviceTransform t;
    t.native_w = 1080; t.native_h = 1920;
    t.video_w  = 1920; t.video_h  = 1080;  // 16:9 landscape video, portrait native
    t.rotation = 0;
    t.crop = false;
    t.recalculate();

    // letterbox: scale = min(1080/1920, 1920/1080) = 0.5625
    // One of the offsets should be non-zero (horizontal bars)
    bool has_offset = (std::abs(t.offset_x) > 0.5f || std::abs(t.offset_y) > 0.5f);
    EXPECT_TRUE(has_offset);
}

// ---------------------------------------------------------------------------
// T-14: crop=true → scale が max(sx,sy)
// ---------------------------------------------------------------------------
TEST(DeviceTransformTest, CropUseMaxScale) {
    DeviceTransform no_crop, with_crop;
    no_crop.native_w  = with_crop.native_w  = 1080;
    no_crop.native_h  = with_crop.native_h  = 1920;
    no_crop.video_w   = with_crop.video_w   = 1920;
    no_crop.video_h   = with_crop.video_h   = 1080;
    no_crop.rotation  = with_crop.rotation  = 0;
    no_crop.crop      = false;
    with_crop.crop    = true;
    no_crop.recalculate();
    with_crop.recalculate();

    EXPECT_GT(with_crop.scale_x, no_crop.scale_x);
}
