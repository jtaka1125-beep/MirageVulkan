#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
#include <queue>
#include <memory>
#include <condition_variable>

#ifdef USE_LIBUSB
#include <libusb-1.0/libusb.h>
#include "mirage_protocol.hpp"
#endif

namespace gui {

/**
 * Multi-device USB AOA Command Sender
 * Handles multiple Android devices via USB AOA protocol.
 * Each device is identified by USB bus:address or serial number.
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Callbacks are invoked from internal threads; avoid blocking operations
 * - Use stop() before destruction to ensure clean shutdown
 *
 * Error Handling:
 * - Methods return false/0 on failure
 * - Detailed errors are logged to stderr
 * - Device disconnect is handled gracefully with automatic cleanup
 */
class MultiUsbCommandSender {
public:
    struct DeviceInfo {
        std::string usb_id;          // Unique USB ID (bus:addr or serial)
        std::string serial;          // USB serial string (if available)
        uint8_t bus = 0;
        uint8_t address = 0;
        bool connected = false;
        uint64_t commands_sent = 0;
        uint64_t acks_received = 0;
        uint64_t errors = 0;         // Total error count for this device
        uint64_t bytes_received = 0; // Total bytes received
    };

    // Error callback type for external error handling
    using ErrorCallback = std::function<void(const std::string& usb_id, int error_code, const std::string& message)>;

    using AckCallback = std::function<void(const std::string& usb_id, uint32_t seq, uint8_t status)>;
    using VideoDataCallback = std::function<void(const std::string& usb_id, const uint8_t* data, size_t len)>;
    using AudioCallback = std::function<void(const std::string& usb_id, const uint8_t* payload, size_t len, uint32_t timestamp)>;

    MultiUsbCommandSender();
    ~MultiUsbCommandSender();

    // Start/stop sender (scans for all AOA devices)
    bool start();
    void stop();

    // Rescan for new devices (can be called while running)
    void rescan();

    bool running() const { return running_.load(); }

    // Get connected device count
    int device_count() const;

    // Get list of connected device IDs
    std::vector<std::string> get_device_ids() const;

    // Get device info
    bool get_device_info(const std::string& usb_id, DeviceInfo& out) const;

    // Check if specific device is connected
    bool is_device_connected(const std::string& usb_id) const;

    // Set callback for ACK responses
    void set_ack_callback(AckCallback cb) { ack_callback_ = cb; }

    // Set callback for video data (from first device's ep_in)
    void set_video_callback(VideoDataCallback cb) { video_callback_ = cb; }

    // Set callback for audio data (from USB audio frames)
    void set_audio_callback(AudioCallback cb) { audio_callback_ = cb; }

    // Set callback for error notifications
    void set_error_callback(ErrorCallback cb) { error_callback_ = cb; }

    // Callback invoked after AOA strings are sent but BEFORE AOA_START_ACCESSORY
    // This is where HID devices must be registered (AOA v2 requirement)
    using PreStartCallback = std::function<bool(libusb_device_handle* handle, int aoa_version)>;
    void set_pre_start_callback(PreStartCallback cb) { pre_start_callback_ = cb; }

    // Callback invoked after a device is opened post re-enumeration
    using DeviceOpenedCallback = std::function<void(const std::string& usb_id, libusb_device_handle* handle)>;
    void set_device_opened_callback(DeviceOpenedCallback cb) { device_opened_callback_ = cb; }

    // Callback invoked when a device is disconnected (for HID cleanup etc.)
    using DeviceClosedCallback = std::function<void(const std::string& usb_id)>;
    void set_device_closed_callback(DeviceClosedCallback cb) { device_closed_callback_ = cb; }

    // Get total error statistics
    struct ErrorStats {
        uint64_t total_errors = 0;
        uint64_t io_errors = 0;
        uint64_t timeout_errors = 0;
        uint64_t disconnects = 0;
    };
    ErrorStats get_error_stats() const;

    // Total bytes received via USB bulk transfer
    uint64_t total_bytes_received() const { return total_bytes_received_.load(); }

    // Send commands to specific device (returns sequence number, 0 on error)
    uint32_t send_ping(const std::string& usb_id);
    uint32_t send_tap(const std::string& usb_id, int x, int y, int screen_w = 0, int screen_h = 0);
    uint32_t send_swipe(const std::string& usb_id, int x1, int y1, int x2, int y2, int duration_ms = 300);
    uint32_t send_back(const std::string& usb_id);
    uint32_t send_key(const std::string& usb_id, int keycode);
    uint32_t send_click_id(const std::string& usb_id, const std::string& resource_id);
    uint32_t send_click_text(const std::string& usb_id, const std::string& text);

    // Video control commands
    uint32_t send_video_fps(const std::string& usb_id, int fps);
    uint32_t send_video_route(const std::string& usb_id, uint8_t mode, const std::string& host, int port);
    uint32_t send_video_idr(const std::string& usb_id);

    // Send commands to ALL connected devices (returns number of devices sent to)
    int send_tap_all(int x, int y, int screen_w = 0, int screen_h = 0);
    int send_swipe_all(int x1, int y1, int x2, int y2, int duration_ms = 300);
    int send_back_all();
    int send_key_all(int keycode);

    // Quick AOA version check without mode switch
    int check_aoa_version(libusb_device* dev);

    // Get first device ID (for backward compatibility)
    std::string get_first_device_id() const;

private:
#ifdef USE_LIBUSB
    struct DeviceHandle {
        DeviceInfo info;
        libusb_device_handle* handle = nullptr;
        uint8_t ep_out = 0;
        uint8_t ep_in = 0;
        std::queue<std::vector<uint8_t>> command_queue;
        std::mutex queue_mutex;
        std::atomic<uint32_t> next_seq{1};
        std::thread recv_thread;  // Per-device receive thread
        std::atomic<bool> recv_running{false};
    };

    bool find_and_open_all_devices();
    bool open_aoa_device(libusb_device* dev, uint16_t pid);
    bool switch_device_to_aoa_mode(libusb_device* dev);
    int get_aoa_protocol_version(libusb_device_handle* handle);
    bool send_aoa_string(libusb_device_handle* handle, uint16_t index, const char* str);
    std::string get_usb_serial(libusb_device_handle* handle, libusb_device_descriptor& desc);
    std::string make_usb_id(libusb_device* dev, const std::string& serial);

    void send_thread();
    void receive_thread();  // Legacy (unused)
    void device_receive_thread(const std::string& device_id);  // Per-device receive

    bool send_raw(DeviceHandle& device, const uint8_t* data, size_t len);
    std::vector<uint8_t> build_packet(DeviceHandle& device, uint8_t cmd, const uint8_t* payload, size_t payload_len);
    uint32_t queue_command(const std::string& usb_id, uint8_t cmd, const uint8_t* payload, size_t payload_len);

    libusb_context* ctx_ = nullptr;
    std::map<std::string, std::unique_ptr<DeviceHandle>> devices_;
    mutable std::mutex devices_mutex_;
#endif

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};  // Prevent concurrent stop() calls
    std::thread send_thread_;
    std::thread recv_thread_;

    AckCallback ack_callback_;
    VideoDataCallback video_callback_;
    AudioCallback audio_callback_;
    ErrorCallback error_callback_;
    PreStartCallback pre_start_callback_;
    DeviceOpenedCallback device_opened_callback_;
    DeviceClosedCallback device_closed_callback_;

    // Error statistics (atomic for thread-safe reads)
    mutable std::atomic<uint64_t> total_bytes_received_{0};
    mutable std::atomic<uint64_t> total_errors_{0};
    mutable std::atomic<uint64_t> io_errors_{0};
    mutable std::atomic<uint64_t> timeout_errors_{0};
    mutable std::atomic<uint64_t> disconnects_{0};

    // Condition variable for clean shutdown signaling
    std::condition_variable shutdown_cv_;
    std::mutex shutdown_mutex_;
};

} // namespace gui
