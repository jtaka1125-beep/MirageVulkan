// =============================================================================
// Unit tests for AOA HID Touch (src/aoa_hid_touch.hpp / .cpp)
// =============================================================================
// Tests pure logic only: coordinate conversion, report building, contact state.
// No USB hardware required (USE_LIBUSB not defined).
// =============================================================================
#include <gtest/gtest.h>
#include "aoa_hid_touch.hpp"

using namespace mirage;
using namespace mirage::protocol;

// ===========================================================================
// pixel_to_hid_x / pixel_to_hid_y coordinate conversion
// ===========================================================================
TEST(AoaHidTouch, PixelToHidOrigin) {
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(0, 1080), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(0, 1920), 0);
}

TEST(AoaHidTouch, PixelToHidMaxEdge) {
    // Last pixel maps to HID_TOUCH_COORD_MAX
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(1079, 1080), HID_TOUCH_COORD_MAX);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(1919, 1920), HID_TOUCH_COORD_MAX);
}

TEST(AoaHidTouch, PixelToHidMidpoint) {
    // Midpoint of 1080-wide screen
    uint16_t mid_x = AoaHidTouch::pixel_to_hid_x(540, 1080);
    // Expected: 540 * 32767 / 1079 ≈ 16398
    EXPECT_GT(mid_x, 16000);
    EXPECT_LT(mid_x, 17000);

    uint16_t mid_y = AoaHidTouch::pixel_to_hid_y(960, 1920);
    EXPECT_GT(mid_y, 16000);
    EXPECT_LT(mid_y, 17000);
}

TEST(AoaHidTouch, PixelToHidNegativeClampsToZero) {
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(-1, 1080), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(-100, 1080), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(-1, 1920), 0);
}

TEST(AoaHidTouch, PixelToHidOverflowClampsToMax) {
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(1080, 1080), HID_TOUCH_COORD_MAX);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(5000, 1080), HID_TOUCH_COORD_MAX);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(2000, 1920), HID_TOUCH_COORD_MAX);
}

TEST(AoaHidTouch, PixelToHidDegenerateScreenSize) {
    // screen_w <= 1 should return 0
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(100, 0), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(100, 1), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(100, 0), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_y(100, 1), 0);
}

TEST(AoaHidTouch, PixelToHidSmallScreen) {
    // 2-pixel wide screen: pixel 0 -> 0, pixel 1 -> 32767
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(0, 2), 0);
    EXPECT_EQ(AoaHidTouch::pixel_to_hid_x(1, 2), HID_TOUCH_COORD_MAX);
}

// ===========================================================================
// pack_status
// ===========================================================================
TEST(AoaHidTouch, PackStatusTipDown) {
    uint8_t s = AoaHidTouch::pack_status(true, 0);
    EXPECT_EQ(s & 0x01, 1);        // bit0 = tip_switch
    EXPECT_EQ((s >> 3) & 0x1F, 0); // contact_id = 0
}

TEST(AoaHidTouch, PackStatusTipUp) {
    uint8_t s = AoaHidTouch::pack_status(false, 3);
    EXPECT_EQ(s & 0x01, 0);        // tip_switch off
    EXPECT_EQ((s >> 3) & 0x1F, 3); // contact_id = 3
}

TEST(AoaHidTouch, PackStatusMaxContactId) {
    uint8_t s = AoaHidTouch::pack_status(true, 31);
    EXPECT_EQ(s & 0x01, 1);
    EXPECT_EQ((s >> 3) & 0x1F, 31);
}

TEST(AoaHidTouch, PackStatusPaddingBitsZero) {
    // Bits 1-2 should always be zero
    for (uint8_t id = 0; id < 5; id++) {
        uint8_t s_down = AoaHidTouch::pack_status(true, id);
        uint8_t s_up   = AoaHidTouch::pack_status(false, id);
        EXPECT_EQ(s_down & 0x06, 0) << "id=" << (int)id;
        EXPECT_EQ(s_up   & 0x06, 0) << "id=" << (int)id;
    }
}

// ===========================================================================
// TouchReport struct layout
// ===========================================================================
TEST(AoaHidTouch, TouchReportSize) {
    EXPECT_EQ(sizeof(TouchReport), HID_TOUCH_REPORT_SIZE);
    EXPECT_EQ(sizeof(TouchReport), 27u);
}

TEST(AoaHidTouch, TouchReportPackedLayout) {
    TouchReport r{};
    r.report_id = HID_TOUCH_REPORT_ID;
    r.contacts[0].status = AoaHidTouch::pack_status(true, 0);
    r.contacts[0].x = 1000;
    r.contacts[0].y = 2000;
    r.contact_count = 1;

    // Verify via raw bytes
    auto* raw = reinterpret_cast<uint8_t*>(&r);
    EXPECT_EQ(raw[0], HID_TOUCH_REPORT_ID);  // report_id
    EXPECT_EQ(raw[26], 1);                    // contact_count at end
}

// ===========================================================================
// Contact state management (touch_down / touch_move / touch_up)
// ===========================================================================
TEST(AoaHidTouch, TouchDownSetsState) {
    AoaHidTouch touch;
    EXPECT_TRUE(touch.touch_down(0, 100, 200));
    // No crash, returns true
}

TEST(AoaHidTouch, TouchDownRejectsInvalidContact) {
    AoaHidTouch touch;
    EXPECT_FALSE(touch.touch_down(5, 100, 200));   // Max contacts is 5, id must be 0-4
    EXPECT_FALSE(touch.touch_down(255, 100, 200));
}

TEST(AoaHidTouch, TouchMoveRequiresActiveContact) {
    AoaHidTouch touch;
    // Move without prior touch_down should fail
    EXPECT_FALSE(touch.touch_move(0, 100, 200));
}

TEST(AoaHidTouch, TouchMoveAfterDown) {
    AoaHidTouch touch;
    EXPECT_TRUE(touch.touch_down(0, 100, 200));
    EXPECT_TRUE(touch.touch_move(0, 300, 400));
}

TEST(AoaHidTouch, TouchMoveRejectsInvalidContact) {
    AoaHidTouch touch;
    EXPECT_FALSE(touch.touch_move(5, 100, 200));
    EXPECT_FALSE(touch.touch_move(255, 100, 200));
}

TEST(AoaHidTouch, TouchUpReleasesContact) {
    AoaHidTouch touch;
    EXPECT_TRUE(touch.touch_down(0, 100, 200));
    EXPECT_TRUE(touch.touch_up(0));

    // Move after up should fail (contact no longer active)
    EXPECT_FALSE(touch.touch_move(0, 300, 400));
}

TEST(AoaHidTouch, TouchUpRejectsInvalidContact) {
    AoaHidTouch touch;
    EXPECT_FALSE(touch.touch_up(5));
    EXPECT_FALSE(touch.touch_up(255));
}

TEST(AoaHidTouch, TouchUpAll) {
    AoaHidTouch touch;
    // Set up multiple contacts
    touch.touch_down(0, 100, 200);
    touch.touch_down(1, 300, 400);
    touch.touch_down(2, 500, 600);

    EXPECT_TRUE(touch.touch_up_all());

    // All contacts should be released
    EXPECT_FALSE(touch.touch_move(0, 0, 0));
    EXPECT_FALSE(touch.touch_move(1, 0, 0));
    EXPECT_FALSE(touch.touch_move(2, 0, 0));
}

// ===========================================================================
// Multiple simultaneous contacts
// ===========================================================================
TEST(AoaHidTouch, MultipleContactsIndependent) {
    AoaHidTouch touch;

    // Two fingers down
    EXPECT_TRUE(touch.touch_down(0, 100, 100));
    EXPECT_TRUE(touch.touch_down(1, 500, 500));

    // Move finger 0 only
    EXPECT_TRUE(touch.touch_move(0, 200, 200));

    // Release finger 0
    EXPECT_TRUE(touch.touch_up(0));

    // Finger 1 still active
    EXPECT_TRUE(touch.touch_move(1, 600, 600));

    // Finger 0 no longer active
    EXPECT_FALSE(touch.touch_move(0, 300, 300));
}

TEST(AoaHidTouch, AllFiveContacts) {
    AoaHidTouch touch;
    for (uint8_t i = 0; i < HID_TOUCH_MAX_CONTACTS; i++) {
        EXPECT_TRUE(touch.touch_down(i, i * 100, i * 200));
    }
    // All 5 can move
    for (uint8_t i = 0; i < HID_TOUCH_MAX_CONTACTS; i++) {
        EXPECT_TRUE(touch.touch_move(i, i * 100 + 50, i * 200 + 50));
    }
    // Release all
    EXPECT_TRUE(touch.touch_up_all());
}

// ===========================================================================
// Registered state management
// ===========================================================================
TEST(AoaHidTouch, InitiallyNotRegistered) {
    AoaHidTouch touch;
    EXPECT_FALSE(touch.is_registered());
}

TEST(AoaHidTouch, MarkUnregistered) {
    AoaHidTouch touch;
    touch.mark_unregistered();
    EXPECT_FALSE(touch.is_registered());
}

// ===========================================================================
// flush without USB returns false
// ===========================================================================
TEST(AoaHidTouch, FlushWithoutUsbReturnsFalse) {
    AoaHidTouch touch;
    touch.touch_down(0, 100, 200);
    // Without USE_LIBUSB, flush always returns false
    EXPECT_FALSE(touch.flush());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
