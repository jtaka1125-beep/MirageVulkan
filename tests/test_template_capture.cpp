// =============================================================================
// Unit tests for TemplateCapture (src/ai/template_capture.hpp/cpp)
// GPU不要 — CPU RGBAバッファからROI切り出し＋Gray8変換テスト
// =============================================================================
#include <gtest/gtest.h>
#include "ai/template_capture.hpp"

#include <cstring>
#include <vector>
#include <cmath>

using namespace mirage::ai;

// ---------------------------------------------------------------------------
// テスト用RGBAフレーム生成ヘルパー
// ---------------------------------------------------------------------------
static std::vector<uint8_t> makeRgbaFrame(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    std::vector<uint8_t> buf(w * h * 4);
    for (int i = 0; i < w * h; ++i) {
        buf[i * 4 + 0] = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
    return buf;
}

// Gray8期待値（luma近似: (77*r + 150*g + 29*b + 128) >> 8）
static uint8_t expectedGray(uint8_t r, uint8_t g, uint8_t b) {
    int y = (77 * r + 150 * g + 29 * b + 128) >> 8;
    return (uint8_t)std::min(std::max(y, 0), 255);
}

// ---------------------------------------------------------------------------
// 正常なROI切出し — フレーム全体
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, FullFrameCapture) {
    const int W = 4, H = 3;
    auto rgba = makeRgbaFrame(W, H, 100, 200, 50);

    RoiRect roi{0, 0, W, H};
    auto result = captureTemplateGray8FromBuffer(rgba.data(), W, H, roi);

    ASSERT_TRUE(result.is_ok()) << result.error().message;
    auto& gray = result.value();
    EXPECT_EQ(gray.w, W);
    EXPECT_EQ(gray.h, H);
    EXPECT_EQ(gray.stride, W);
    EXPECT_EQ(gray.pix.size(), (size_t)(W * H));

    uint8_t expected = expectedGray(100, 200, 50);
    for (auto px : gray.pix) {
        EXPECT_EQ(px, expected);
    }
}

// ---------------------------------------------------------------------------
// 正常なROI切出し — 部分領域
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, SubRegionCapture) {
    const int W = 10, H = 10;
    // グラデーションフレーム: ピクセル(x,y)のRを x*25 にする
    std::vector<uint8_t> rgba(W * H * 4);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            int idx = (y * W + x) * 4;
            rgba[idx + 0] = (uint8_t)(x * 25);  // R
            rgba[idx + 1] = 0;                    // G
            rgba[idx + 2] = 0;                    // B
            rgba[idx + 3] = 255;                  // A
        }
    }

    RoiRect roi{2, 3, 4, 5};
    auto result = captureTemplateGray8FromBuffer(rgba.data(), W, H, roi);

    ASSERT_TRUE(result.is_ok()) << result.error().message;
    auto& gray = result.value();
    EXPECT_EQ(gray.w, 4);
    EXPECT_EQ(gray.h, 5);

    // roi内のピクセル検証: x=2..5, y=3..7
    for (int ry = 0; ry < 5; ++ry) {
        for (int rx = 0; rx < 4; ++rx) {
            int src_x = roi.x + rx;
            uint8_t expected = expectedGray((uint8_t)(src_x * 25), 0, 0);
            EXPECT_EQ(gray.pix[ry * gray.stride + rx], expected)
                << "at roi(" << rx << "," << ry << ")";
        }
    }
}

// ---------------------------------------------------------------------------
// RGBA→Gray8 変換の数値検証（代表色）
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, GrayConversionValues) {
    struct TestCase {
        uint8_t r, g, b;
    };

    TestCase cases[] = {
        {0, 0, 0},           // 黒
        {255, 255, 255},     // 白
        {255, 0, 0},         // 純赤
        {0, 255, 0},         // 純緑
        {0, 0, 255},         // 純青
        {128, 128, 128},     // 中間灰
    };

    for (auto& tc : cases) {
        auto rgba = makeRgbaFrame(1, 1, tc.r, tc.g, tc.b);
        RoiRect roi{0, 0, 1, 1};
        auto result = captureTemplateGray8FromBuffer(rgba.data(), 1, 1, roi);
        ASSERT_TRUE(result.is_ok());
        EXPECT_EQ(result.value().pix[0], expectedGray(tc.r, tc.g, tc.b))
            << "RGB(" << (int)tc.r << "," << (int)tc.g << "," << (int)tc.b << ")";
    }
}

// ---------------------------------------------------------------------------
// nullデータ
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, NullData) {
    RoiRect roi{0, 0, 1, 1};
    auto result = captureTemplateGray8FromBuffer(nullptr, 10, 10, roi);
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// 不正フレームサイズ
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, InvalidFrameSize) {
    uint8_t dummy[4] = {0};
    RoiRect roi{0, 0, 1, 1};

    auto r1 = captureTemplateGray8FromBuffer(dummy, 0, 10, roi);
    EXPECT_TRUE(r1.is_err());

    auto r2 = captureTemplateGray8FromBuffer(dummy, 10, 0, roi);
    EXPECT_TRUE(r2.is_err());

    auto r3 = captureTemplateGray8FromBuffer(dummy, -1, 10, roi);
    EXPECT_TRUE(r3.is_err());
}

// ---------------------------------------------------------------------------
// ROIが完全に範囲外（クランプ後に無効）
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, RoiCompletelyOutside) {
    auto rgba = makeRgbaFrame(10, 10, 128, 128, 128);
    RoiRect roi{20, 20, 5, 5};  // フレーム外

    CaptureConfig cfg;
    cfg.allow_partial_clamp = true;
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// ROI部分的に範囲外 → クランプで有効
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, RoiPartialClamp) {
    auto rgba = makeRgbaFrame(10, 10, 50, 100, 150);
    RoiRect roi{8, 8, 5, 5};  // (8,8) から 5x5 → クランプで (8,8,2,2)

    CaptureConfig cfg;
    cfg.allow_partial_clamp = true;
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);

    ASSERT_TRUE(result.is_ok()) << result.error().message;
    EXPECT_EQ(result.value().w, 2);
    EXPECT_EQ(result.value().h, 2);
}

// ---------------------------------------------------------------------------
// ROI範囲外 — クランプ無効時はエラー
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, RoiOutOfBoundsNoClamp) {
    auto rgba = makeRgbaFrame(10, 10, 128, 128, 128);
    RoiRect roi{8, 8, 5, 5};

    CaptureConfig cfg;
    cfg.allow_partial_clamp = false;
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);
    EXPECT_TRUE(result.is_err());
}

// ---------------------------------------------------------------------------
// ゼロサイズROI
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, ZeroSizeRoi) {
    auto rgba = makeRgbaFrame(10, 10, 128, 128, 128);

    {
        RoiRect roi{0, 0, 0, 5};
        CaptureConfig cfg;
        cfg.allow_partial_clamp = false;
        auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);
        EXPECT_TRUE(result.is_err());
    }
    {
        RoiRect roi{0, 0, 5, 0};
        CaptureConfig cfg;
        cfg.allow_partial_clamp = false;
        auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);
        EXPECT_TRUE(result.is_err());
    }
}

// ---------------------------------------------------------------------------
// 負座標ROI — クランプ有効
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, NegativeRoiWithClamp) {
    auto rgba = makeRgbaFrame(10, 10, 80, 160, 40);
    RoiRect roi{-3, -2, 8, 7};  // クランプ → (0,0,5,5)

    CaptureConfig cfg;
    cfg.allow_partial_clamp = true;
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 10, 10, roi, cfg);

    ASSERT_TRUE(result.is_ok()) << result.error().message;
    EXPECT_EQ(result.value().w, 5);
    EXPECT_EQ(result.value().h, 5);
}

// ---------------------------------------------------------------------------
// 1x1 フレーム / 1x1 ROI
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, SinglePixel) {
    auto rgba = makeRgbaFrame(1, 1, 200, 100, 50);
    RoiRect roi{0, 0, 1, 1};
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 1, 1, roi);

    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value().w, 1);
    EXPECT_EQ(result.value().h, 1);
    EXPECT_EQ(result.value().pix[0], expectedGray(200, 100, 50));
}

// ---------------------------------------------------------------------------
// strideがwと等しいことの検証
// ---------------------------------------------------------------------------
TEST(TemplateCaptureTest, StrideEqualsWidth) {
    auto rgba = makeRgbaFrame(16, 8, 0, 0, 0);
    RoiRect roi{2, 1, 10, 5};
    auto result = captureTemplateGray8FromBuffer(rgba.data(), 16, 8, roi);

    ASSERT_TRUE(result.is_ok());
    auto& gray = result.value();
    EXPECT_EQ(gray.stride, gray.w);
    EXPECT_EQ(gray.pix.size(), (size_t)(gray.stride * gray.h));
}
