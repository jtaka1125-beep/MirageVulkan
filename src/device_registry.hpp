#pragma once
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <functional>
#include <cstdint>

namespace gui {

// =============================================================================
// DeviceEntity: 端末1台の全情報を一塊で管理
// =============================================================================
struct DeviceEntity {
    // --- 不変ID ---
    std::string hardware_id;        // android_id ベースハッシュ (一意キー)
    std::string display_name;       // "RebotAi A9"
    std::string model;              // "A9"
    std::string manufacturer;       // "RebotAi"

    // --- ADB接続 ---
    std::string adb_usb_id;         // "adb-A9250700956-xxx" (USB ADB, 空=未接続)
    std::string adb_wifi_id;        // "192.168.0.6:5555" (WiFi ADB, 空=未接続)
    std::string usb_serial;         // "A9250700956" (USB物理シリアル)
    std::string ip_address;         // "192.168.0.6"

    // --- USB AOA ---
    bool aoa_connected = false;
    int aoa_version = -1;           // -1=unchecked, 0=not supported, 1=v1, 2=v2(HID)

    // --- 映像チャネル ---
    int video_port = 0;             // multi_receiver の UDPポート

    enum class VideoRoute : uint8_t { USB = 0, WiFi = 1 };
    VideoRoute video_route = VideoRoute::USB;

    // --- 操作チャネル ---
    enum class ControlRoute : uint8_t { USB = 0, WiFi_ADB = 1 };
    ControlRoute control_route = ControlRoute::USB;

    // --- 状態 ---
    int target_fps = 60;
    bool is_main = false;

    enum class Status : uint8_t {
        Disconnected = 0,
        Connecting,
        AdbOnly,        // ADB接続のみ (AOAなし)
        AoaActive,      // AOA接続中
        Mirroring,      // 映像ストリーミング中
    };
    Status status = Status::Disconnected;

    // --- 統計 ---
    float current_fps = 0.0f;
    float bandwidth_mbps = 0.0f;

    // --- ヘルパー ---
    bool hasUsb() const { return !adb_usb_id.empty() || aoa_connected; }
    bool hasWifi() const { return !adb_wifi_id.empty(); }
    bool hasAnyConnection() const { return hasUsb() || hasWifi(); }
    std::string preferredAdbId() const {
        return !adb_usb_id.empty() ? adb_usb_id : adb_wifi_id;
    }
};

// =============================================================================
// DeviceRegistry: 全端末の中央レジストリ
// =============================================================================
class DeviceRegistry {
public:
    DeviceRegistry() = default;
    ~DeviceRegistry() = default;

    // --- 端末登録・検索 ---
    // hardware_id で登録 (既存なら既存を返す)
    DeviceEntity& registerOrUpdate(const std::string& hardware_id);

    // 各種キーで検索 (見つからなければ nullptr)
    DeviceEntity* findByHardwareId(const std::string& hw_id);
    const DeviceEntity* findByHardwareId(const std::string& hw_id) const;
    DeviceEntity* findByUsbSerial(const std::string& serial);
    DeviceEntity* findByAdbId(const std::string& adb_id);
    DeviceEntity* findByPort(int video_port);

    // 全端末取得
    std::vector<DeviceEntity> getAllDevices() const;
    std::vector<std::string> getAllHardwareIds() const;
    size_t deviceCount() const;

    // --- 接続情報更新 ---
    void setAdbUsb(const std::string& hw_id, const std::string& adb_id, const std::string& usb_serial = "");
    void setAdbWifi(const std::string& hw_id, const std::string& adb_id, const std::string& ip = "");
    void setAoaConnected(const std::string& hw_id, bool connected);
    void setVideoPort(const std::string& hw_id, int port);

    // --- 状態変更 ---
    void setVideoRoute(const std::string& hw_id, DeviceEntity::VideoRoute route);
    void setControlRoute(const std::string& hw_id, DeviceEntity::ControlRoute route);
    void setTargetFps(const std::string& hw_id, int fps);
    void setMainDevice(const std::string& hw_id);
    void setStatus(const std::string& hw_id, DeviceEntity::Status status);
    void updateStats(const std::string& hw_id, float fps, float bandwidth);

    // --- メインデバイス ---
    std::string getMainDeviceId() const;

    // --- 変更通知 ---
    using ChangeCallback = std::function<void(const std::string& hw_id, const std::string& field)>;
    void setChangeCallback(ChangeCallback cb);

    // --- デバッグ ---
    void dump() const;

private:
    void notify(const std::string& hw_id, const std::string& field);

    mutable std::mutex mutex_;
    std::map<std::string, DeviceEntity> devices_;           // hw_id -> entity
    std::map<std::string, std::string> usb_serial_map_;     // usb_serial -> hw_id
    std::map<std::string, std::string> adb_id_map_;         // adb_id -> hw_id
    std::map<int, std::string> port_map_;                   // port -> hw_id
    std::string main_device_id_;
    ChangeCallback change_cb_;
};

} // namespace gui
