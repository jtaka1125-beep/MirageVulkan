// =============================================================================
// Unit tests for ADB security functions (src/adb_security.hpp)
// =============================================================================
#include <gtest/gtest.h>
#include "adb_security.hpp"

using namespace gui::security;

// ===========================================================================
// isValidAdbId
// ===========================================================================

TEST(AdbSecurityTest, ValidUsbSerial) {
    EXPECT_TRUE(isValidAdbId("ABCDEF123456"));
    EXPECT_TRUE(isValidAdbId("R5CT123ABCD"));
    EXPECT_TRUE(isValidAdbId("device-1_test"));
}

TEST(AdbSecurityTest, ValidWifiId) {
    EXPECT_TRUE(isValidAdbId("192.168.0.5:5555"));
    EXPECT_TRUE(isValidAdbId("10.0.0.1:39867"));
}

TEST(AdbSecurityTest, InvalidAdbIdEmpty) {
    EXPECT_FALSE(isValidAdbId(""));
}

TEST(AdbSecurityTest, InvalidAdbIdTooLong) {
    std::string long_id(65, 'A');
    EXPECT_FALSE(isValidAdbId(long_id));
}

TEST(AdbSecurityTest, InvalidAdbIdShellInjection) {
    EXPECT_FALSE(isValidAdbId("device; rm -rf /"));
    EXPECT_FALSE(isValidAdbId("$(whoami)"));
    EXPECT_FALSE(isValidAdbId("dev`id`"));
    EXPECT_FALSE(isValidAdbId("dev|cat /etc/passwd"));
    EXPECT_FALSE(isValidAdbId("dev&background"));
}

TEST(AdbSecurityTest, InvalidAdbIdSpecialChars) {
    EXPECT_FALSE(isValidAdbId("dev ice")); // space
    EXPECT_FALSE(isValidAdbId("dev\nice")); // newline
}

// ===========================================================================
// sanitizeCommand
// ===========================================================================

TEST(AdbSecurityTest, SanitizeValidCommands) {
    EXPECT_EQ(sanitizeCommand("input tap 100 200"), "input tap 100 200");
    EXPECT_EQ(sanitizeCommand("screencap -p /sdcard/screen.png"),
              "screencap -p /sdcard/screen.png");
}

TEST(AdbSecurityTest, SanitizeEmptyCommand) {
    EXPECT_EQ(sanitizeCommand(""), "");
}

TEST(AdbSecurityTest, SanitizeCommandSubstitution) {
    EXPECT_EQ(sanitizeCommand("echo $(whoami)"), "");
    EXPECT_EQ(sanitizeCommand("echo `id`"), "");
}

TEST(AdbSecurityTest, SanitizeDestructiveCommands) {
    EXPECT_EQ(sanitizeCommand("ls; rm -rf /"), "");
    EXPECT_EQ(sanitizeCommand("cat; dd if=/dev/zero"), "");
}

TEST(AdbSecurityTest, SanitizePipeToShell) {
    EXPECT_EQ(sanitizeCommand("cat file | sh"), "");
    EXPECT_EQ(sanitizeCommand("cat file | bash"), "");
}

TEST(AdbSecurityTest, SanitizeRedirectionToRoot) {
    EXPECT_EQ(sanitizeCommand("echo x > /etc/passwd"), "");
}

// ===========================================================================
// escapeShellArg
// ===========================================================================

TEST(AdbSecurityTest, EscapePlainString) {
    EXPECT_EQ(escapeShellArg("hello"), "hello");
}

TEST(AdbSecurityTest, EscapeMetacharacters) {
    std::string result = escapeShellArg("a;b|c");
    EXPECT_NE(result, "a;b|c"); // must be escaped
    // Semicolon and pipe should be preceded by backslash
    EXPECT_NE(result.find("\\;"), std::string::npos);
    EXPECT_NE(result.find("\\|"), std::string::npos);
}

// ===========================================================================
// isAllowedRemotePath
// ===========================================================================

TEST(AdbSecurityTest, AllowedPaths) {
    EXPECT_TRUE(isAllowedRemotePath("/data/local/tmp/mirage.apk"));
    EXPECT_TRUE(isAllowedRemotePath("/sdcard/screenshot.png"));
}

TEST(AdbSecurityTest, DisallowedPaths) {
    EXPECT_FALSE(isAllowedRemotePath(""));
    EXPECT_FALSE(isAllowedRemotePath("/etc/passwd"));
    EXPECT_FALSE(isAllowedRemotePath("/system/bin/sh"));
    EXPECT_FALSE(isAllowedRemotePath("/data/data/com.app/files"));
}

TEST(AdbSecurityTest, PathTraversalBlocked) {
    // These technically start with allowed prefix but contain metacharacters
    EXPECT_FALSE(isAllowedRemotePath("/data/local/tmp/$(rm -rf /)"));
    EXPECT_FALSE(isAllowedRemotePath("/sdcard/file;rm"));
}

// ===========================================================================
// classifyConnectionString
// ===========================================================================

TEST(AdbSecurityTest, ClassifyUsb) {
    EXPECT_EQ(classifyConnectionString("ABCDEF123456"), "usb");
    EXPECT_EQ(classifyConnectionString("R5CT900ABCD"), "usb");
}

TEST(AdbSecurityTest, ClassifyWifi) {
    EXPECT_EQ(classifyConnectionString("192.168.0.5:5555"), "wifi");
    EXPECT_EQ(classifyConnectionString("10.0.0.1:39867"), "wifi");
}

TEST(AdbSecurityTest, ClassifyAmbiguousColon) {
    // Has colon but no dots -> usb
    EXPECT_EQ(classifyConnectionString("emulator:5554"), "usb");
}

// ===========================================================================
// extractIp
// ===========================================================================

TEST(AdbSecurityTest, ExtractIpFromWifi) {
    EXPECT_EQ(extractIp("192.168.0.5:5555"), "192.168.0.5");
    EXPECT_EQ(extractIp("10.0.0.1:39867"), "10.0.0.1");
}

TEST(AdbSecurityTest, ExtractIpFromUsb) {
    EXPECT_EQ(extractIp("ABCDEF123456"), "");
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
