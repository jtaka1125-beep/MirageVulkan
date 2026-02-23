// =============================================================================
// Unit tests for WinUsbChecker (src/winusb_checker.hpp / winusb_checker.cpp)
// Tests only the pure/testable functions:
//   - isAndroidVid()
//   - parseDeviceOutput()
//   - buildDiagnosticSummary()
// Does NOT call checkDevices() or anyDeviceNeedsWinUsb() (require PowerShell).
// WUC-1 through WUC-14
// =============================================================================
#include <gtest/gtest.h>
#include "winusb_checker.hpp"

using namespace mirage;

// ---------------------------------------------------------------------------
// WUC-1: isAndroidVid — known Google VID
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, IsAndroidVid_Google) {
    EXPECT_TRUE(WinUsbChecker::isAndroidVid("18D1"));
}

// ---------------------------------------------------------------------------
// WUC-2: isAndroidVid — Samsung VID
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, IsAndroidVid_Samsung) {
    EXPECT_TRUE(WinUsbChecker::isAndroidVid("04E8"));
}

// ---------------------------------------------------------------------------
// WUC-3: isAndroidVid — MediaTek VID
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, IsAndroidVid_MediaTek) {
    EXPECT_TRUE(WinUsbChecker::isAndroidVid("0E8D"));
}

// ---------------------------------------------------------------------------
// WUC-4: isAndroidVid — unknown VID returns false
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, IsAndroidVid_Unknown) {
    EXPECT_FALSE(WinUsbChecker::isAndroidVid("DEAD"));
    EXPECT_FALSE(WinUsbChecker::isAndroidVid("FFFF"));
    EXPECT_FALSE(WinUsbChecker::isAndroidVid("0000"));
}

// ---------------------------------------------------------------------------
// WUC-5: parseDeviceOutput — empty input
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseEmpty) {
    auto result = WinUsbChecker::parseDeviceOutput("");
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// WUC-6: parseDeviceOutput — single device, WinUSB already installed
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseSingleDevice_WinUSBInstalled) {
    std::string raw = "18D1|4EE7|Pixel 6|USB\\VID_18D1&PID_4EE7\\123456|WinUSB\n";
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].vid, "18D1");
    EXPECT_EQ(result[0].pid, "4EE7");
    EXPECT_EQ(result[0].name, "Pixel 6");
    EXPECT_EQ(result[0].current_driver, "WinUSB");
    EXPECT_FALSE(result[0].needs_winusb);  // already installed
}

// ---------------------------------------------------------------------------
// WUC-7: parseDeviceOutput — device needs WinUSB (driver = usbccgp)
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseSingleDevice_NeedsWinUSB) {
    std::string raw = "04E8|6860|Galaxy S23|USB\\VID_04E8&PID_6860\\ABCDEF|usbccgp\n";
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].current_driver, "usbccgp");
    EXPECT_TRUE(result[0].needs_winusb);
}

// ---------------------------------------------------------------------------
// WUC-8: parseDeviceOutput — driver field missing → "None", needs WinUSB
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseDevice_NoDriverField) {
    std::string raw = "0E8D|2008|MediaTek|USB\\VID_0E8D&PID_2008\\999\n";
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0].current_driver, "None");
    EXPECT_TRUE(result[0].needs_winusb);
}

// ---------------------------------------------------------------------------
// WUC-9: parseDeviceOutput — non-Android VID filtered out
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseNonAndroidFiltered) {
    std::string raw = "DEAD|BEEF|Some Device|USB\\VID_DEAD&PID_BEEF\\0|usbccgp\n";
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// WUC-10: parseDeviceOutput — multiple lines, mixed
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseMultipleLines) {
    std::string raw =
        "18D1|4EE7|Pixel 6|inst1|WinUSB\n"
        "04E8|6860|Galaxy S23|inst2|usbccgp\n"
        "DEAD|BEEF|Junk|inst3|None\n";    // non-Android, filtered
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    ASSERT_EQ(result.size(), 2u);
    EXPECT_FALSE(result[0].needs_winusb);   // Pixel 6 has WinUSB
    EXPECT_TRUE(result[1].needs_winusb);    // Galaxy needs WinUSB
}

// ---------------------------------------------------------------------------
// WUC-11: parseDeviceOutput — too-short lines skipped
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, ParseShortLineSkipped) {
    std::string raw = "18D1|4EE7\n";  // only 2 fields, need ≥4
    auto result = WinUsbChecker::parseDeviceOutput(raw);
    EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// WUC-12: buildDiagnosticSummary — empty list
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, DiagnosticEmpty) {
    std::vector<WinUsbChecker::UsbDeviceStatus> devices;
    std::string summary = WinUsbChecker::buildDiagnosticSummary(devices);
    EXPECT_FALSE(summary.empty());
    EXPECT_NE(summary.find("No"), std::string::npos);
}

// ---------------------------------------------------------------------------
// WUC-13: buildDiagnosticSummary — all OK
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, DiagnosticAllOk) {
    WinUsbChecker::UsbDeviceStatus d;
    d.vid = "18D1"; d.pid = "4EE7"; d.name = "Pixel";
    d.current_driver = "WinUSB"; d.needs_winusb = false;
    std::string summary = WinUsbChecker::buildDiagnosticSummary({d});
    EXPECT_NE(summary.find("1"), std::string::npos);  // 1 device OK
    EXPECT_EQ(summary.find("need"), std::string::npos);  // no "need"
}

// ---------------------------------------------------------------------------
// WUC-14: buildDiagnosticSummary — one needs WinUSB
// ---------------------------------------------------------------------------
TEST(WinUsbCheckerTest, DiagnosticNeedsWinUSB) {
    WinUsbChecker::UsbDeviceStatus d;
    d.vid = "04E8"; d.pid = "6860"; d.name = "Galaxy";
    d.current_driver = "usbccgp"; d.needs_winusb = true;
    std::string summary = WinUsbChecker::buildDiagnosticSummary({d});
    EXPECT_NE(summary.find("1 need"), std::string::npos);
    EXPECT_NE(summary.find("Galaxy"), std::string::npos);
}
