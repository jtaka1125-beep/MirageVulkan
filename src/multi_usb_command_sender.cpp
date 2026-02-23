#include "multi_usb_command_sender.hpp"
#include <system_error>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <stdexcept>
#include "mirage_log.hpp"
#include "mirage_protocol.hpp"
#include "winusb_checker.hpp"

using namespace mirage::protocol;

namespace gui {

// =============================================================================
// Error Handling Utilities
// =============================================================================

// Thread-safe error state per device
struct DeviceErrorState {
    std::atomic<int> consecutive_errors{0};
    std::atomic<int> total_errors{0};
    std::atomic<bool> in_recovery{false};
    static constexpr int MAX_CONSECUTIVE_ERRORS = 5;
    static constexpr int RECOVERY_DELAY_MS = 100;

    bool should_recover() const {
        return consecutive_errors.load() >= MAX_CONSECUTIVE_ERRORS && !in_recovery.load();
    }

    void record_error() {
        consecutive_errors.fetch_add(1);
        total_errors.fetch_add(1);
    }

    void record_success() {
        consecutive_errors.store(0);
    }

    void reset() {
        consecutive_errors.store(0);
        in_recovery.store(false);
    }
};

// Global error tracking (for libusb context errors)
static std::atomic<int> g_libusb_init_failures{0};
static constexpr int MAX_LIBUSB_RETRIES = 3;

MultiUsbCommandSender::MultiUsbCommandSender() = default;

MultiUsbCommandSender::~MultiUsbCommandSender() {
    stop();
}

#ifdef USE_LIBUSB

bool MultiUsbCommandSender::start() {
    if (running_.load()) return true;

    // Initialize libusb with retry logic
    int init_ret = LIBUSB_ERROR_OTHER;
    for (int retry = 0; retry < MAX_LIBUSB_RETRIES; retry++) {
        init_ret = libusb_init(&ctx_);
        if (init_ret == LIBUSB_SUCCESS) {
            g_libusb_init_failures.store(0);
            break;
        }
        MLOG_ERROR("multicmd", "libusb init failed (attempt %d/%d): %s", retry + 1, MAX_LIBUSB_RETRIES, libusb_error_name(init_ret));
        g_libusb_init_failures.fetch_add(1);

        if (retry < MAX_LIBUSB_RETRIES - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    if (init_ret != LIBUSB_SUCCESS) {
        MLOG_ERROR("multicmd", "FATAL: Failed to init libusb after %d attempts", MAX_LIBUSB_RETRIES);
        return false;
    }

    // Set libusb debug level for better diagnostics
#if LIBUSB_API_VERSION >= 0x01000106
    libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#endif

    if (!find_and_open_all_devices()) {
        MLOG_WARN("multicmd", "No AOA devices found (will retry on rescan)");
        // Diagnose: check if WinUSB driver is the issue
        if (mirage::WinUsbChecker::anyDeviceNeedsWinUsb()) {
            auto summary = mirage::WinUsbChecker::getDiagnosticSummary();
            MLOG_ERROR("multicmd", "WinUSB DRIVER ISSUE DETECTED: %s", summary.c_str());
            MLOG_ERROR("multicmd", "Run install_android_winusb.py or use GUI [Driver Setup] button to fix");
        }
        // Don't fail - devices may connect later
    }

    running_.store(true);

    // Start send thread with exception safety
    try {
        send_thread_ = std::thread(&MultiUsbCommandSender::send_thread, this);
    } catch (const std::system_error& e) {
        MLOG_ERROR("multicmd", "FATAL: Failed to start send thread: %s", e.what());
        running_.store(false);
        if (ctx_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
        return false;
    }

    // Start per-device receive threads with exception handling
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (auto& [id, dev] : devices_) {
            if (dev->ep_in != 0 && !dev->recv_running.load()) {
                try {
                    dev->recv_running.store(true);
                    dev->recv_thread = std::thread(&MultiUsbCommandSender::device_receive_thread, this, id);
                    MLOG_INFO("multicmd", "Started receive thread for %s", id.c_str());
                } catch (const std::system_error& e) {
                    MLOG_ERROR("multicmd", "ERROR: Failed to start recv thread for %s: %s", id.c_str(), e.what());
                    dev->recv_running.store(false);
                    // Continue with other devices
                }
            }
        }
    }

    MLOG_INFO("multicmd", "Started with %d device(s)", device_count());
    return true;
}

void MultiUsbCommandSender::stop() {
    // Early exit if already stopped
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        MLOG_INFO("multicmd", "Already stopped or stopping");
        return;
    }

    MLOG_INFO("multicmd", "Initiating graceful shutdown...");

    // Join send thread with timeout protection
    if (send_thread_.joinable()) {
        auto join_start = std::chrono::steady_clock::now();
        constexpr auto MAX_JOIN_WAIT = std::chrono::seconds(5);

        // Use atomic flag to track join completion
        std::atomic<bool> join_completed{false};
        std::thread joiner([this, &join_completed]() {
            if (send_thread_.joinable()) {
                send_thread_.join();
            }
            join_completed.store(true);
        });

        // Wait for joiner with timeout using polling
        while (!join_completed.load()) {
            auto elapsed = std::chrono::steady_clock::now() - join_start;
            if (elapsed > MAX_JOIN_WAIT) {
                MLOG_WARN("multicmd", "WARNING: Send thread join timeout after %.1fs, detaching", std::chrono::duration<double>(elapsed).count());
                // Detach joiner thread - it will clean up when send_thread_ finally exits
                if (joiner.joinable()) {
                    joiner.detach();
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        // If joiner completed normally, join it
        if (join_completed.load() && joiner.joinable()) {
            joiner.join();
        }

        auto elapsed = std::chrono::steady_clock::now() - join_start;
        if (elapsed > std::chrono::seconds(1)) {
            MLOG_INFO("multicmd", "Send thread join took %.1fs", std::chrono::duration<double>(elapsed).count());
        }
    }

    // Collect threads to join and stop them
    std::vector<std::thread> threads_to_join;
    std::vector<std::pair<libusb_device_handle*, std::string>> handles_to_close;

    // Signal all receive threads to stop
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (auto& [id, dev] : devices_) {
            dev->recv_running.store(false);
        }
    }

    // Wait for threads to notice the stop signal (with bounded wait)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Collect threads and handles to close (while holding lock)
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        for (auto& [id, dev] : devices_) {
            if (dev->recv_thread.joinable()) {
                threads_to_join.push_back(std::move(dev->recv_thread));
            }
            if (dev->handle) {
                handles_to_close.push_back({dev->handle, id});
                dev->handle = nullptr;  // Prevent double-close
            }
        }
    }

    // Join threads OUTSIDE the lock to avoid deadlock
    // Track any threads that fail to join
    int failed_joins = 0;
    for (auto& t : threads_to_join) {
        if (t.joinable()) {
            try {
                t.join();
            } catch (const std::system_error& e) {
                MLOG_ERROR("multicmd", "ERROR: Thread join failed: %s", e.what());
                failed_joins++;
            }
        }
    }

    if (failed_joins > 0) {
        MLOG_ERROR("multicmd", "WARNING: %d thread(s) failed to join cleanly", failed_joins);
    }

    // Close USB handles with error handling
    for (auto& [handle, id] : handles_to_close) {
        MLOG_INFO("multicmd", "Closing device %s", id.c_str());
        int ret = libusb_release_interface(handle, 0);
        if (ret != LIBUSB_SUCCESS && ret != LIBUSB_ERROR_NOT_FOUND && ret != LIBUSB_ERROR_NO_DEVICE) {
            MLOG_ERROR("multicmd", "WARNING: release_interface failed for %s: %s", id.c_str(), libusb_error_name(ret));
        }
        libusb_close(handle);
    }

    // Clear devices
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        devices_.clear();
    }

    if (ctx_) {
        libusb_exit(ctx_);
        ctx_ = nullptr;
    }

    MLOG_INFO("multicmd", "Stopped successfully");
}

void MultiUsbCommandSender::rescan() {
    if (!running_.load()) return;

    MLOG_INFO("multicmd", "Rescanning for devices...");
    find_and_open_all_devices();
    MLOG_INFO("multicmd", "Found %d device(s)", device_count());
}

int MultiUsbCommandSender::device_count() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    int count = 0;
    for (const auto& [id, dev] : devices_) {
        if (dev->info.connected) count++;
    }
    return count;
}

std::vector<std::string> MultiUsbCommandSender::get_device_ids() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    std::vector<std::string> ids;
    for (const auto& [id, dev] : devices_) {
        if (dev->info.connected) {
            ids.push_back(id);
        }
    }
    return ids;
}

bool MultiUsbCommandSender::get_device_info(const std::string& usb_id, DeviceInfo& out) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    auto it = devices_.find(usb_id);
    if (it == devices_.end()) return false;
    out = it->second->info;
    return true;
}

bool MultiUsbCommandSender::is_device_connected(const std::string& usb_id) const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    auto it = devices_.find(usb_id);
    return it != devices_.end() && it->second->info.connected;
}

std::string MultiUsbCommandSender::get_first_device_id() const {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    for (const auto& [id, dev] : devices_) {
        if (dev->info.connected) return id;
    }
    return "";
}

void MultiUsbCommandSender::send_thread() {
    MLOG_INFO("multicmd", "Send thread started");

    // ISSUE-7+9+10: CV-driven loop; out-of-lock USB transfer; ACK always checked
    constexpr int RESCAN_INTERVAL_MS = 30000;
    auto last_rescan = std::chrono::steady_clock::now();

    while (running_.load()) {
        // ISSUE-7: wake immediately on new command, otherwise poll every 30ms
        {
            std::unique_lock<std::mutex> cv_lk(send_cv_mtx_);
            send_cv_.wait_for(cv_lk, std::chrono::milliseconds(30));
        }

        // ISSUE-10: ACK timeout check runs unconditionally (not just when idle)
        process_pending_acks_();

        // ISSUE-9: collect work under brief lock, then do USB transfer outside lock
        struct SendWork {
            DeviceHandle*        dev;
            std::string          id;
            std::vector<uint8_t> packet;
        };
        std::vector<SendWork> work;
        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            for (auto& [id, dev] : devices_) {
                if (!dev->info.connected) continue;
                std::lock_guard<std::mutex> qlock(dev->queue_mutex);
                if (!dev->command_queue.empty()) {
                    work.push_back({dev.get(), id,
                                    std::move(dev->command_queue.front())});
                    dev->command_queue.pop();
                }
            }
        }

        // ISSUE-9: USB bulk transfer here — no mutex held
        for (auto& w : work) {
            if (send_raw(*w.dev, w.packet.data(), w.packet.size())) {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                auto it = devices_.find(w.id);
                if (it != devices_.end()) {
                    it->second->info.commands_sent++;
                    mirage::protocol::PacketHeader hdr;
                    if (mirage::protocol::parse_header(w.packet.data(), w.packet.size(), hdr) &&
                        hdr.cmd != mirage::protocol::CMD_PING) {
                        DeviceHandle::PendingAck pa;
                        pa.seq        = hdr.seq;
                        pa.cmd_byte   = hdr.cmd;
                        pa.packet     = w.packet;
                        pa.sent_at    = std::chrono::steady_clock::now();
                        pa.retry_count = 0;
                        std::lock_guard<std::mutex> qlock(it->second->queue_mutex);
                        it->second->pending_acks[hdr.seq] = std::move(pa);
                    }
                }
            }
        }

        // Auto-rescan when no devices (every 30s)
        auto now = std::chrono::steady_clock::now();
        if (device_count() == 0) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_rescan).count();
            if (ms >= RESCAN_INTERVAL_MS) {
                last_rescan = now;
                MLOG_INFO("multicmd", "No devices - auto rescan");
                find_and_open_all_devices();
                int cnt = device_count();
                if (cnt > 0) MLOG_INFO("multicmd", "Auto rescan found %d device(s)", cnt);
            }
        } else {
            last_rescan = now;
        }
    }

    MLOG_INFO("multicmd", "Send thread ended");
}

// ISSUE-13: receive_thread() removed (dead code)


void MultiUsbCommandSender::device_receive_thread(const std::string& device_id) {
    MLOG_INFO("multicmd", "Per-device receive thread started for %s", device_id.c_str());

    const int BUFFER_SIZE = 16384;
    uint8_t buf[BUFFER_SIZE];
    uint64_t recv_count = 0;

    // Per-device error tracking
    DeviceErrorState error_state;
    int consecutive_timeouts = 0;
    constexpr int MAX_CONSECUTIVE_TIMEOUTS = 1000;  // ~10 seconds at 10ms timeout

    while (running_.load()) {
        // Copy necessary fields while holding lock to avoid use-after-free
        libusb_device_handle* handle = nullptr;
        uint8_t ep_in = 0;
        bool connected = false;
        bool should_continue = false;
        bool recv_running = false;

        {
            std::lock_guard<std::mutex> lock(devices_mutex_);
            auto it = devices_.find(device_id);
            if (it == devices_.end()) {
                MLOG_INFO("multicmd", "[%s] Device removed from map, exiting", device_id.c_str());
                break;
            }
            recv_running = it->second->recv_running.load();
            if (!recv_running) {
                MLOG_INFO("multicmd", "[%s] recv_running=false, exiting", device_id.c_str());
                break;
            }
            DeviceHandle* dev = it->second.get();
            if (dev) {
                handle = dev->handle;
                ep_in = dev->ep_in;
                connected = dev->info.connected;
                should_continue = true;
            }
        }

        // Error recovery check
        if (error_state.should_recover()) {
            MLOG_ERROR("multicmd", "[%s] Too many errors, entering recovery mode", device_id.c_str());
            error_state.in_recovery.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(DeviceErrorState::RECOVERY_DELAY_MS));
            error_state.reset();
            continue;
        }

        if (!should_continue || !handle || !connected || ep_in == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // USB transfer uses local copies - safe even if device is removed
        int transferred = 0;
        int ret = libusb_bulk_transfer(handle, ep_in, buf, BUFFER_SIZE, &transferred, 10);

        if (ret == LIBUSB_SUCCESS && transferred > 0) {
            // Success - reset error counters
            total_bytes_received_ += transferred;
            error_state.record_success();
            consecutive_timeouts = 0;
            recv_count++;

            if (recv_count % 500 == 1) {
                MLOG_INFO("multicmd", "[%s] Received %d bytes (total: %llu, errors: %d)", device_id.c_str(), transferred, (unsigned long long)recv_count,
                        error_state.total_errors.load());
            }

            // Check if it's an ACK packet
            if (transferred >= (int)HEADER_SIZE) {
                uint32_t magic = buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
                uint8_t version = buf[4];
                uint8_t cmd = buf[5];
                uint32_t seq = buf[6] | (buf[7] << 8) | (buf[8] << 16) | (buf[9] << 24);

                if (magic == PROTOCOL_MAGIC && version == PROTOCOL_VERSION && cmd == CMD_ACK) {
                    uint8_t status = (transferred >= (int)HEADER_SIZE + 5) ? buf[HEADER_SIZE + 4] : 0;

                    // Update acks_received with lock
                    {
                        std::lock_guard<std::mutex> lock(devices_mutex_);
                        auto it = devices_.find(device_id);
                        if (it != devices_.end()) {
                            it->second->info.acks_received++;
                        }
                    }

                    // FIX-D: remove from pending_acks; re-queue if BUSY
                    {
                        std::lock_guard<std::mutex> lock(devices_mutex_);
                        auto dev_it = devices_.find(device_id);
                        if (dev_it != devices_.end()) {
                            auto& dev = dev_it->second;
                            std::lock_guard<std::mutex> qlock(dev->queue_mutex);
                            auto pa_it = dev->pending_acks.find(seq);
                            if (pa_it != dev->pending_acks.end()) {
                                if (status == STATUS_ERR_BUSY && pa_it->second.retry_count < 3) {
                                    // BUSY: re-queue the same packet after brief delay
                                    pa_it->second.retry_count++;
                                    pa_it->second.sent_at = std::chrono::steady_clock::now();
                                    dev->command_queue.push(pa_it->second.packet);
                                    MLOG_INFO("multicmd", "[%s] STATUS_BUSY seq=%u, retry %d/3",
                                        device_id.c_str(), seq, pa_it->second.retry_count);
                                } else {
                                    dev->pending_acks.erase(pa_it);
                                }
                            }
                        }
                    }

                    // Call callback outside lock to avoid potential deadlock
                    if (ack_callback_) {
                        try {
                            ack_callback_(device_id, seq, status);
                        } catch (const std::exception& e) {
                            MLOG_INFO("multicmd", "[%s] ACK callback exception: %s", device_id.c_str(), e.what());
                        }
                    }
                    continue;
                }
            }

            // Not an ACK - treat as video data
            if (video_callback_) {
                try {
                    video_callback_(device_id, buf, transferred);
                } catch (const std::exception& e) {
                    MLOG_INFO("multicmd", "[%s] Video callback exception: %s", device_id.c_str(), e.what());
                    error_state.record_error();
            // ISSUE-15: lifetime counter for diagnostics
            { std::lock_guard<std::mutex> _lk(devices_mutex_);
              auto _it = devices_.find(device_id);
              if (_it != devices_.end()) _it->second->recv_errors_lifetime++; }
                }
            }
        } else if (ret == LIBUSB_ERROR_TIMEOUT) {
            // Timeout is normal when no data available
            consecutive_timeouts++;
            if (consecutive_timeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
                // Exponential backoff: 100ms -> 200ms -> 500ms -> 1000ms (cap)
                int backoff_level = consecutive_timeouts / MAX_CONSECUTIVE_TIMEOUTS;  // 1,2,3,...
                int sleep_ms = (backoff_level <= 1) ? 100 :
                               (backoff_level <= 3) ? 200 :
                               (backoff_level <= 10) ? 500 : 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                // Log at decreasing frequency: every 10s, 20s, 50s, 100s
                int log_interval = MAX_CONSECUTIVE_TIMEOUTS * (backoff_level <= 3 ? 1 : backoff_level <= 10 ? 5 : 10);
                if (consecutive_timeouts % log_interval == 0) {
                    MLOG_WARN("multicmd", "[%s] Extended timeout (%d, backoff=%dms)", device_id.c_str(), consecutive_timeouts, sleep_ms);
                }
            }
        } else if (ret == LIBUSB_ERROR_NO_DEVICE) {
            MLOG_INFO("multicmd", "[%s] Device physically disconnected", device_id.c_str());

            // Mark as disconnected with lock
            {
                std::lock_guard<std::mutex> lock(devices_mutex_);
                auto it = devices_.find(device_id);
                if (it != devices_.end()) {
                    it->second->info.connected = false;
                }
            }
            // Notify listeners (e.g. HID unregister)
            if (device_closed_callback_) {
                device_closed_callback_(device_id);
            }
            break;
        } else if (ret == LIBUSB_ERROR_IO || ret == LIBUSB_ERROR_PIPE) {
            error_state.record_error();
            MLOG_ERROR("multicmd", "[%s] USB I/O error: %s (consecutive: %d)", device_id.c_str(), libusb_error_name(ret), error_state.consecutive_errors.load());

            if (error_state.consecutive_errors.load() >= DeviceErrorState::MAX_CONSECUTIVE_ERRORS) {
                MLOG_ERROR("multicmd", "[%s] Too many I/O errors, marking disconnected", device_id.c_str());
                {
                    std::lock_guard<std::mutex> lock(devices_mutex_);
                    auto it = devices_.find(device_id);
                    if (it != devices_.end()) {
                        it->second->info.connected = false;
                    }
                }
                // Notify listeners (e.g. HID unregister)
                if (device_closed_callback_) {
                    device_closed_callback_(device_id);
                }
                break;
            }
        } else if (ret != LIBUSB_SUCCESS) {
            // Other errors - log but don't immediately disconnect
            error_state.record_error();
            // Use thread_local to avoid data race between device threads
            static thread_local int other_error_count = 0;
            if (++other_error_count <= 10 || other_error_count % 100 == 0) {
                MLOG_ERROR("multicmd", "[%s] USB error: %s", device_id.c_str(), libusb_error_name(ret));
            }
        }
    }

    MLOG_ERROR("multicmd", "Per-device receive thread ended for %s (recv_count=%llu, total_errors=%d)", device_id.c_str(), (unsigned long long)recv_count, error_state.total_errors.load());
}

bool MultiUsbCommandSender::send_raw(DeviceHandle& device, const uint8_t* data, size_t len) {
    // Validation
    if (!data || len == 0) {
        MLOG_ERROR("multicmd", "send_raw: invalid data (null=%d, len=%zu)", data == nullptr, len);
        return false;
    }

    if (!device.handle) {
        MLOG_INFO("multicmd", "send_raw: device handle is null for %s", device.info.usb_id.c_str());
        device.info.connected = false;
        return false;
    }

    if (device.ep_out == 0) {
        MLOG_INFO("multicmd", "send_raw: no OUT endpoint for %s", device.info.usb_id.c_str());
        return false;
    }

    // Prevent excessively large transfers
    constexpr size_t MAX_TRANSFER_SIZE = 64 * 1024;  // 64KB limit
    if (len > MAX_TRANSFER_SIZE) {
        MLOG_INFO("multicmd", "send_raw: transfer too large (%zu > %zu)", len, MAX_TRANSFER_SIZE);
        return false;
    }

    int transferred = 0;
    int ret = libusb_bulk_transfer(device.handle, device.ep_out,
                                   const_cast<uint8_t*>(data), (int)len, &transferred, 1000);

    if (ret != LIBUSB_SUCCESS) {
        // Categorize errors
        switch (ret) {
            case LIBUSB_ERROR_NO_DEVICE:
                MLOG_INFO("multicmd", "USB send: device %s physically removed", device.info.usb_id.c_str());
                device.info.connected = false;
                if (device_closed_callback_) {
                    device_closed_callback_(device.info.usb_id);
                }
                break;

            case LIBUSB_ERROR_IO:
                MLOG_ERROR("multicmd", "USB send: I/O error on %s (may recover)", device.info.usb_id.c_str());
                // Don't immediately mark disconnected - may be transient
                break;

            case LIBUSB_ERROR_PIPE:
                MLOG_ERROR("multicmd", "USB send: pipe error on %s (endpoint stall?)", device.info.usb_id.c_str());
                // Could try libusb_clear_halt here
                break;

            case LIBUSB_ERROR_TIMEOUT:
                MLOG_WARN("multicmd", "USB send: timeout on %s (device busy?)", device.info.usb_id.c_str());
                break;

            case LIBUSB_ERROR_OVERFLOW:
                MLOG_INFO("multicmd", "USB send: overflow on %s (data too large)", device.info.usb_id.c_str());
                break;

            default:
                MLOG_ERROR("multicmd", "USB send error on %s: %s (%d)", device.info.usb_id.c_str(), libusb_error_name(ret), ret);
                if (ret == LIBUSB_ERROR_NO_DEVICE || ret == LIBUSB_ERROR_IO) {
                    device.info.connected = false;
                }
        }
        return false;
    }

    // Verify complete transfer
    if (transferred != (int)len) {
        MLOG_INFO("multicmd", "USB send: partial transfer on %s (%d/%zu bytes)", device.info.usb_id.c_str(), transferred, len);
        return false;
    }

    return true;
}

std::vector<uint8_t> MultiUsbCommandSender::build_packet(DeviceHandle& device, uint8_t cmd,
                                                          const uint8_t* payload, size_t payload_len) {
    std::vector<uint8_t> packet(HEADER_SIZE + payload_len);
    uint32_t seq = device.next_seq.fetch_add(1);

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

// FIX-D: Check pending ACKs for timeout and re-queue
void MultiUsbCommandSender::process_pending_acks_() {
    static constexpr int ACK_TIMEOUT_MS   = 2000;
    static constexpr int MAX_ACK_RETRIES  = 2;

    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(devices_mutex_);
    for (auto& [id, dev] : devices_) {
        if (!dev->info.connected) continue;
        std::lock_guard<std::mutex> qlock(dev->queue_mutex);
        for (auto it = dev->pending_acks.begin(); it != dev->pending_acks.end(); ) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.sent_at).count();
            if (elapsed_ms < ACK_TIMEOUT_MS) { ++it; continue; }

            if (it->second.retry_count < MAX_ACK_RETRIES) {
                // Timeout but retries remain: re-queue
                it->second.retry_count++;
                it->second.sent_at = now;
                dev->command_queue.push(it->second.packet);
                MLOG_INFO("multicmd", "[%s] ACK timeout seq=%u cmd=0x%02X, retry %d/%d",
                    id.c_str(), it->second.seq, it->second.cmd_byte,
                    it->second.retry_count, MAX_ACK_RETRIES);
                ++it;
            } else {
                // Max retries exceeded
                MLOG_INFO("multicmd", "[%s] ACK timeout FINAL seq=%u cmd=0x%02X ? dropping",
                    id.c_str(), it->second.seq, it->second.cmd_byte);
                dev->info.errors++;
                if (error_callback_) {
                    // Call outside lock to avoid deadlock — snapshot and call later
                    // (simplified: call with lock held, callback must not re-enter)
                    error_callback_(id, -1, "ACK timeout: seq=" + std::to_string(it->second.seq));
                }
                it = dev->pending_acks.erase(it);
            }
        }
    }
}

uint32_t MultiUsbCommandSender::queue_command(const std::string& usb_id, uint8_t cmd,
                                               const uint8_t* payload, size_t payload_len) {
    std::lock_guard<std::mutex> lock(devices_mutex_);

    auto it = devices_.find(usb_id);
    if (it == devices_.end() || !it->second->info.connected) {
        return 0;
    }

    auto& dev = *it->second;
    auto packet = build_packet(dev, cmd, payload, payload_len);
    uint32_t seq = packet[6] | (packet[7] << 8) | (packet[8] << 16) | (packet[9] << 24);

    {
        std::lock_guard<std::mutex> qlock(dev.queue_mutex);
        dev.command_queue.push(std::move(packet));
    }
    send_cv_.notify_one();  // ISSUE-7: wake send_thread

    return seq;
}

// Command API methods (send_ping, send_tap, etc.) are in usb_command_api.cpp

#else // !USE_LIBUSB

bool MultiUsbCommandSender::start() {
    MLOG_INFO("multicmd", "USB support not compiled (USE_LIBUSB not defined)");
    return false;
}

void MultiUsbCommandSender::stop() { running_.store(false); }
void MultiUsbCommandSender::rescan() {}
int MultiUsbCommandSender::device_count() const { return 0; }
std::vector<std::string> MultiUsbCommandSender::get_device_ids() const { return {}; }
bool MultiUsbCommandSender::get_device_info(const std::string&, DeviceInfo&) const { return false; }
bool MultiUsbCommandSender::is_device_connected(const std::string&) const { return false; }
std::string MultiUsbCommandSender::get_first_device_id() const { return ""; }
uint32_t MultiUsbCommandSender::send_ping(const std::string&) { return 0; }
uint32_t MultiUsbCommandSender::send_tap(const std::string&, int, int, int, int) { return 0; }
uint32_t MultiUsbCommandSender::send_swipe(const std::string&, int, int, int, int, int) { return 0; }
uint32_t MultiUsbCommandSender::send_back(const std::string&) { return 0; }
uint32_t MultiUsbCommandSender::send_key(const std::string&, int) { return 0; }
uint32_t MultiUsbCommandSender::send_click_id(const std::string&, const std::string&) { return 0; }
uint32_t MultiUsbCommandSender::send_click_text(const std::string&, const std::string&) { return 0; }
int MultiUsbCommandSender::send_tap_all(int, int, int, int) { return 0; }
int MultiUsbCommandSender::send_swipe_all(int, int, int, int, int) { return 0; }
int MultiUsbCommandSender::send_back_all() { return 0; }
int MultiUsbCommandSender::send_key_all(int) { return 0; }

// =============================================================================
// Video Control Commands (stub when !USE_LIBUSB)
// =============================================================================
uint32_t MultiUsbCommandSender::send_video_fps(const std::string&, int) { return 0; }
uint32_t MultiUsbCommandSender::send_video_route(const std::string&, uint8_t, const std::string&, int) { return 0; }
uint32_t MultiUsbCommandSender::send_video_idr(const std::string&) { return 0; }

#endif // USE_LIBUSB


// Video control commands (send_video_fps, etc.) are in usb_command_api.cpp

} // namespace gui
