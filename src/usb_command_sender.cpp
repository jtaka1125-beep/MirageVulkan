#include "usb_command_sender.hpp"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"

using namespace mirage::protocol;

namespace gui {

UsbCommandSender::UsbCommandSender() = default;

UsbCommandSender::~UsbCommandSender() {
    stop();
}

#ifdef USE_LIBUSB

bool UsbCommandSender::start() {
    if (running_.load()) return true;

    // Initialize libusb
    if (libusb_init(&ctx_) != LIBUSB_SUCCESS) {
        MLOG_ERROR("usbcmd", "Failed to init libusb");
        return false;
    }

    if (!find_and_open_device()) {
        MLOG_INFO("usbcmd", "No AOA device found");
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    running_.store(true);
    connected_.store(true);

    send_thread_ = std::thread(&UsbCommandSender::send_thread, this);
    recv_thread_ = std::thread(&UsbCommandSender::receive_thread, this);

    MLOG_INFO("usbcmd", "Started USB command sender");
    return true;
}

void UsbCommandSender::stop() {
    running_.store(false);
    connected_.store(false);

    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }

    if (handle_) {
        libusb_release_interface(handle_, 0);
        libusb_close(handle_);
        handle_ = nullptr;
    }

    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }
}

int UsbCommandSender::get_aoa_protocol_version(libusb_device_handle* handle) {
    uint8_t version[2] = {0, 0};
    int ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_GET_PROTOCOL,
        0, 0,
        version, sizeof(version),
        1000
    );
    if (ret < 0) {
        return -1;
    }
    return version[0] | (version[1] << 8);
}

bool UsbCommandSender::send_aoa_string(libusb_device_handle* handle, uint16_t index, const char* str) {
    int ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_SEND_STRING,
        0, index,
        (unsigned char*)str, strlen(str) + 1,
        1000
    );
    return ret >= 0;
}

bool UsbCommandSender::try_switch_android_to_aoa(libusb_device* dev) {
    libusb_device_handle* handle = nullptr;
    int ret = libusb_open(dev, &handle);
    if (ret != LIBUSB_SUCCESS) {
        MLOG_ERROR("usbcmd", "Failed to open device for AOA switch: %s", libusb_error_name(ret));
        return false;
    }

    // Check if device supports AOA
    int aoa_version = get_aoa_protocol_version(handle);
    if (aoa_version < 1) {
        MLOG_INFO("usbcmd", "Device does not support AOA protocol");
        libusb_close(handle);
        return false;
    }
    MLOG_INFO("usbcmd", "Device supports AOA protocol version %d", aoa_version);

    // Send accessory identification strings
    // MUST match Android accessory_filter.xml: manufacturer="Mirage" model="MirageCtl" version="1"
    if (!send_aoa_string(handle, AOA_STRING_MANUFACTURER, "Mirage")) {
        MLOG_ERROR("usbcmd", "Failed to send manufacturer string");
        libusb_close(handle);
        return false;
    }
    if (!send_aoa_string(handle, AOA_STRING_MODEL, "MirageCtl")) {
        MLOG_ERROR("usbcmd", "Failed to send model string");
        libusb_close(handle);
        return false;
    }
    if (!send_aoa_string(handle, AOA_STRING_DESCRIPTION, "Mirage Control Interface")) {
        MLOG_ERROR("usbcmd", "Failed to send description string");
        libusb_close(handle);
        return false;
    }
    if (!send_aoa_string(handle, AOA_STRING_VERSION, "1")) {
        MLOG_ERROR("usbcmd", "Failed to send version string");
        libusb_close(handle);
        return false;
    }
    if (!send_aoa_string(handle, AOA_STRING_URI, "https://github.com/mirage")) {
        MLOG_ERROR("usbcmd", "Failed to send URI string");
        libusb_close(handle);
        return false;
    }
    if (!send_aoa_string(handle, AOA_STRING_SERIAL, "MirageCtl001")) {
        MLOG_ERROR("usbcmd", "Failed to send serial string");
        libusb_close(handle);
        return false;
    }

    // Start accessory mode
    ret = libusb_control_transfer(
        handle,
        LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR,
        AOA_START_ACCESSORY,
        0, 0,
        nullptr, 0,
        1000
    );
    if (ret < 0) {
        MLOG_ERROR("usbcmd", "Failed to start accessory mode: %s", libusb_error_name(ret));
        libusb_close(handle);
        return false;
    }

    MLOG_INFO("usbcmd", "Sent AOA start accessory command, device will re-enumerate");
    libusb_close(handle);
    return true;
}

bool UsbCommandSender::switch_device_to_aoa_mode() {
    libusb_device** devs;
    ssize_t cnt = libusb_get_device_list(ctx_, &devs);
    if (cnt < 0) {
        return false;
    }

    bool switched = false;
    for (ssize_t i = 0; i < cnt && !switched; i++) {
        libusb_device* dev = devs[i];
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(dev, &desc) != 0) continue;

        // Skip if already AOA device (any AOA PID)
        if (desc.idVendor == AOA_VID &&
            (desc.idProduct == AOA_PID_ACCESSORY ||
             desc.idProduct == AOA_PID_ACCESSORY_ADB ||
             desc.idProduct == AOA_PID_AUDIO ||
             desc.idProduct == AOA_PID_AUDIO_ADB ||
             desc.idProduct == AOA_PID_ACCESSORY_AUDIO ||
             desc.idProduct == AOA_PID_ACCESSORY_AUDIO_ADB)) {
            continue;
        }

        // Try to switch Android devices to AOA mode
        // Common Android vendor IDs
        bool is_android = false;
        switch (desc.idVendor) {
            case AOA_VID: // Google
            case 0x04E8: // Samsung
            case 0x22B8: // Motorola
            case 0x0BB4: // HTC
            case 0x12D1: // Huawei
            case 0x2717: // Xiaomi
            case 0x19D2: // ZTE
            case 0x1004: // LG
            case 0x0FCE: // Sony Ericsson
            case 0x2A70: // OnePlus
            case 0x0E8D: // MediaTek (many Chinese devices)
            case 0x1782: // Spreadtrum
            case 0x1F3A: // Allwinner
            case 0x2207: // Rockchip
                is_android = true;
                break;
            default:
                break;
        }

        if (is_android) {
            MLOG_INFO("usbcmd", "Found potential Android device (VID=%04x PID=%04x), attempting AOA switch", desc.idVendor, desc.idProduct);
            if (try_switch_android_to_aoa(dev)) {
                switched = true;
            }
        }
    }

    libusb_free_device_list(devs, 1);
    return switched;
}

bool UsbCommandSender::find_and_open_device() {
    // First, try to find already-AOA devices (all AOA PID variants)
    uint16_t pids[] = {
        AOA_PID_ACCESSORY,           // 0x2D01
        AOA_PID_ACCESSORY_ADB,       // 0x2D00
        AOA_PID_ACCESSORY_AUDIO,     // 0x2D04
        AOA_PID_ACCESSORY_AUDIO_ADB  // 0x2D05
    };

    for (uint16_t pid : pids) {
        handle_ = libusb_open_device_with_vid_pid(ctx_, AOA_VID, pid);
        if (handle_) {
            MLOG_INFO("usbcmd", "Found AOA device (VID=%04x PID=%04x)", AOA_VID, pid);
            break;
        }
    }

    // If no AOA device found, try to switch connected Android devices
    if (!handle_) {
        MLOG_INFO("usbcmd", "No AOA device found, attempting to switch Android devices to AOA mode...");
        if (switch_device_to_aoa_mode()) {
            // Wait for device to re-enumerate after AOA switch
            MLOG_INFO("usbcmd", "Waiting for device to re-enumerate in AOA mode...");
            std::this_thread::sleep_for(std::chrono::milliseconds(3000));

            // Try again to find AOA device
            for (uint16_t pid : pids) {
                handle_ = libusb_open_device_with_vid_pid(ctx_, AOA_VID, pid);
                if (handle_) {
                    MLOG_INFO("usbcmd", "Found AOA device after switch (VID=%04x PID=%04x)", AOA_VID, pid);
                    break;
                }
            }
        }
    }

    if (!handle_) {
        return false;
    }

    // Claim interface 0
    if (libusb_claim_interface(handle_, 0) != LIBUSB_SUCCESS) {
        MLOG_ERROR("usbcmd", "Failed to claim interface");
        libusb_close(handle_);
        handle_ = nullptr;
        return false;
    }

    // Find bulk endpoints
    libusb_device* dev = libusb_get_device(handle_);
    struct libusb_config_descriptor* config;
    if (libusb_get_active_config_descriptor(dev, &config) != LIBUSB_SUCCESS) {
        MLOG_ERROR("usbcmd", "Failed to get config descriptor");
        return false;
    }

    ep_out_ = 0;
    ep_in_ = 0;
    for (int i = 0; i < config->interface[0].altsetting[0].bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor* ep = &config->interface[0].altsetting[0].endpoint[i];
        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK) {
            if ((ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT) {
                ep_out_ = ep->bEndpointAddress;
                MLOG_INFO("usbcmd", "Found bulk OUT endpoint: 0x%02x", ep_out_);
            } else {
                ep_in_ = ep->bEndpointAddress;
                MLOG_INFO("usbcmd", "Found bulk IN endpoint: 0x%02x", ep_in_);
            }
        }
    }

    libusb_free_config_descriptor(config);

    if (ep_out_ == 0) {
        MLOG_INFO("usbcmd", "No bulk OUT endpoint found");
        return false;
    }

    return true;
}

void UsbCommandSender::send_thread() {
    MLOG_INFO("usbcmd", "Send thread started");

    while (running_.load()) {
        std::vector<uint8_t> packet;

        {
            std::lock_guard<std::mutex> lock(queue_mtx_);
            if (!command_queue_.empty()) {
                packet = std::move(command_queue_.front());
                command_queue_.pop();
            }
        }

        if (!packet.empty()) {
            if (send_raw(packet.data(), packet.size())) {
                commands_sent_.fetch_add(1);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    MLOG_INFO("usbcmd", "Send thread ended");
}

void UsbCommandSender::receive_thread() {
    MLOG_INFO("usbcmd", "Receive thread started");

    const int BUFFER_SIZE = 1024;
    uint8_t buf[BUFFER_SIZE];
    int transferred;

    while (running_.load()) {
        if (ep_in_ == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        int ret = libusb_bulk_transfer(handle_, ep_in_, buf, BUFFER_SIZE, &transferred, 500);

        if (ret == LIBUSB_SUCCESS && transferred >= (int)HEADER_SIZE) {
            // Parse header
            uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
            uint8_t version = buf[4];
            uint8_t cmd = buf[5];
            uint32_t seq = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);
            uint32_t payload_len = buf[10] | (buf[11] << 8) | (buf[12] << 16) | (buf[13] << 24);

            if (magic == PROTOCOL_MAGIC && version == PROTOCOL_VERSION) {
                if (cmd == CMD_ACK) {
                    uint8_t status = (transferred >= (int)HEADER_SIZE + 5) ? buf[HEADER_SIZE + 4] : 0;
                    acks_received_.fetch_add(1);

                    if (ack_callback_) {
                        ack_callback_(seq, status);
                    }
                } else if (cmd == CMD_AUDIO_FRAME && payload_len >= 4) {
                    // Audio frame: timestamp (4 bytes) + opus data
                    uint32_t timestamp = buf[HEADER_SIZE] | (buf[HEADER_SIZE + 1] << 8) |
                                        (buf[HEADER_SIZE + 2] << 16) | (buf[HEADER_SIZE + 3] << 24);
                    const uint8_t* audio_data = buf + HEADER_SIZE;
                    size_t audio_len = payload_len;

                    if (audio_callback_ && transferred >= (int)(HEADER_SIZE + payload_len)) {
                        audio_callback_(audio_data, audio_len, timestamp);
                    }
                }
            }
        } else if (ret == LIBUSB_ERROR_TIMEOUT) {
            // Timeout is normal
        } else if (ret != LIBUSB_SUCCESS) {
            MLOG_ERROR("usbcmd", "USB receive error: %d", ret);
            connected_.store(false);
            break;
        }
    }

    MLOG_INFO("usbcmd", "Receive thread ended");
}

bool UsbCommandSender::send_raw(const uint8_t* data, size_t len) {
    if (!handle_ || ep_out_ == 0) return false;

    int transferred = 0;
    int ret = libusb_bulk_transfer(handle_, ep_out_, const_cast<uint8_t*>(data), (int)len, &transferred, 1000);

    if (ret != LIBUSB_SUCCESS) {
        MLOG_ERROR("usbcmd", "USB send error: %d", ret);
        return false;
    }

    if (transferred != (int)len) {
        MLOG_INFO("usbcmd", "Partial transfer: sent %d of %zu bytes", transferred, len);
        return false;
    }

    return true;
}

std::vector<uint8_t> UsbCommandSender::build_packet(uint8_t cmd, const uint8_t* payload, size_t payload_len) {
    // Validate payload_len fits in protocol header (uint32_t)
    if (payload_len > UINT32_MAX) {
        MLOG_INFO("usbcmd", "Payload too large: %zu bytes (max %u)", payload_len, UINT32_MAX);
        return {};
    }

    std::vector<uint8_t> packet(HEADER_SIZE + payload_len);

    uint32_t seq = next_seq_.fetch_add(1);

    // Header (little endian)
    packet[0] = PROTOCOL_MAGIC & 0xFF;
    packet[1] = (PROTOCOL_MAGIC >> 8) & 0xFF;
    packet[2] = (PROTOCOL_MAGIC >> 16) & 0xFF;
    packet[3] = (PROTOCOL_MAGIC >> 24) & 0xFF;
    packet[4] = PROTOCOL_VERSION;
    packet[5] = cmd;
    packet[6] = seq & 0xFF;
    packet[7] = (seq >> 8) & 0xFF;
    packet[8] = (seq >> 16) & 0xFF;
    packet[9] = (seq >> 24) & 0xFF;
    packet[10] = payload_len & 0xFF;
    packet[11] = (payload_len >> 8) & 0xFF;
    packet[12] = (payload_len >> 16) & 0xFF;
    packet[13] = (payload_len >> 24) & 0xFF;

    if (payload && payload_len > 0) {
        memcpy(packet.data() + HEADER_SIZE, payload, payload_len);
    }

    return packet;
}

uint32_t UsbCommandSender::send_ping() {
    auto packet = build_packet(CMD_PING, nullptr, 0);
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));
    return seq;
}

uint32_t UsbCommandSender::send_tap(int x, int y, int screen_w, int screen_h) {
    // Payload: x(4) + y(4) + w(4) + h(4) + flags(4) = 20 bytes
    uint8_t payload[20];

    // Little endian
    payload[0] = x & 0xFF;
    payload[1] = (x >> 8) & 0xFF;
    payload[2] = (x >> 16) & 0xFF;
    payload[3] = (x >> 24) & 0xFF;

    payload[4] = y & 0xFF;
    payload[5] = (y >> 8) & 0xFF;
    payload[6] = (y >> 16) & 0xFF;
    payload[7] = (y >> 24) & 0xFF;

    payload[8] = screen_w & 0xFF;
    payload[9] = (screen_w >> 8) & 0xFF;
    payload[10] = (screen_w >> 16) & 0xFF;
    payload[11] = (screen_w >> 24) & 0xFF;

    payload[12] = screen_h & 0xFF;
    payload[13] = (screen_h >> 8) & 0xFF;
    payload[14] = (screen_h >> 16) & 0xFF;
    payload[15] = (screen_h >> 24) & 0xFF;

    // flags = 0
    payload[16] = 0;
    payload[17] = 0;
    payload[18] = 0;
    payload[19] = 0;

    auto packet = build_packet(CMD_TAP, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued TAP(%d, %d) seq=%u", x, y, seq);
    return seq;
}

uint32_t UsbCommandSender::send_swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    // Payload: x1(4) + y1(4) + x2(4) + y2(4) + duration(4) + flags(4) = 24 bytes
    // Android Protocol.kt は payloadLen >= 24 を要求する
    uint8_t payload[24];

    payload[0] = x1 & 0xFF;
    payload[1] = (x1 >> 8) & 0xFF;
    payload[2] = (x1 >> 16) & 0xFF;
    payload[3] = (x1 >> 24) & 0xFF;

    payload[4] = y1 & 0xFF;
    payload[5] = (y1 >> 8) & 0xFF;
    payload[6] = (y1 >> 16) & 0xFF;
    payload[7] = (y1 >> 24) & 0xFF;

    payload[8] = x2 & 0xFF;
    payload[9] = (x2 >> 8) & 0xFF;
    payload[10] = (x2 >> 16) & 0xFF;
    payload[11] = (x2 >> 24) & 0xFF;

    payload[12] = y2 & 0xFF;
    payload[13] = (y2 >> 8) & 0xFF;
    payload[14] = (y2 >> 16) & 0xFF;
    payload[15] = (y2 >> 24) & 0xFF;

    payload[16] = duration_ms & 0xFF;
    payload[17] = (duration_ms >> 8) & 0xFF;
    payload[18] = (duration_ms >> 16) & 0xFF;
    payload[19] = (duration_ms >> 24) & 0xFF;

    // flags = 0 (reserved)
    payload[20] = 0;
    payload[21] = 0;
    payload[22] = 0;
    payload[23] = 0;

    auto packet = build_packet(CMD_SWIPE, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued SWIPE(%d,%d)->(%d,%d) seq=%u", x1, y1, x2, y2, seq);
    return seq;
}

uint32_t UsbCommandSender::send_back() {
    // Payload: flags(4) = 4 bytes
    uint8_t payload[4] = {0, 0, 0, 0};

    auto packet = build_packet(CMD_BACK, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued BACK seq=%u", seq);
    return seq;
}

uint32_t UsbCommandSender::send_key(int keycode) {
    // Payload: keycode(4) + flags(4) = 8 bytes
    uint8_t payload[8];

    payload[0] = keycode & 0xFF;
    payload[1] = (keycode >> 8) & 0xFF;
    payload[2] = (keycode >> 16) & 0xFF;
    payload[3] = (keycode >> 24) & 0xFF;
    payload[4] = 0;
    payload[5] = 0;
    payload[6] = 0;
    payload[7] = 0;

    auto packet = build_packet(CMD_KEY, payload, sizeof(payload));
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued KEY(%d) seq=%u", keycode, seq);
    return seq;
}

uint32_t UsbCommandSender::send_click_id(const std::string& resource_id) {
    // Payload: UTF-8文字列そのまま (Protocol.ktがペイロード全体をUTF-8として解釈する)
    auto packet = build_packet(CMD_CLICK_ID,
                               reinterpret_cast<const uint8_t*>(resource_id.data()),
                               resource_id.size());
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued CLICK_ID(%s) seq=%u", resource_id.c_str(), seq);
    return seq;
}

uint32_t UsbCommandSender::send_click_text(const std::string& text) {
    // Payload: UTF-8文字列そのまま (Protocol.ktがペイロード全体をUTF-8として解釈する)
    auto packet = build_packet(CMD_CLICK_TEXT,
                               reinterpret_cast<const uint8_t*>(text.data()),
                               text.size());
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    std::lock_guard<std::mutex> lock(queue_mtx_);
    command_queue_.push(std::move(packet));

    MLOG_INFO("usbcmd", "Queued CLICK_TEXT(%s) seq=%u", text.c_str(), seq);
    return seq;
}

#else // !USE_LIBUSB

bool UsbCommandSender::start() {
    MLOG_INFO("usbcmd", "USB support not compiled (USE_LIBUSB not defined)");
    return false;
}

void UsbCommandSender::stop() {
    running_.store(false);
}

uint32_t UsbCommandSender::send_ping() { return 0; }
uint32_t UsbCommandSender::send_tap(int, int, int, int) { return 0; }
uint32_t UsbCommandSender::send_swipe(int, int, int, int, int) { return 0; }
uint32_t UsbCommandSender::send_back() { return 0; }
uint32_t UsbCommandSender::send_key(int) { return 0; }
uint32_t UsbCommandSender::send_click_id(const std::string&) { return 0; }
uint32_t UsbCommandSender::send_click_text(const std::string&) { return 0; }

#endif // USE_LIBUSB

} // namespace gui
