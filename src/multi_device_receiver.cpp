#include "multi_device_receiver.hpp"
#include <cstdio>
#include "mirage_log.hpp"
#include "config_loader.hpp"  // ExpectedSizeRegistry

namespace gui {

MultiDeviceReceiver::MultiDeviceReceiver() = default;

MultiDeviceReceiver::~MultiDeviceReceiver() {
    stop();
}

bool MultiDeviceReceiver::start(uint16_t base_port) {
    if (running_) return true;
    if (!adb_manager_) {
        MLOG_INFO("multi", "No device manager set");
        return false;
    }

    auto devices = adb_manager_->getUniqueDevices();
    if (devices.empty()) {
        MLOG_INFO("multi", "No devices found");
        return false;
    }

    std::lock_guard<std::mutex> lock(receivers_mutex_);

    for (const auto& device : devices) {
        ReceiverEntry entry;
        entry.hardware_id = device.hardware_id;
        entry.display_name = device.display_name;
        entry.receiver = std::make_unique<MirrorReceiver>();
        // VulkanコンテキストをReceiverに伝播
        if (vk_device_ != VK_NULL_HANDLE) {
            entry.receiver->setVulkanContext(vk_physical_device_, vk_device_,
                vk_graphics_queue_family_, vk_graphics_queue_,
                vk_compute_queue_family_, vk_compute_queue_);
        }
        entry.last_stats_time = std::chrono::steady_clock::now();

        // Always use port=0 for OS-assigned port to avoid TIME_WAIT conflicts
        uint16_t request_port = 0;
        (void)base_port;  // base_port kept for API compatibility
        if (entry.receiver->start(request_port)) {
            // Get actual bound port from receiver
            entry.port = entry.receiver->getPort();
            MLOG_INFO("multi", "Started receiver for %s on port %d", device.display_name.c_str(), entry.port);

            port_to_device_[entry.port] = device.hardware_id;
            receivers_[device.hardware_id] = std::move(entry);
        } else {
            MLOG_ERROR("multi", "Failed to start receiver for %s", device.display_name.c_str());
        }
    }

    running_ = !receivers_.empty();
    MLOG_INFO("multi", "Started %zu receivers", receivers_.size());
    return running_;
}

bool MultiDeviceReceiver::restart_as_tcp(const std::string& hardware_id, uint16_t tcp_port) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) {
        MLOG_ERROR("multi", "restart_as_tcp: device %s not found", hardware_id.c_str());
        return false;
    }

    auto& entry = it->second;
    int old_port = entry.port;

    // Stop existing UDP receiver
    if (entry.receiver) {
        entry.receiver->stop();
    }

    // Create new receiver in TCP mode
    entry.receiver = std::make_unique<MirrorReceiver>();
    // VulkanコンテキストをReceiverに伝播
    if (vk_device_ != VK_NULL_HANDLE) {
        entry.receiver->setVulkanContext(vk_physical_device_, vk_device_,
            vk_graphics_queue_family_, vk_graphics_queue_,
            vk_compute_queue_family_, vk_compute_queue_);
    }
    if (entry.receiver->start_tcp(tcp_port)) {
        entry.port = tcp_port;
        port_to_device_.erase(old_port);
        port_to_device_[tcp_port] = hardware_id;
        MLOG_INFO("multi", "Restarted %s in TCP mode on port %d (was UDP %d)",
                  entry.display_name.c_str(), tcp_port, old_port);
        return true;
    } else {
        MLOG_ERROR("multi", "Failed to restart %s in TCP mode on port %d",
                   entry.display_name.c_str(), tcp_port);
        return false;
    }
}

bool MultiDeviceReceiver::restart_as_tcp_vid0(const std::string& hardware_id, uint16_t tcp_port) {
    // IDRコールバック：デバウンス付き（スレッド横断で共有するatomicタイマー）
    auto make_idr_callback = [this](const std::string& hw_id) -> std::function<void()> {
        auto last_idr_ms = std::make_shared<std::atomic<int64_t>>(0);
        auto* mgr = adb_manager_;
        return [hw_id, mgr, last_idr_ms]() {
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t prev = last_idr_ms->load();
            if (now_ms - prev < 1000) return;  // 1秒デバウンス
            if (!last_idr_ms->compare_exchange_strong(prev, now_ms)) return;  // CASで競合防止
            if (!mgr) return;
            AdbDeviceManager::UniqueDevice dev;
            if (mgr->getUniqueDevice(hw_id, dev) && !dev.preferred_adb_id.empty()) {
                MLOG_INFO("multi", "FU-A gap → IDR要求: %s", dev.preferred_adb_id.c_str());
                std::thread([adb_id = dev.preferred_adb_id, mgr]() {
                    mgr->adbCommand(adb_id, "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR");
                }).detach();
            }
        };
    };

    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) {
        // エントリが無い場合は新規作成 (start()をスキップした場合)
        ReceiverEntry entry;
        entry.hardware_id = hardware_id;
        entry.display_name = hardware_id;
        if (adb_manager_) {
            AdbDeviceManager::UniqueDevice dev_info;
            if (adb_manager_->getUniqueDevice(hardware_id, dev_info)) {
                entry.display_name = dev_info.display_name;
            }
        }
        entry.receiver = std::make_unique<MirrorReceiver>();
        // VulkanコンテキストをReceiverに伝播
        if (vk_device_ != VK_NULL_HANDLE) {
            entry.receiver->setVulkanContext(vk_physical_device_, vk_device_,
                vk_graphics_queue_family_, vk_graphics_queue_,
                vk_compute_queue_family_, vk_compute_queue_);
        }
        entry.last_stats_time = std::chrono::steady_clock::now();

        // FU-Aギャップ検出時にIDR要求（デバウンス: 1秒に1回まで）
        if (adb_manager_) {
            entry.receiver->setIdrCallback(make_idr_callback(hardware_id));
        }

        if (entry.receiver->start_tcp_vid0(tcp_port)) {
            entry.port = tcp_port;
            port_to_device_[tcp_port] = hardware_id;
            MLOG_INFO("multi", "Started %s in VID0 TCP mode on port %d (new entry)",
                      entry.display_name.c_str(), tcp_port);
            receivers_[hardware_id] = std::move(entry);
            running_ = true;
            return true;
        } else {
            MLOG_ERROR("multi", "Failed to start %s in VID0 TCP mode on port %d",
                       entry.display_name.c_str(), tcp_port);
            return false;
        }
    }

    auto& entry = it->second;
    int old_port = entry.port;

    if (entry.receiver) {
        entry.receiver->stop();
    }

    entry.receiver = std::make_unique<MirrorReceiver>();
    // VulkanコンテキストをReceiverに伝播
    if (vk_device_ != VK_NULL_HANDLE) {
        entry.receiver->setVulkanContext(vk_physical_device_, vk_device_,
            vk_graphics_queue_family_, vk_graphics_queue_,
            vk_compute_queue_family_, vk_compute_queue_);
    }

    // FU-Aギャップ検出時にIDR要求（デバウンス: 1秒に1回まで）
    if (adb_manager_) {
        entry.receiver->setIdrCallback(make_idr_callback(hardware_id));
    }

    if (entry.receiver->start_tcp_vid0(tcp_port)) {
        entry.port = tcp_port;
        port_to_device_.erase(old_port);
        port_to_device_[tcp_port] = hardware_id;
        MLOG_INFO("multi", "Restarted %s in VID0 TCP mode on port %d (was %d)",
                  entry.display_name.c_str(), tcp_port, old_port);
        return true;
    } else {
        MLOG_ERROR("multi", "Failed to restart %s in VID0 TCP mode on port %d",
                   entry.display_name.c_str(), tcp_port);
        return false;
    }
}

void MultiDeviceReceiver::setVulkanContext(VkPhysicalDevice physical_device, VkDevice device,
                                            uint32_t graphics_queue_family, uint32_t compute_queue_family,
                                            VkQueue graphics_queue, VkQueue compute_queue) {
    vk_physical_device_ = physical_device;
    vk_device_ = device;
    vk_graphics_queue_family_ = graphics_queue_family;
    vk_compute_queue_family_ = compute_queue_family;
    vk_graphics_queue_ = graphics_queue;
    vk_compute_queue_ = compute_queue;
    MLOG_INFO("multi", "Vulkanコンテキスト設定完了");
}

void MultiDeviceReceiver::setTiledCallback(TiledCallback cb) {
    tiled_callback_ = std::move(cb);
}

void MultiDeviceReceiver::setFrameCallback(FrameCallback cb) {
    frame_callback_ = std::move(cb);

    // receiverが存在していてコールバック設定済みならポーリングスレッドを起動
    bool has_receivers;
    {
        std::lock_guard<std::mutex> lock(receivers_mutex_);
        has_receivers = !receivers_.empty();
    }
    if (frame_callback_ && has_receivers && !frame_poll_running_.load()) {
        running_ = true;  // ensure running flag is set
        frame_poll_running_.store(true);
        frame_poll_thread_ = std::thread(&MultiDeviceReceiver::framePollThreadFunc, this);
        MLOG_INFO("multi", "Frame poll thread started by setFrameCallback");
    }
}

void MultiDeviceReceiver::framePollThreadFunc() {
    MLOG_INFO("multi", "フレームポーリングスレッド開始");

    while (frame_poll_running_.load() && running_) {
        // 各デバイスのフレームをポーリング
        std::vector<std::string> device_ids;
        {
            std::lock_guard<std::mutex> lock(receivers_mutex_);
            device_ids.reserve(receivers_.size());
            for (const auto& [hw_id, entry] : receivers_) {
                device_ids.push_back(hw_id);
            }
        }

        for (const auto& hw_id : device_ids) {
            if (!frame_poll_running_.load()) break;
            std::shared_ptr<mirage::SharedFrame> sf;
            get_latest_shared_frame(hw_id, sf);  // callback fired inside if sf valid
        }

        // ~60FPS相当のポーリング間隔
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    MLOG_INFO("multi", "フレームポーリングスレッド終了");
}

void MultiDeviceReceiver::stop() {
    if (!running_) return;

    // フレームポーリングスレッド停止
    frame_poll_running_.store(false);
    if (frame_poll_thread_.joinable()) {
        frame_poll_thread_.join();
    }

    std::lock_guard<std::mutex> lock(receivers_mutex_);

    for (auto& [hw_id, entry] : receivers_) {
        if (entry.receiver) {
            entry.receiver->stop();
        }
    }

    receivers_.clear();
    port_to_device_.clear();
    running_ = false;

    MLOG_INFO("multi", "Stopped all receivers");
}

bool MultiDeviceReceiver::get_latest_frame(const std::string& hardware_id, MirrorFrame& out) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) return false;

    auto& entry = it->second;
    if (!entry.receiver) return false;

    if (entry.receiver->get_latest_frame(out)) {
        entry.frames++;
        entry.last_frame_time = std::chrono::steady_clock::now();

        // Update stats periodically
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_stats_time).count();

        if (elapsed >= 1000) {
            float elapsed_sec = elapsed / 1000.0f;
            entry.fps = (entry.frames - entry.prev_frames) / elapsed_sec;

            uint64_t current_bytes = entry.receiver->bytes_received();
            uint64_t new_bytes = current_bytes - entry.prev_bytes;
            entry.bandwidth_mbps = (new_bytes * 8.0f / 1000000.0f) / elapsed_sec;
            entry.bytes = current_bytes;
            entry.packets = entry.receiver->packets_received();

            entry.prev_frames = entry.frames;
            entry.prev_bytes = current_bytes;
            entry.last_stats_time = now;
        }

        // Call frame callback if set (legacy MirrorFrame path)
        // Use get_latest_shared_frame for SharedFrame callback


        return true;
    }

    return false;
}

bool MultiDeviceReceiver::get_latest_shared_frame(const std::string& hardware_id,
                                                    std::shared_ptr<mirage::SharedFrame>& out) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) return false;
    auto& entry = it->second;
    if (!entry.receiver) return false;
    if (!entry.receiver->get_latest_shared_frame(out)) return false;
    entry.frames++;
    entry.last_frame_time = std::chrono::steady_clock::now();
    if (frame_callback_) {
        frame_callback_(hardware_id, out);  // SharedFrame direct
    }
    return true;
}

bool MultiDeviceReceiver::get_latest_frame_by_port(int port, MirrorFrame& out) {
    // Get receiver pointer with minimal lock time
    MirrorReceiver* receiver_ptr = nullptr;
    {
        std::lock_guard<std::mutex> lock(receivers_mutex_);

        auto it = port_to_device_.find(port);
        if (it == port_to_device_.end()) return false;

        const std::string& hw_id = it->second;
        auto entry_it = receivers_.find(hw_id);
        if (entry_it == receivers_.end()) return false;

        auto& entry = entry_it->second;
        receiver_ptr = entry.receiver.get();
    }
    // Lock released - receiver access is safe because we own unique_ptr

    if (!receiver_ptr) return false;
    return receiver_ptr->get_latest_frame(out);
}

std::vector<MultiDeviceReceiver::DeviceStats> MultiDeviceReceiver::getStats() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    std::vector<DeviceStats> stats;
    stats.reserve(receivers_.size());

    auto now = std::chrono::steady_clock::now();

    for (const auto& [hw_id, entry] : receivers_) {
        DeviceStats ds;
        ds.hardware_id = entry.hardware_id;
        ds.display_name = entry.display_name;
        ds.port = entry.port;
        ds.packets = entry.packets;
        // Use live bytes_received() so BandwidthMonitor sees TCP VID0 traffic
        // even before the first decoded frame (entry.bytes lags by one stats cycle).
        ds.bytes = entry.receiver ? entry.receiver->bytes_received() : entry.bytes;
        ds.fps = entry.fps;
        ds.bandwidth_mbps = entry.bandwidth_mbps;

        // Check if receiving (had frame in last 2 seconds)
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_frame_time).count();
        ds.receiving = (since_last < 2000);
        ds.last_frame_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            entry.last_frame_time.time_since_epoch()).count();

        stats.push_back(ds);
    }

    return stats;
}

int MultiDeviceReceiver::getActiveDeviceCount() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    auto now = std::chrono::steady_clock::now();
    int count = 0;

    for (const auto& [hw_id, entry] : receivers_) {
        auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - entry.last_frame_time).count();
        if (since_last < 2000) {
            count++;
        }
    }

    return count;
}

std::vector<std::string> MultiDeviceReceiver::getDeviceIds() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    std::vector<std::string> ids;
    ids.reserve(receivers_.size());

    for (const auto& [hw_id, entry] : receivers_) {
        ids.push_back(hw_id);
    }

    return ids;
}

int MultiDeviceReceiver::getPortForDevice(const std::string& hardware_id) const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it != receivers_.end()) return it->second.port;
    return 0;
}

void MultiDeviceReceiver::feed_rtp_packet(const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    // Feed to first device's receiver (USB video from primary device)
    if (!receivers_.empty()) {
        auto& entry = receivers_.begin()->second;
        if (entry.receiver) {
            entry.receiver->feed_rtp_packet(data, len);
        }
    }
}

std::string MultiDeviceReceiver::getFirstDeviceId() const {
    std::lock_guard<std::mutex> lock(receivers_mutex_);

    if (!receivers_.empty()) {
        return receivers_.begin()->first;
    }
    return "";
}

bool MultiDeviceReceiver::restart_as_tcp_vid0_tiled(const std::string& hardware_id,
                                                     uint16_t port0, uint16_t port1,
                                                     const std::string& host) {
    MLOG_INFO("multi", "restart_as_tcp_vid0_tiled: %s port0=%d port1=%d host=%s",
              hardware_id.c_str(), port0, port1, host.c_str());

    std::lock_guard<std::mutex> lock(receivers_mutex_);
    auto it = receivers_.find(hardware_id);
    if (it == receivers_.end()) {
        // 新規エントリ作成
        ReceiverEntry entry;
        entry.hardware_id = hardware_id;
        entry.display_name = hardware_id;
        if (adb_manager_) {
            AdbDeviceManager::UniqueDevice dev_info;
            if (adb_manager_->getUniqueDevice(hardware_id, dev_info))
                entry.display_name = dev_info.display_name;
        }
        entry.last_stats_time = std::chrono::steady_clock::now();
        // TileCompositor を起動（receiver は compositor 内部で管理）
        auto tc = std::make_unique<TileCompositor>();
        if (vk_device_ != VK_NULL_HANDLE) {
            tc->setVulkanContext(vk_physical_device_, vk_device_,
                vk_graphics_queue_family_, vk_graphics_queue_,
                vk_compute_queue_family_, vk_compute_queue_);
        }
        // 合成済みフレームをフレームコールバックへ転送
        auto cb = frame_callback_;
        auto hw_id_copy = hardware_id;
        // Zero-copy tiled callback (preferred path)
        if (tiled_callback_) {
            auto tiled_cb_copy = tiled_callback_;
            auto hw_tiled = hardware_id;
            tc->setTiledCallback([this, hw_tiled, tiled_cb_copy](
                const std::shared_ptr<mirage::SharedFrame>& top,
                const std::shared_ptr<mirage::SharedFrame>& bot,
                int slice_h) {
                // FPS accounting
                {
                    std::lock_guard<std::mutex> lock(receivers_mutex_);
                    auto it = receivers_.find(hw_tiled);
                    if (it != receivers_.end()) {
                        auto& e = it->second;
                        e.frames++;
                        e.last_frame_time = std::chrono::steady_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                            e.last_frame_time - e.last_stats_time).count();
                        if (elapsed >= 1000) {
                            float elapsed_sec = elapsed / 1000.0f;
                            e.fps = (e.frames - e.prev_frames) / elapsed_sec;
                            e.prev_frames = e.frames;
                            e.last_stats_time = e.last_frame_time;
                        }
                    }
                }
                tiled_cb_copy(hw_tiled, top, bot, slice_h);
            });
        }

        tc->setFrameCallback([this, hw_id_copy](std::shared_ptr<mirage::SharedFrame> sf) {
            // Update frame count and FPS for tiled mode
            {
                std::lock_guard<std::mutex> lock(receivers_mutex_);
                auto it = receivers_.find(hw_id_copy);
                if (it != receivers_.end()) {
                    auto& e = it->second;
                    e.frames++;
                    e.last_frame_time = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        e.last_frame_time - e.last_stats_time).count();
                    if (elapsed >= 1000) {
                        float elapsed_sec = elapsed / 1000.0f;
                        e.fps = (e.frames - e.prev_frames) / elapsed_sec;
                        e.prev_frames = e.frames;
                        e.last_stats_time = e.last_frame_time;
                    }
                }
            }
            if (frame_callback_) frame_callback_(hw_id_copy, sf);
        });
        // デバイスのネイティブ解像度を設定 → compose()がnative_hで出力
        {
            int native_w = 0, native_h = 0;
            if (mirage::config::ExpectedSizeRegistry::instance()
                    .getExpectedSize(hardware_id, native_w, native_h) && native_h > 0) {
                tc->set_native_size(native_w, native_h, 2);  // tilesY=2 (1x2 split)
                MLOG_INFO("multi", "TileCompositor native size: %dx%d", native_w, native_h);
            }
        }
        if (!tc->start(port0, port1, host)) {
            MLOG_ERROR("multi", "TileCompositor start failed for %s", hardware_id.c_str());
            return false;
        }
        entry.tile_compositor = std::move(tc);
        entry.port = port0;
        port_to_device_[port0] = hardware_id;
        receivers_[hardware_id] = std::move(entry);
        running_ = true;
        MLOG_INFO("multi", "TiledMode started for %s host=%s (new entry)", hardware_id.c_str(), host.c_str());
        return true;
    }

    auto& entry = it->second;
    // 既存 receiver/compositor を停止
    if (entry.receiver)        { entry.receiver->stop(); entry.receiver.reset(); }
    if (entry.tile_compositor) { entry.tile_compositor->stop(); entry.tile_compositor.reset(); }
    port_to_device_.erase(entry.port);

    auto tc = std::make_unique<TileCompositor>();
    if (vk_device_ != VK_NULL_HANDLE) {
        tc->setVulkanContext(vk_physical_device_, vk_device_,
            vk_graphics_queue_family_, vk_graphics_queue_,
            vk_compute_queue_family_, vk_compute_queue_);
    }
    auto hw_id_copy = hardware_id;
    tc->setFrameCallback([this, hw_id_copy](std::shared_ptr<mirage::SharedFrame> sf) {
        // Update frame count and FPS for tiled mode
        {
            std::lock_guard<std::mutex> lock(receivers_mutex_);
            auto it = receivers_.find(hw_id_copy);
            if (it != receivers_.end()) {
                auto& e = it->second;
                e.frames++;
                e.last_frame_time = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    e.last_frame_time - e.last_stats_time).count();
                if (elapsed >= 1000) {
                    float elapsed_sec = elapsed / 1000.0f;
                    e.fps = (e.frames - e.prev_frames) / elapsed_sec;
                    e.prev_frames = e.frames;
                    e.last_stats_time = e.last_frame_time;
                }
            }
        }
        if (frame_callback_) frame_callback_(hw_id_copy, sf);
    });
    // 既存エントリ再起動パス: tiled_callback_ も登録する
    if (tiled_callback_) {
        auto tiled_cb_copy2 = tiled_callback_;
        auto hw_tiled2 = hardware_id;
        tc->setTiledCallback([this, hw_tiled2, tiled_cb_copy2](
            const std::shared_ptr<mirage::SharedFrame>& top,
            const std::shared_ptr<mirage::SharedFrame>& bot,
            int slice_h) {
            tiled_cb_copy2(hw_tiled2, top, bot, slice_h);
        });
    }
    // デバイスのネイティブ解像度を設定 → compose()がnative_hで出力（既存エントリ再起動パス）
    {
        int native_w = 0, native_h = 0;
        if (mirage::config::ExpectedSizeRegistry::instance()
                .getExpectedSize(hardware_id, native_w, native_h) && native_h > 0) {
            tc->set_native_size(native_w, native_h, 2);  // tilesY=2 (1x2 split)
            MLOG_INFO("multi", "TileCompositor native size (restart): %dx%d", native_w, native_h);
        }
    }
    if (!tc->start(port0, port1, host)) {
        MLOG_ERROR("multi", "TileCompositor start failed for %s", hardware_id.c_str());
        return false;
    }
    entry.tile_compositor = std::move(tc);
    entry.port = port0;
    port_to_device_[port0] = hardware_id;
    MLOG_INFO("multi", "TiledMode restarted for %s (port0=%d,port1=%d,host=%s)", hardware_id.c_str(), port0, port1, host.c_str());
    return true;
}

} // namespace gui
