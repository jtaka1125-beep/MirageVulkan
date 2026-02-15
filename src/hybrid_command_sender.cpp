#include "hybrid_command_sender.hpp"
#include <cstdio>
#include "mirage_log.hpp"

namespace gui {

HybridCommandSender::HybridCommandSender() = default;

HybridCommandSender::~HybridCommandSender() {
    stop();
}

bool HybridCommandSender::start() {
    if (running_) return true;

    MLOG_INFO("hybridcmd", "Starting multi-device USB command sender");

    usb_sender_ = std::make_unique<MultiUsbCommandSender>();
    adb_fallback_ = std::make_unique<mirage::AdbTouchFallback>();
    MLOG_INFO("hybridcmd", "ADB fallback initialized, HID touch created per-device on AOA switch");

    if (ack_callback_) {
        usb_sender_->set_ack_callback(ack_callback_);
    }
    if (video_callback_) {
        usb_sender_->set_video_callback(video_callback_);
    }

    // Register HID touch during AOA mode switch (before AOA_START_ACCESSORY)
    usb_sender_->set_pre_start_callback([this](libusb_device_handle* handle, int aoa_version) -> bool {
        if (aoa_version < 2) return false;
        auto touch = std::make_unique<mirage::AoaHidTouch>();
        MLOG_INFO("hybridcmd", "Registering HID touch device during AOA switch (v%d)", aoa_version);
        if (!touch->register_device(handle)) return false;
        // Store with temp key; moved to real device ID in device_opened_callback
        hid_touches_["_pending"] = std::move(touch);
        return true;
    });

    // Unregister HID on device disconnect
    usb_sender_->set_device_closed_callback([this](const std::string& usb_id) {
        auto it = hid_touches_.find(usb_id);
        if (it != hid_touches_.end()) {
            MLOG_INFO("hybridcmd", "Device %s disconnected, unregistering HID touch", usb_id.c_str());
            // Note: unregister_device will likely fail (device gone), but clears internal state
            if (it->second && it->second->is_registered()) {
                it->second->unregister_device(it->second->get_handle());
            }
            hid_touches_.erase(it);
        }
    });

    // Move pending HID registration to the real device ID after re-enumeration
    usb_sender_->set_device_opened_callback([this](const std::string& usb_id, libusb_device_handle* handle) {
        auto it = hid_touches_.find("_pending");
        if (it != hid_touches_.end()) {
            it->second->set_handle(handle);
            hid_touches_[usb_id] = std::move(it->second);
            hid_touches_.erase(it);
            MLOG_INFO("hybridcmd", "HID touch registered for device %s", usb_id.c_str());
        } else if (hid_touches_.count(usb_id) && hid_touches_[usb_id]->is_registered()) {
            hid_touches_[usb_id]->set_handle(handle);
            MLOG_INFO("hybridcmd", "HID touch handle updated for device %s", usb_id.c_str());
        }
    });

    if (!usb_sender_->start()) {
        MLOG_ERROR("hybridcmd", "Failed to start USB sender (will retry on rescan)");
        // Don't fail - devices may connect later
    }

    running_ = true;
    MLOG_INFO("hybridcmd", "Started with %d device(s)", usb_sender_->device_count());
    return true;
}

void HybridCommandSender::stop() {
    running_ = false;

    hid_touches_.clear();
    adb_fallback_.reset();

    if (usb_sender_) {
        usb_sender_->stop();
        usb_sender_.reset();
    }

    MLOG_INFO("hybridcmd", "Stopped");
}

int HybridCommandSender::device_count() const {
    return usb_sender_ ? usb_sender_->device_count() : 0;
}

std::vector<std::string> HybridCommandSender::get_device_ids() const {
    return usb_sender_ ? usb_sender_->get_device_ids() : std::vector<std::string>();
}

bool HybridCommandSender::is_device_connected(const std::string& usb_id) const {
    return usb_sender_ && usb_sender_->is_device_connected(usb_id);
}

std::string HybridCommandSender::get_first_device_id() const {
    return usb_sender_ ? usb_sender_->get_first_device_id() : "";
}

bool HybridCommandSender::get_device_info(const std::string& usb_id, MultiUsbCommandSender::DeviceInfo& out) const {
    return usb_sender_ && usb_sender_->get_device_info(usb_id, out);
}

void HybridCommandSender::set_ack_callback(AckCallback cb) {
    ack_callback_ = cb;
    if (usb_sender_) {
        usb_sender_->set_ack_callback(cb);
    }
}

void HybridCommandSender::set_video_callback(VideoDataCallback cb) {
    video_callback_ = cb;
    if (usb_sender_) {
        usb_sender_->set_video_callback(cb);
    }
}

// ── Internal HID helpers ──

mirage::AoaHidTouch* HybridCommandSender::get_hid_for_device(const std::string& device_id) {
    auto it = hid_touches_.find(device_id);
    if (it != hid_touches_.end() && it->second && it->second->is_registered()) {
        return it->second.get();
    }
    return nullptr;
}

bool HybridCommandSender::try_hid_tap(const std::string& device_id, int x, int y, int screen_w, int screen_h) {
    auto* hid = get_hid_for_device(device_id);
    if (!hid) return false;
    bool ok = hid->tap(x, y, screen_w, screen_h);
    if (ok) {
        MLOG_INFO("hybridcmd", "HID tap (%d, %d) on %dx%d [%s]", x, y, screen_w, screen_h, device_id.c_str());
    }
    return ok;
}

bool HybridCommandSender::try_hid_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int screen_w, int screen_h, int duration_ms) {
    auto* hid = get_hid_for_device(device_id);
    if (!hid) return false;
    bool ok = hid->swipe(x1, y1, x2, y2, screen_w, screen_h, duration_ms);
    if (ok) {
        MLOG_INFO("hybridcmd", "HID swipe (%d,%d)->(%d,%d) %dms on %dx%d [%s]", x1, y1, x2, y2, duration_ms, screen_w, screen_h, device_id.c_str());
    }
    return ok;
}

// ── Send to specific device (3-tier fallback) ──

uint32_t HybridCommandSender::send_ping(const std::string& device_id) {
    uint32_t seq = usb_sender_ ? usb_sender_->send_ping(device_id) : 0;
    // RTT計測: コマンド送信タイミング記録
    if (seq > 0) rtt_tracker_.record_ping_sent(static_cast<uint16_t>(seq));
    return seq;
}

uint32_t HybridCommandSender::send_tap(const std::string& device_id, int x, int y, int screen_w, int screen_h) {
    // Tier 1: AOA HID (fastest, per-device)
    auto* hid = get_hid_for_device(device_id);
    if (hid && screen_w > 0 && screen_h > 0) {
        if (try_hid_tap(device_id, x, y, screen_w, screen_h)) {
            current_touch_mode_.store(TouchMode::AOA_HID);
            return 1;
        }
        MLOG_WARN("hybridcmd", "AOA HID tap failed for %s, falling back to MIRA USB", device_id.c_str());
    }

    // Tier 2: MIRA protocol via USB bulk
    if (usb_sender_) {
        uint32_t seq = usb_sender_->send_tap(device_id, x, y, screen_w, screen_h);
        if (seq > 0) {
            current_touch_mode_.store(TouchMode::MIRA_USB);
            return seq;
        }
        MLOG_WARN("hybridcmd", "MIRA USB tap failed, falling back to ADB");
    }

    // Tier 3: ADB shell (slowest)
    if (adb_fallback_ && adb_fallback_->is_enabled()) {
        if (adb_fallback_->tap(x, y)) {
            current_touch_mode_.store(TouchMode::ADB_FALLBACK);
            return 1;
        }
    }

    MLOG_ERROR("hybridcmd", "All tap methods failed for device %s", device_id.c_str());
    return 0;
}

uint32_t HybridCommandSender::send_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms, int screen_w, int screen_h) {
    // Tier 1: AOA HID (fastest, per-device)
    auto* hid = get_hid_for_device(device_id);
    if (hid && screen_w > 0 && screen_h > 0) {
        if (try_hid_swipe(device_id, x1, y1, x2, y2, screen_w, screen_h, duration_ms)) {
            current_touch_mode_.store(TouchMode::AOA_HID);
            return 1;
        }
        MLOG_WARN("hybridcmd", "AOA HID swipe failed for %s, falling back to MIRA USB", device_id.c_str());
    }

    // Tier 2: MIRA protocol via USB bulk
    if (usb_sender_) {
        uint32_t seq = usb_sender_->send_swipe(device_id, x1, y1, x2, y2, duration_ms);
        if (seq > 0) {
            current_touch_mode_.store(TouchMode::MIRA_USB);
            return seq;
        }
        MLOG_WARN("hybridcmd", "MIRA USB swipe failed, falling back to ADB");
    }

    // Tier 3: ADB shell (slowest)
    if (adb_fallback_ && adb_fallback_->is_enabled()) {
        if (adb_fallback_->swipe(x1, y1, x2, y2, duration_ms)) {
            current_touch_mode_.store(TouchMode::ADB_FALLBACK);
            return 1;
        }
    }

    MLOG_ERROR("hybridcmd", "All swipe methods failed for device %s", device_id.c_str());
    return 0;
}

uint32_t HybridCommandSender::send_back(const std::string& device_id) {
    // Tier 2: MIRA protocol via USB bulk (no HID for keys)
    if (usb_sender_) {
        uint32_t seq = usb_sender_->send_back(device_id);
        if (seq > 0) {
            return seq;
        }
        MLOG_WARN("hybridcmd", "MIRA USB back failed, falling back to ADB");
    }

    // Tier 3: ADB shell
    if (adb_fallback_ && adb_fallback_->is_enabled()) {
        if (adb_fallback_->back()) {
            return 1;
        }
    }

    MLOG_ERROR("hybridcmd", "All back methods failed for device %s", device_id.c_str());
    return 0;
}

uint32_t HybridCommandSender::send_key(const std::string& device_id, int keycode) {
    // Tier 2: MIRA protocol via USB bulk (no HID for keys)
    if (usb_sender_) {
        uint32_t seq = usb_sender_->send_key(device_id, keycode);
        if (seq > 0) {
            return seq;
        }
        MLOG_WARN("hybridcmd", "MIRA USB key %d failed, falling back to ADB", keycode);
    }

    // Tier 3: ADB shell
    if (adb_fallback_ && adb_fallback_->is_enabled()) {
        if (adb_fallback_->key(keycode)) {
            return 1;
        }
    }

    MLOG_ERROR("hybridcmd", "All key methods failed for device %s keycode %d", device_id.c_str(), keycode);
    return 0;
}

uint32_t HybridCommandSender::send_click_id(const std::string& device_id, const std::string& resource_id) {
    return usb_sender_ ? usb_sender_->send_click_id(device_id, resource_id) : 0;
}

uint32_t HybridCommandSender::send_click_text(const std::string& device_id, const std::string& text) {
    return usb_sender_ ? usb_sender_->send_click_text(device_id, text) : 0;
}

// ── Long press and pinch ──

bool HybridCommandSender::send_long_press(const std::string& device_id, int x, int y, int screen_w, int screen_h, int hold_ms) {
    // Tier 1: AOA HID (per-device)
    auto* hid = get_hid_for_device(device_id);
    if (hid && screen_w > 0 && screen_h > 0) {
        if (hid->long_press(x, y, screen_w, screen_h, hold_ms)) {
            MLOG_INFO("hybridcmd", "HID long press (%d, %d) %dms [%s]", x, y, hold_ms, device_id.c_str());
            current_touch_mode_.store(TouchMode::AOA_HID);
            return true;
        }
        MLOG_WARN("hybridcmd", "AOA HID long press failed for %s, falling back to ADB", device_id.c_str());
    }

    // Tier 3: ADB fallback (no MIRA protocol for long press)
    if (adb_fallback_ && adb_fallback_->is_enabled()) {
        if (adb_fallback_->long_press(x, y, hold_ms)) {
            MLOG_INFO("hybridcmd", "ADB long press (%d, %d) %dms", x, y, hold_ms);
            current_touch_mode_.store(TouchMode::ADB_FALLBACK);
            return true;
        }
    }

    MLOG_ERROR("hybridcmd", "All long press methods failed for device %s", device_id.c_str());
    return false;
}

bool HybridCommandSender::send_pinch(const std::string& device_id, int cx, int cy, int start_dist, int end_dist, int screen_w, int screen_h, int duration_ms) {
    // HID only - pinch requires multitouch (per-device)
    auto* hid = get_hid_for_device(device_id);
    if (hid && screen_w > 0 && screen_h > 0) {
        if (hid->pinch(cx, cy, start_dist, end_dist, screen_w, screen_h, duration_ms)) {
            MLOG_INFO("hybridcmd", "HID pinch (%d, %d) %d->%d %dms [%s]", cx, cy, start_dist, end_dist, duration_ms, device_id.c_str());
            current_touch_mode_.store(TouchMode::AOA_HID);
            return true;
        }
    }

    MLOG_ERROR("hybridcmd", "Pinch failed for device %s (HID-only, no fallback)", device_id.c_str());
    return false;
}

// ── Send to all devices (3-tier fallback per device) ──

int HybridCommandSender::send_tap_all(int x, int y, int screen_w, int screen_h) {
    auto ids = get_device_ids();
    if (ids.empty()) {
        // No MIRA devices - try ADB fallback directly
        if (adb_fallback_ && adb_fallback_->is_enabled()) {
            if (adb_fallback_->tap(x, y)) {
                current_touch_mode_.store(TouchMode::ADB_FALLBACK);
                return 1;
            }
        }
        return 0;
    }

    int count = 0;
    for (const auto& id : ids) {
        if (send_tap(id, x, y, screen_w, screen_h) > 0) {
            count++;
        }
    }
    return count;
}

int HybridCommandSender::send_swipe_all(int x1, int y1, int x2, int y2, int duration_ms, int screen_w, int screen_h) {
    auto ids = get_device_ids();
    if (ids.empty()) {
        // No MIRA devices - try ADB fallback directly
        if (adb_fallback_ && adb_fallback_->is_enabled()) {
            if (adb_fallback_->swipe(x1, y1, x2, y2, duration_ms)) {
                current_touch_mode_.store(TouchMode::ADB_FALLBACK);
                return 1;
            }
        }
        return 0;
    }

    int count = 0;
    for (const auto& id : ids) {
        if (send_swipe(id, x1, y1, x2, y2, duration_ms, screen_w, screen_h) > 0) {
            count++;
        }
    }
    return count;
}

int HybridCommandSender::send_back_all() {
    auto ids = get_device_ids();
    if (ids.empty()) {
        if (adb_fallback_ && adb_fallback_->is_enabled()) {
            if (adb_fallback_->back()) {
                return 1;
            }
        }
        return 0;
    }

    int count = 0;
    for (const auto& id : ids) {
        if (send_back(id) > 0) {
            count++;
        }
    }
    return count;
}

int HybridCommandSender::send_key_all(int keycode) {
    auto ids = get_device_ids();
    if (ids.empty()) {
        if (adb_fallback_ && adb_fallback_->is_enabled()) {
            if (adb_fallback_->key(keycode)) {
                return 1;
            }
        }
        return 0;
    }

    int count = 0;
    for (const auto& id : ids) {
        if (send_key(id, keycode) > 0) {
            count++;
        }
    }
    return count;
}

// ── Legacy API - send to first device ──

uint32_t HybridCommandSender::send_ping() {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_ping(first);
}

uint32_t HybridCommandSender::send_tap(int x, int y, int screen_w, int screen_h) {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_tap(first, x, y, screen_w, screen_h);
}

uint32_t HybridCommandSender::send_swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_swipe(first, x1, y1, x2, y2, duration_ms);
}

uint32_t HybridCommandSender::send_back() {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_back(first);
}

uint32_t HybridCommandSender::send_key(int keycode) {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_key(first, keycode);
}

uint32_t HybridCommandSender::send_click_id(const std::string& resource_id) {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_click_id(first, resource_id);
}

uint32_t HybridCommandSender::send_click_text(const std::string& text) {
    auto first = get_first_device_id();
    return first.empty() ? 0 : send_click_text(first, text);
}

// ── Stats ──

bool HybridCommandSender::usb_connected() const {
    return device_count() > 0;
}

uint64_t HybridCommandSender::usb_commands_sent() const {
    if (!usb_sender_) return 0;

    uint64_t total = 0;
    auto ids = usb_sender_->get_device_ids();
    for (const auto& id : ids) {
        MultiUsbCommandSender::DeviceInfo info;
        if (usb_sender_->get_device_info(id, info)) {
            total += info.commands_sent;
        }
    }
    return total;
}

// ── Touch mode string ──

std::string HybridCommandSender::get_touch_mode_str() const {
    switch (current_touch_mode_.load()) {
        case TouchMode::AOA_HID: return "AOA_HID";
        case TouchMode::MIRA_USB: return "MIRA_USB";
        case TouchMode::ADB_FALLBACK: return "ADB";
        default: return "UNKNOWN";
    }
}

} // namespace gui
