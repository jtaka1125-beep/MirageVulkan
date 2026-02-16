// =============================================================================
// MirageVulkan - AdbDeviceManager Unit Tests
// =============================================================================
// Tests device management logic without actual ADB execution

#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <map>
#include <regex>

// =============================================================================
// Mock/Testable versions of AdbDeviceManager logic
// =============================================================================

namespace test {

enum class ConnectionType { USB, WiFi, Unknown };

struct DeviceInfo {
    std::string adb_id;
    std::string hardware_id;
    std::string model;
    std::string manufacturer;
    ConnectionType conn_type;
    std::string ip_address;
    bool is_online = false;

    std::string unique_key() const { return hardware_id.empty() ? adb_id : hardware_id; }
};

// Determine connection type from ADB ID
ConnectionType determineConnectionType(const std::string& adb_id) {
    // WiFi: IP:port format (e.g., "192.168.0.10:5555")
    std::regex wifi_pattern(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d+$)");
    if (std::regex_match(adb_id, wifi_pattern)) {
        return ConnectionType::WiFi;
    }

    // mDNS format: "adb-SERIAL-hash._adb-tls-connect._tcp"
    if (adb_id.find("._adb-tls-connect._tcp") != std::string::npos) {
        return ConnectionType::WiFi;
    }

    // USB: everything else (serial number)
    if (!adb_id.empty()) {
        return ConnectionType::USB;
    }

    return ConnectionType::Unknown;
}

// Extract IP from WiFi ADB ID
std::string extractIp(const std::string& adb_id) {
    // IP:port format
    size_t colon = adb_id.find(':');
    if (colon != std::string::npos) {
        std::string potential_ip = adb_id.substr(0, colon);
        std::regex ip_pattern(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");
        if (std::regex_match(potential_ip, ip_pattern)) {
            return potential_ip;
        }
    }
    return "";
}

// Extract serial from mDNS format
std::string extractSerialFromMdns(const std::string& adb_id) {
    // Format: "adb-SERIAL-hash._adb-tls-connect._tcp"
    if (adb_id.find("adb-") == 0) {
        size_t dash2 = adb_id.find('-', 4);
        if (dash2 != std::string::npos) {
            return adb_id.substr(4, dash2 - 4);
        }
    }
    return "";
}

// Parse `adb devices` output
std::vector<std::pair<std::string, std::string>> parseAdbDevicesOutput(const std::string& output) {
    std::vector<std::pair<std::string, std::string>> result;
    std::istringstream stream(output);
    std::string line;

    while (std::getline(stream, line)) {
        // Skip header line
        if (line.find("List of devices") != std::string::npos) continue;
        if (line.empty()) continue;

        // Parse "device_id\tstate"
        size_t tab = line.find('\t');
        if (tab != std::string::npos) {
            std::string device_id = line.substr(0, tab);
            std::string state = line.substr(tab + 1);

            // Trim whitespace
            while (!state.empty() && (state.back() == '\r' || state.back() == ' ')) {
                state.pop_back();
            }

            result.push_back({device_id, state});
        }
    }

    return result;
}

// Port assignment logic
std::map<std::string, int> assignPorts(const std::vector<std::string>& hardware_ids, int base_port) {
    std::map<std::string, int> result;
    int port = base_port;
    for (const auto& hw_id : hardware_ids) {
        result[hw_id] = port++;
    }
    return result;
}

} // namespace test

// =============================================================================
// Tests
// =============================================================================

class AdbDeviceManagerTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// =============================================================================
// Test: Connection Type Detection
// =============================================================================

TEST_F(AdbDeviceManagerTest, DetectUsbConnection) {
    EXPECT_EQ(test::determineConnectionType("A9250700956"), test::ConnectionType::USB);
    EXPECT_EQ(test::determineConnectionType("emulator-5554"), test::ConnectionType::USB);
    EXPECT_EQ(test::determineConnectionType("R3CT40XXXXX"), test::ConnectionType::USB);
}

TEST_F(AdbDeviceManagerTest, DetectWifiConnection) {
    EXPECT_EQ(test::determineConnectionType("192.168.0.10:5555"), test::ConnectionType::WiFi);
    EXPECT_EQ(test::determineConnectionType("10.0.0.5:5555"), test::ConnectionType::WiFi);
    EXPECT_EQ(test::determineConnectionType("192.168.1.100:37645"), test::ConnectionType::WiFi);
}

TEST_F(AdbDeviceManagerTest, DetectMdnsConnection) {
    EXPECT_EQ(test::determineConnectionType("adb-A9250700956-abc123._adb-tls-connect._tcp"),
              test::ConnectionType::WiFi);
}

TEST_F(AdbDeviceManagerTest, DetectUnknownConnection) {
    EXPECT_EQ(test::determineConnectionType(""), test::ConnectionType::Unknown);
}

// =============================================================================
// Test: IP Extraction
// =============================================================================

TEST_F(AdbDeviceManagerTest, ExtractIpFromWifiId) {
    EXPECT_EQ(test::extractIp("192.168.0.10:5555"), "192.168.0.10");
    EXPECT_EQ(test::extractIp("10.0.0.5:5555"), "10.0.0.5");
    EXPECT_EQ(test::extractIp("192.168.1.100:37645"), "192.168.1.100");
}

TEST_F(AdbDeviceManagerTest, ExtractIpFromUsbId) {
    EXPECT_EQ(test::extractIp("A9250700956"), "");
    EXPECT_EQ(test::extractIp("emulator-5554"), "");
}

// =============================================================================
// Test: Serial Extraction from mDNS
// =============================================================================

TEST_F(AdbDeviceManagerTest, ExtractSerialFromMdns) {
    EXPECT_EQ(test::extractSerialFromMdns("adb-A9250700956-abc123._adb-tls-connect._tcp"),
              "A9250700956");
    EXPECT_EQ(test::extractSerialFromMdns("adb-R3CT40XXXXX-def456._adb-tls-connect._tcp"),
              "R3CT40XXXXX");
}

TEST_F(AdbDeviceManagerTest, ExtractSerialFromNonMdns) {
    EXPECT_EQ(test::extractSerialFromMdns("192.168.0.10:5555"), "");
    EXPECT_EQ(test::extractSerialFromMdns("A9250700956"), "");
}

// =============================================================================
// Test: Parse ADB Devices Output
// =============================================================================

TEST_F(AdbDeviceManagerTest, ParseAdbDevicesOutput) {
    std::string output =
        "List of devices attached\n"
        "A9250700956\tdevice\n"
        "192.168.0.10:5555\tdevice\n"
        "R3CT40XXXXX\toffline\n";

    auto devices = test::parseAdbDevicesOutput(output);

    ASSERT_EQ(devices.size(), 3u);
    EXPECT_EQ(devices[0].first, "A9250700956");
    EXPECT_EQ(devices[0].second, "device");
    EXPECT_EQ(devices[1].first, "192.168.0.10:5555");
    EXPECT_EQ(devices[1].second, "device");
    EXPECT_EQ(devices[2].first, "R3CT40XXXXX");
    EXPECT_EQ(devices[2].second, "offline");
}

TEST_F(AdbDeviceManagerTest, ParseEmptyOutput) {
    std::string output = "List of devices attached\n";
    auto devices = test::parseAdbDevicesOutput(output);
    EXPECT_TRUE(devices.empty());
}

TEST_F(AdbDeviceManagerTest, ParseWithCarriageReturn) {
    std::string output =
        "List of devices attached\r\n"
        "A9250700956\tdevice\r\n";

    auto devices = test::parseAdbDevicesOutput(output);

    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(devices[0].first, "A9250700956");
    EXPECT_EQ(devices[0].second, "device");
}

// =============================================================================
// Test: Port Assignment
// =============================================================================

TEST_F(AdbDeviceManagerTest, AssignPorts) {
    std::vector<std::string> hw_ids = {"hw_001", "hw_002", "hw_003"};
    auto ports = test::assignPorts(hw_ids, 5000);

    EXPECT_EQ(ports["hw_001"], 5000);
    EXPECT_EQ(ports["hw_002"], 5001);
    EXPECT_EQ(ports["hw_003"], 5002);
}

TEST_F(AdbDeviceManagerTest, AssignPortsEmpty) {
    std::vector<std::string> hw_ids;
    auto ports = test::assignPorts(hw_ids, 5000);
    EXPECT_TRUE(ports.empty());
}

// =============================================================================
// Test: DeviceInfo unique_key
// =============================================================================

TEST_F(AdbDeviceManagerTest, UniqueKeyWithHardwareId) {
    test::DeviceInfo info;
    info.adb_id = "A9250700956";
    info.hardware_id = "android_id_12345";

    EXPECT_EQ(info.unique_key(), "android_id_12345");
}

TEST_F(AdbDeviceManagerTest, UniqueKeyWithoutHardwareId) {
    test::DeviceInfo info;
    info.adb_id = "A9250700956";
    info.hardware_id = "";

    EXPECT_EQ(info.unique_key(), "A9250700956");
}

// =============================================================================
// Test: Duplicate Detection Logic
// =============================================================================

TEST_F(AdbDeviceManagerTest, DuplicateDetectionBySameHardwareId) {
    std::map<std::string, test::DeviceInfo> devices;

    // USB connection
    test::DeviceInfo usb_dev;
    usb_dev.adb_id = "A9250700956";
    usb_dev.hardware_id = "android_id_12345";
    usb_dev.conn_type = test::ConnectionType::USB;

    // WiFi connection (same device)
    test::DeviceInfo wifi_dev;
    wifi_dev.adb_id = "192.168.0.10:5555";
    wifi_dev.hardware_id = "android_id_12345";
    wifi_dev.conn_type = test::ConnectionType::WiFi;

    // Both should map to same unique key
    EXPECT_EQ(usb_dev.unique_key(), wifi_dev.unique_key());
}

// =============================================================================
// Test: IP Address Validation
// =============================================================================

TEST_F(AdbDeviceManagerTest, ValidIpAddresses) {
    std::regex ip_pattern(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");

    EXPECT_TRUE(std::regex_match("192.168.0.1", ip_pattern));
    EXPECT_TRUE(std::regex_match("10.0.0.1", ip_pattern));
    EXPECT_TRUE(std::regex_match("255.255.255.255", ip_pattern));
    EXPECT_TRUE(std::regex_match("0.0.0.0", ip_pattern));
}

TEST_F(AdbDeviceManagerTest, InvalidIpAddresses) {
    std::regex ip_pattern(R"(^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$)");

    EXPECT_FALSE(std::regex_match("192.168.0", ip_pattern));
    EXPECT_FALSE(std::regex_match("192.168.0.1.1", ip_pattern));
    EXPECT_FALSE(std::regex_match("abc.def.ghi.jkl", ip_pattern));
    EXPECT_FALSE(std::regex_match("", ip_pattern));
}

// =============================================================================
// Main
// =============================================================================

int main(int argc, char** argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
