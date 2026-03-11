// =============================================================================
// MirageSystem - GUI Initialization Helpers
// =============================================================================
// Split from gui_main.cpp for maintainability
// Contains: Component initialization functions
// =============================================================================

#include "gui_init.hpp"
#include "stream/monitor_lane_client.hpp"
#include "gui_state.hpp"
#include "gui_command.hpp"
#include "mirage_log.hpp"
#include "mirage_config.hpp"
#include "config_loader.hpp"
#include "event_bus.hpp"
#include "frame_dispatcher.hpp"
#include "bandwidth_monitor.hpp"
#include "winusb_checker.hpp"
#include "route_controller.hpp"
#include "vid0_parser.hpp"

#include <thread>
#include <chrono>
#include <map>
#include <unordered_map>
#include <vector>
#include <tuple>
#include <fstream>
#include <mutex>
using namespace mirage::gui::state;
using namespace mirage::gui::command;

namespace mirage::gui::init {

// Frame source diagnostics (hardware_id -> local TCP port)
static std::mutex g_hwport_m;
static std::unordered_map<std::string, int> g_hw_to_port;

// USB/WiFi frame source switching
// USB is primary: when USB frames arrive, TCP frames are suppressed.
// When USB dies (> USB_FRAME_TIMEOUT_MS), TCP takes over automatically.
static constexpr int64_t USB_FRAME_TIMEOUT_MS = 200;  // USB dead after 200ms of no frames
static constexpr int64_t USB_RECOVERY_HOLD_MS  = 500;  // wait 500ms stable USB before switching back
static std::atomic<int64_t> g_usb_frame_last_ms{0};   // last USB frame timestamp
static std::atomic<bool>    g_usb_active{false};       // currently using USB as primary
static std::atomic<int64_t> g_usb_recovery_since_ms{0}; // when USB frames resumed
static std::atomic<int64_t> g_switch_to_wifi_ms{0};   // timestamp of last USB->WiFi switch
static std::atomic<int64_t> g_switch_to_usb_ms{0};    // timestamp of last WiFi->USB switch



// =========================================================================
// Helper: Auto-start MirageCapture ScreenCaptureService on one device
// =========================================================================
static void autoStartCaptureService(const std::string& adb_id, const std::string& display_name, int tcp_port = 0) {
    if (!g_adb_manager) return;

    std::string svc_check = g_adb_manager->adbCommand(adb_id,
        "shell dumpsys activity services com.mirage.capture");
    if (svc_check.find("ScreenCaptureService") != std::string::npos) {
        MLOG_INFO("gui", "ScreenCaptureService already running on %s", display_name.c_str());
        return;
    }

    MLOG_INFO("gui", "Auto-starting MirageCapture on %s (%s)", display_name.c_str(), adb_id.c_str());
    g_adb_manager->adbCommand(adb_id,
        "shell am start -n com.mirage.capture/.ui.CaptureActivity "
        "--ez auto_mirror true --es mirror_mode tcp --ei tcp_port " + std::to_string(tcp_port > 0 ? tcp_port : 50000));

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::string ui_check = g_adb_manager->adbCommand(adb_id, "shell dumpsys activity top");
    if (ui_check.find("MediaProjectionPermissionActivity") != std::string::npos ||
        ui_check.find("GrantPermissionsActivity") != std::string::npos) {
        // uiautomator dump縺ｧ"Start now"/"莉翫☆縺宣幕蟋・繝懊ち繝ｳ繧呈爾縺・
        bool tapped = false;
        std::string ui_xml = g_adb_manager->adbCommand(adb_id,
            "shell uiautomator dump /dev/stdout");
        if (!ui_xml.empty()) {
            // Step 1: "全体" / "Entire screen" 選択 (Android 14+ の画面選択ダイアログ)
            // タップしないと "Start now" が押せない
            for (const char* label : {"Entire screen", u8"\u5168\u4f53",  // 全体
                                      "Whole screen", "Entire Screen"}) {
                size_t label_pos = ui_xml.find(label);
                if (label_pos == std::string::npos) continue;
                size_t bounds_pos = ui_xml.find("bounds=\"", label_pos);
                size_t node_start = ui_xml.rfind("<node", label_pos);
                if ((bounds_pos == std::string::npos || bounds_pos - label_pos > 500) && node_start != std::string::npos)
                    bounds_pos = ui_xml.find("bounds=\"", node_start);
                if (bounds_pos != std::string::npos && bounds_pos < label_pos + 500) {
                    size_t b_start = ui_xml.find('[', bounds_pos);
                    if (b_start != std::string::npos) {
                        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                        if (sscanf(ui_xml.c_str() + b_start, "[%d,%d][%d,%d]",
                                   &x1, &y1, &x2, &y2) == 4) {
                            int tap_x = (x1 + x2) / 2;
                            int tap_y = (y1 + y2) / 2;
                            g_adb_manager->adbCommand(adb_id,
                                "shell input tap " + std::to_string(tap_x) + " " + std::to_string(tap_y));
                            MLOG_INFO("gui", "Auto-tapped 'Entire screen' on %s (%d,%d)",
                                      display_name.c_str(), tap_x, tap_y);
                            std::this_thread::sleep_for(std::chrono::milliseconds(500));
                            break;
                        }
                    }
                }
            }

            // Step 2: "Start now" / "今すぐ開始" / "START NOW" のboundsを検索
            for (const char* label : {"Start now", "START NOW",
                                      "\\xe4\\xbb\\x8a\\xe3\\x81\\x99\\xe3\\x81\\x90\\xe9\\x96\\x8b\\xe5\\xa7\\x8b"}) {
                size_t label_pos = ui_xml.find(label);
                if (label_pos == std::string::npos) continue;
                // 隧ｲ蠖薙ヮ繝ｼ繝峨・bounds螻樊ｧ繧呈爾縺・ bounds="[x1,y1][x2,y2]"
                size_t bounds_pos = ui_xml.find("bounds=\"", label_pos);
                // bounds螻樊ｧ縺悟燕譁ｹ縺ｫ縺ゅｋ蝣ｴ蜷医ｂ閠・・・医ヮ繝ｼ繝蛾幕蟋九ち繧ｰ蜀・ｼ・
                if (bounds_pos == std::string::npos || bounds_pos - label_pos > 500) {
                    // label_pos縺九ｉ騾・婿蜷代↓bounds繧呈爾縺・
                    size_t node_start = ui_xml.rfind("<node", label_pos);
                    if (node_start != std::string::npos)
                        bounds_pos = ui_xml.find("bounds=\"", node_start);
                }
                if (bounds_pos != std::string::npos && bounds_pos < label_pos + 500) {
                    // 繝代・繧ｹ: bounds="[x1,y1][x2,y2]"
                    size_t b_start = ui_xml.find('[', bounds_pos);
                    if (b_start != std::string::npos) {
                        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                        if (sscanf(ui_xml.c_str() + b_start, "[%d,%d][%d,%d]",
                                   &x1, &y1, &x2, &y2) == 4) {
                            int tap_x = (x1 + x2) / 2;
                            int tap_y = (y1 + y2) / 2;
                            g_adb_manager->adbCommand(adb_id,
                                "shell input tap " + std::to_string(tap_x) + " " + std::to_string(tap_y));
                            MLOG_INFO("gui", "Auto-tapped '%s' on %s (bounds [%d,%d][%d,%d] -> %d,%d)",
                                      label, display_name.c_str(), x1, y1, x2, y2, tap_x, tap_y);
                            tapped = true;
                            break;
                        }
                    }
                }
            }
        }
        // 繝輔か繝ｼ繝ｫ繝舌ャ繧ｯ: uiautomator縺ｧ隕九▽縺九ｉ縺ｪ縺九▲縺溷ｴ蜷医・蝗ｺ螳壽ｯ皮紫縺ｧtap
        if (!tapped) {
            std::string wm_size = g_adb_manager->adbCommand(adb_id, "shell wm size");
            int screen_w = 1080, screen_h = 1920;
            auto parse_wh = [](const std::string& s, int& w, int& h) {
                size_t pos = s.find(": ");
                if (pos == std::string::npos) return false;
                pos += 2;
                size_t x_pos = s.find('x', pos);
                if (x_pos == std::string::npos) return false;
                try { w = std::stoi(s.substr(pos, x_pos - pos));
                      h = std::stoi(s.substr(x_pos + 1)); return true; }
                catch (...) { return false; }
            };
            size_t override_pos = wm_size.find("Override size");
            size_t physical_pos = wm_size.find("Physical size");
            if (override_pos != std::string::npos) parse_wh(wm_size.substr(override_pos), screen_w, screen_h);
            else if (physical_pos != std::string::npos) parse_wh(wm_size.substr(physical_pos), screen_w, screen_h);
            // Fallback Step 1: "全体" を選択 (画面上部 30% 付近に表示されることが多い)
            int entire_x = static_cast<int>(screen_w * 0.50f);
            int entire_y = static_cast<int>(screen_h * 0.30f);
            g_adb_manager->adbCommand(adb_id,
                "shell input tap " + std::to_string(entire_x) + " " + std::to_string(entire_y));
            MLOG_INFO("gui", "Auto-tapped 'Entire screen' fallback on %s (%dx%d -> %d,%d)",
                      display_name.c_str(), screen_w, screen_h, entire_x, entire_y);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            // Fallback Step 2: "今すぐ開始" (画面下部 61% 付近)
            int tap_x = static_cast<int>(screen_w * 0.73f);
            int tap_y = static_cast<int>(screen_h * 0.61f);
            g_adb_manager->adbCommand(adb_id,
                "shell input tap " + std::to_string(tap_x) + " " + std::to_string(tap_y));
            MLOG_INFO("gui", "Auto-tapped 'Start now' fallback on %s (%dx%d -> %d,%d)",
                      display_name.c_str(), screen_w, screen_h, tap_x, tap_y);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }

    std::string verify = g_adb_manager->adbCommand(adb_id,
        "shell dumpsys activity services com.mirage.capture");
    if (verify.find("ScreenCaptureService") != std::string::npos) {
        MLOG_INFO("gui", "ScreenCaptureService started on %s", display_name.c_str());
    } else {
        MLOG_WARN("gui", "ScreenCaptureService failed to start on %s (manual tap may be needed)",
                  display_name.c_str());
    }
}

// =============================================================================
// USB Video Callback Setup
// =============================================================================

void setupUsbVideoCallback() {
    if (!g_hybrid_cmd) return;

    g_hybrid_cmd->set_video_callback([](const std::string& device_id, const uint8_t* data, size_t len) {
        // Resolve USB serial to hardware_id for device unification
        std::string resolved_id = device_id;
        if (g_adb_manager) {
            std::string hw_id = g_adb_manager->resolveUsbSerial(device_id);
            if (!hw_id.empty()) {
                resolved_id = hw_id;
            }
        }

        // Accumulate data in per-device buffer and parse VID0 packets
        mirage::video::ParseResult parse_result;

        {
            std::lock_guard<std::mutex> buf_lock(g_usb_video_buffers_mutex);
            auto& buffer = g_usb_video_buffers[resolved_id];
            buffer.insert(buffer.end(), data, data + len);
            parse_result = mirage::video::parseVid0Packets(buffer);
        }

        auto& rtp_packets = parse_result.rtp_packets;

        if (rtp_packets.empty()) return;

        std::lock_guard<std::mutex> lock(g_usb_decoders_mutex);

        auto it = g_usb_decoders.find(resolved_id);
        if (it == g_usb_decoders.end()) {
            auto decoder = std::make_unique<::gui::MirrorReceiver>();

            // Set Vulkan context for UnifiedDecoder if GUI is initialized
            auto gui = g_gui;
            if (gui && gui->vulkanContext()) {
                auto* vk_ctx = gui->vulkanContext();
                decoder->setVulkanContext(
                    vk_ctx->physicalDevice(),
                    vk_ctx->device(),
                    vk_ctx->queueFamilies().graphics,
                    vk_ctx->graphicsQueue(),
                    vk_ctx->queueFamilies().compute,
                    vk_ctx->computeQueue(),
                    vk_ctx->queueFamilies().video_decode,
                    vk_ctx->videoDecodeQueue()
                );
            }

            if (decoder->init_decoder()) {
                auto [inserted_it, success] = g_usb_decoders.emplace(resolved_id, std::move(decoder));
                it = inserted_it;
                MLOG_INFO("gui", "Created USB decoder for device: %s (raw: %s)", resolved_id.c_str(), device_id.c_str());
            } else {
                MLOG_ERROR("gui", "Failed to create decoder for device: %s", resolved_id.c_str());
                return;
            }
        }

        // Use iterator to avoid repeated map lookups
        if (it != g_usb_decoders.end() && it->second) {
            for (const auto& pkt : rtp_packets) {
                it->second->feed_rtp_packet(pkt.data(), pkt.size());
            }
            // Dispatch latest decoded frame to GUI via EventBus (same path as MultiDeviceReceiver)
            std::shared_ptr<mirage::SharedFrame> sf;
            if (it->second->get_latest_shared_frame(sf) && sf) {
                // Resolve source port for the device
                int src_port = 0;
                {
                    std::lock_guard<std::mutex> lk(g_hwport_m);
                    auto pit = g_hw_to_port.find(resolved_id);
                    if (pit != g_hw_to_port.end()) src_port = pit->second;
                }
                sf->device_id   = resolved_id;
                sf->source_port = src_port;
                // Mark USB frame arrival time for failover logic
                {
                    int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    g_usb_frame_last_ms.store(now_ms, std::memory_order_relaxed);
                    if (!g_usb_active.load(std::memory_order_relaxed)) {
                        // USB just came back: start recovery hold timer
                        int64_t since = g_usb_recovery_since_ms.load(std::memory_order_relaxed);
                        if (since == 0) g_usb_recovery_since_ms.store(now_ms, std::memory_order_relaxed);
                        if ((now_ms - g_usb_recovery_since_ms.load()) >= USB_RECOVERY_HOLD_MS) {
                            g_usb_active.store(true, std::memory_order_relaxed);
                            g_switch_to_usb_ms.store(now_ms, std::memory_order_relaxed);
                            int64_t wifi_ms = g_switch_to_wifi_ms.load(std::memory_order_relaxed);
                            MLOG_INFO("failover", "[FAILOVER] WiFi -> USB recovered (held %.0fms, wifi_duration=%.0fms)",
                                (float)(now_ms - g_usb_recovery_since_ms.load()),
                                wifi_ms > 0 ? (float)(now_ms - wifi_ms) : 0.0f);
                        }
                    } else {
                        g_usb_recovery_since_ms.store(0, std::memory_order_relaxed); // reset recovery timer
                    }
                }
                mirage::dispatcher().dispatchSharedFrame(sf);
            }
        }
    });
}

// =============================================================================
// Initialization Helpers
// =============================================================================

bool initializeMultiReceiver() {
    g_multi_receiver = std::make_unique<::gui::MultiDeviceReceiver>();
    if (!g_adb_manager) {
        g_multi_receiver.reset();
        return false;
    }

    g_multi_receiver->setDeviceManager(g_adb_manager.get());

    // Vulkan繧ｳ繝ｳ繝・く繧ｹ繝医ｒMultiDeviceReceiver縺ｫ險ｭ螳夲ｼ・PU繝・さ繝ｼ繝臥畑・・
    auto gui = g_gui;
    if (gui && gui->vulkanContext()) {
        auto* vk_ctx = gui->vulkanContext();
        g_multi_receiver->setVulkanContext(
            vk_ctx->physicalDevice(), vk_ctx->device(),
            vk_ctx->queueFamilies().graphics, vk_ctx->queueFamilies().compute,
            vk_ctx->graphicsQueue(), vk_ctx->computeQueue());
    }

    // MirrorReceiver繧ｹ繝ｭ繝・ヨ蛻晄悄蛹・(繧ｨ繝ｳ繝医Μ菴懈・ + 繝・さ繝ｼ繝貅門ｙ)
    g_multi_receiver->start(mirage::config::getConfig().network.video_base_port);

    // ── Monitor Lane Client (Phase C-5) ─────────────────────────────────
    // Start MonitorLaneClient for main device (X1 / first device).
    // Decodes UDP :50202 H.264 → RGBA → gui->stageMonitorFrame()
    // DISABLED: Monitor lane not needed for VID0-only mode
    // (stageMonitorFrame was interfering with VID0 recordUpdate slot)

    // ────────────────────────────────────────────────────────────────────

    // MirageCapture APK逶ｴ謗･蜿嶺ｿ｡繝｢繝ｼ繝・
    // 蜷・ョ繝舌う繧ｹ縺ｫ adb forward 繧定ｨｭ螳壹＠縲〃ID0 TCP蜿嶺ｿ｡繧帝幕蟋・
    // MirageCapture APK 縺後く繝｣繝励メ繝｣繝ｻ騾∽ｿ｡繧呈球縺・(scrcpy荳堺ｽｿ逕ｨ)
    // === Auto-start MirageCapture ScreenCaptureService on all devices ===
    for (const auto& dev : g_adb_manager->getUniqueDevices()) {
        autoStartCaptureService(dev.preferred_adb_id, dev.display_name, dev.assigned_tcp_port);
    }

    auto devices = g_adb_manager->getUniqueDevices();
    if (devices.empty()) {
        MLOG_WARN("gui", "Multi-receiver: 繝・ヰ繧､繧ｹ縺瑚ｦ九▽縺九ｊ縺ｾ縺帙ｓ");
        g_multi_receiver.reset();
        return false;
    }

    constexpr int REMOTE_PORT = 50100;  // MirageCapture TcpVideoSender (on-device fixed port)
    constexpr int BASE_LOCAL_PORT = 50000;  // +100 per device index

    

    // Load fixed per-device local tcp ports from devices.json (generated from device_profiles)
    std::unordered_map<std::string,int> fixed_port_map;
    try {
        std::vector<std::string> candidates = {"devices.json", "../devices.json", "../../devices.json", "../../../devices.json"};
        for (const auto& c : candidates) {
            std::ifstream f(c);
            if (!f.is_open()) continue;
            nlohmann::json j = nlohmann::json::parse(f);
            if (j.contains("devices") && j["devices"].is_array()) {
                for (const auto& dj : j["devices"]) {
                    std::string hw = dj.value("hardware_id", "");
                    int port = dj.value("tcp_port", 0);
                    if (!hw.empty() && port > 0) fixed_port_map[hw] = port;
                }
                break;
            }
        }
    } catch (...) {}
int success = 0;
    // TILED_REMOVED: setTiledCallback removed (TileCompositor functionality removed)
    // g_multi_receiver->setTiledCallback(...);

    // Direct callback: GUI queueFrame only, bypasses EventBus (no AI blocking)
    g_multi_receiver->setDirectCallback([](const std::string& hardware_id,
                                           std::shared_ptr<mirage::SharedFrame> frame) {
        auto gui = g_gui;
        if (!gui || !frame) return;
        // Resolve hardware_id -> port for logging
        int source_port = 0;
        {
            std::lock_guard<std::mutex> lk(g_hwport_m);
            auto it = g_hw_to_port.find(hardware_id);
            if (it != g_hw_to_port.end()) source_port = it->second;
        }
        frame->device_id = hardware_id;
        frame->source_port = source_port;
        gui->queueFrame(hardware_id, frame);
    });

        g_multi_receiver->setFrameCallback([](const std::string& hardware_id, std::shared_ptr<mirage::SharedFrame> frame) {
        // Start AI async workers once. Actual device->slot mapping is resolved inside AIEngine
        // from the same GUI-fed FrameReadyEvent stream, so GUI does not special-case main/sub.
        static bool s_async_started = false;
        if (!s_async_started && g_ai_engine && g_ai_enabled) {
            g_ai_engine->setAsyncMode(true);
            s_async_started = true;
        }

        // Use dispatcher for unified SharedFrame delivery.
        // device_id stays hardware_id; GUI/AI both consume the same frame stream.
        std::string device_id = hardware_id;
        int source_port = 0;
        {
            std::lock_guard<std::mutex> lk(g_hwport_m);
            auto it = g_hw_to_port.find(hardware_id);
            if (it != g_hw_to_port.end()) source_port = it->second;
        }

        // SharedFrame direct dispatch (no MirrorFrame::rgba copy)
        // USB/WiFi failover: suppress TCP frames when USB is active
        {
            frame->device_id   = device_id;
            frame->source_port = source_port;
            int64_t now_ms = (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            int64_t usb_last = g_usb_frame_last_ms.load(std::memory_order_relaxed);
            bool usb_alive = (usb_last > 0) && ((now_ms - usb_last) < USB_FRAME_TIMEOUT_MS);
            if (g_usb_active.load(std::memory_order_relaxed) && !usb_alive) {
                // USB just died: switch to WiFi
                g_usb_active.store(false, std::memory_order_relaxed);
                g_usb_recovery_since_ms.store(0, std::memory_order_relaxed);
                g_switch_to_wifi_ms.store(now_ms, std::memory_order_relaxed);
                float lag_ms = usb_last > 0 ? (float)(now_ms - usb_last) : 0.0f;
                MLOG_INFO("failover", "[FAILOVER] USB -> WiFi (lag=%.0fms since last USB frame)", lag_ms);
            }
            if (!g_usb_active.load(std::memory_order_relaxed) || !usb_alive) {
                // WiFi frame: dispatch only when USB is not active
                mirage::dispatcher().dispatchSharedFrame(frame);
            }
            // else: USB is active, TCP frame suppressed
        }
    });

    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        const std::string& adb_id = dev.preferred_adb_id;


        // assigned_tcp_port (devices.json逕ｱ譚･) 縺後≠繧後・菴ｿ縺・√↑縺代ｌ縺ｰ蜍慕噪蜑ｲ蠖・
        int local_port = 0;
        if (dev.assigned_tcp_port > 0) {
            local_port = dev.assigned_tcp_port;
        } else {
            // Stable mapping by ADB id (avoids hardware_id mismatch causing port collisions)
            // Port scheme: 50000 + device_index * 100
            // Each device gets a 100-port block: +0=main, +1=tile, +2=AI(future), ...
            // X1(192.168.0.3)=50000, A9#956(192.168.0.6)=50100, A9#479(192.168.0.8)=50200
            if (adb_id.find("192.168.0.3") != std::string::npos) local_port = 50000;
            else if (adb_id.find("192.168.0.6") != std::string::npos || adb_id.find("A9250700956") != std::string::npos) local_port = 50100;
            else if (adb_id.find("192.168.0.8") != std::string::npos || adb_id.find("A9250700479") != std::string::npos) local_port = 50200;
            else {
                auto itp = fixed_port_map.find(dev.hardware_id);
                if (itp != fixed_port_map.end()) local_port = itp->second;
                else local_port = BASE_LOCAL_PORT + static_cast<int>(i) * 100;
            }
        }
        {
            std::lock_guard<std::mutex> lk(g_hwport_m);
            g_hw_to_port[dev.hardware_id] = local_port;
        }


        // adb forward: Wi-Fi direct接続の場合はforward不要（127.0.0.1経由を避ける）
        if (dev.ip_address.empty()) {
            std::string fwd_cmd = "forward tcp:" + std::to_string(local_port) +
                                  " tcp:" + std::to_string(REMOTE_PORT);
            std::string fwd_result = g_adb_manager->adbCommand(adb_id, fwd_cmd);
            MLOG_INFO("gui", "adb forward: %s -> %s (result: %s)",
                      adb_id.c_str(), fwd_cmd.c_str(), fwd_result.c_str());
        } else {
            // Wi-Fi direct: 古い adb forward が残っていれば削除する
            g_adb_manager->adbCommand(adb_id, "forward --remove tcp:" + std::to_string(local_port));
            MLOG_INFO("gui", "adb forward removed+skipped (Wi-Fi direct): %s ip=%s port=%d",
                      adb_id.c_str(), dev.ip_address.c_str(), local_port);
        }
        // VID0 TCP start - tiled mode for devices with native height > 1440px (e.g. X1 1200x2000)
        {
            // Check if this device needs tiled encoding (native height > HW encoder limit)
            // Prefer devices.json (reliable at startup) over ADB scan result (may be 0)
            int native_h = 0;
            {
                int ew = 0, eh = 0;
                if (mirage::config::ExpectedSizeRegistry::instance().getExpectedSize(dev.hardware_id, ew, eh))
                    native_h = eh;
                else
                    native_h = dev.screen_height;  // fallback to ADB scan
            }

            bool started = false;
            // X1 now uses TiledEncoder on APK side, so enable tiled mode for it
            if (native_h > 2000) {
                // Tiled mode: port0=top, port1=bottom
                // Wi-Fi direct: connect to device IP, no adb forward needed
                std::string tile_host = dev.ip_address.empty() ? "127.0.0.1" : dev.ip_address;
                uint16_t tile_port0 = static_cast<uint16_t>(REMOTE_PORT);  // 50100 on device
                uint16_t tile_port1 = static_cast<uint16_t>(REMOTE_PORT + 1);  // 50101 on device
                
                if (tile_host == "127.0.0.1") {
                    // Fallback to adb forward if no Wi-Fi IP
                    uint16_t port1 = static_cast<uint16_t>(local_port + 1);
                    std::string fwd1 = "forward tcp:" + std::to_string(port1) +
                                       " tcp:" + std::to_string(REMOTE_PORT + 1);
                    g_adb_manager->adbCommand(adb_id, fwd1);
                    tile_port0 = static_cast<uint16_t>(local_port);
                    tile_port1 = port1;
                    MLOG_INFO("gui", "Tiled mode (adb forward): %s port0=%d port1=%d (native_h=%d)",
                              dev.display_name.c_str(), tile_port0, tile_port1, native_h);
                } else {
                    MLOG_INFO("gui", "Tiled mode (Wi-Fi direct): %s -> %s:%d/%d (native_h=%d)",
                              dev.display_name.c_str(), tile_host.c_str(), tile_port0, tile_port1, native_h);
                }
                started = g_multi_receiver->restart_as_tcp_vid0_tiled(
                    dev.hardware_id, tile_port0, tile_port1, tile_host);
            } else {
                std::string tcp_host = dev.ip_address.empty() ? "127.0.0.1" : dev.ip_address;
                { std::string h; if (mirage::config::ExpectedSizeRegistry::instance().getTcpHost(dev.hardware_id, h)) tcp_host = h; }
                MLOG_INFO("mirror", "VID0 TCP host: %s port=%d for %s", tcp_host.c_str(), local_port, dev.display_name.c_str());

                // Send VIDEO_ROUTE(mode=2=TCP) to APK before TCP connect (APK starts TcpVideoSender on receive)
                if (!dev.ip_address.empty() && g_hybrid_cmd) {
                    const uint8_t VIDEO_ROUTE_TCP = 2;
                    g_hybrid_cmd->send_video_route(dev.hardware_id, VIDEO_ROUTE_TCP, tcp_host, local_port);
                    MLOG_INFO("mirror", "Sent VIDEO_ROUTE(TCP) to %s port=%d", dev.hardware_id.c_str(), local_port);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // Wait for APK to start TcpVideoSender
                }

                started = g_multi_receiver->restart_as_tcp_vid0(dev.hardware_id, local_port, tcp_host);
            }
            if (started) success++;
            else MLOG_ERROR("gui", "VID0 TCP start failed: %s port=%d", dev.display_name.c_str(), local_port);
        }
    }

    MLOG_INFO("gui", "Multi-receiver: %d/%zu devices VID0 TCP mode",
              success, devices.size());

    // 繝輔Ξ繝ｼ繝繧ｳ繝ｼ繝ｫ繝舌ャ繧ｯ險ｭ螳・ 繝・さ繝ｼ繝画ｸ医∩繝輔Ξ繝ｼ繝繧脱ventBus邨檎罰縺ｧGUI縺ｫ驟堺ｿ｡
    // Frame callback: decoded frames -> EventBus for GUI/AI/Macro (unified path)

    return success > 0;
}


void initializeHybridCommand() {
    g_hybrid_cmd = std::make_unique<::gui::HybridCommandSender>();

    g_hybrid_cmd->set_ack_callback([](const std::string& device_id, uint32_t seq, uint8_t status) {
        MLOG_INFO("usbcmd", "ACK %s seq=%u status=%d", device_id.c_str(), seq, status);
    });

    setupUsbVideoCallback();

    // Layer 0 support: ユーザー操作検出 → AIEngine.notifyUserInput → VDE STANDBY遷移
#ifdef USE_AI
    g_hybrid_cmd->setUserInputCallback([](const std::string& device_id) {
        if (g_ai_engine) {
            g_ai_engine->notifyUserInput(device_id);
        }
    });
#endif

    g_hybrid_cmd->set_aoa_auto_switch(mirage::config::getConfig().aoa.auto_switch);
    MLOG_INFO("gui", "AOA auto-switch: %s",
              mirage::config::getConfig().aoa.auto_switch ? "enabled" : "disabled");
    if (g_hybrid_cmd->start()) {
        // Always set ADB fallback device (used when USB HID fails e.g. WiFi-only mode)
        if (auto* fb = g_hybrid_cmd->get_adb_fallback()) {
            auto devs = g_adb_manager->getUniqueDevices();
            if (!devs.empty()) {
                fb->set_device(devs[0].preferred_adb_id);
                MLOG_INFO("gui", "ADB fallback device set: %s", devs[0].preferred_adb_id.c_str());
            }
        }
        auto device_ids = g_hybrid_cmd->get_device_ids();
        if (device_ids.empty()) {
            MLOG_INFO("gui", "USB AOA: 0 devices found (ADB fallback will be used for commands)");
            if (mirage::WinUsbChecker::anyDeviceNeedsWinUsb()) {
                auto summary = mirage::WinUsbChecker::getDiagnosticSummary();
                MLOG_WARN("gui", "USB AOA unavailable - WinUSB driver needed: %s", summary.c_str());
            }
        } else {
            MLOG_INFO("gui", "USB AOA: %d device(s) connected", (int)device_ids.size());
            for (const auto& id : device_ids) {
                MLOG_INFO("gui", "  USB device: %s", id.c_str());
            }
        }
    } else {
        MLOG_WARN("gui", "USB command sender failed to start (ADB fallback will be used)");
        // NOTE: Do NOT reset g_hybrid_cmd here - lambda callbacks may still reference it
        // and cause use-after-free. Keep the object alive; device_count() returns 0.
    }

    // Register WiFi TCP command senders (Tier 2.5) from devices.json
    // cmd_port = tcp_port + 1 per device block scheme
    try {
        std::vector<std::string> cands = {"devices.json", "../devices.json",
                                          "../../devices.json", "../../../devices.json"};
        for (const auto& cand : cands) {
            std::ifstream fjson(cand);
            if (!fjson.is_open()) continue;
            nlohmann::json jd = nlohmann::json::parse(fjson);
            if (!jd.contains("devices")) break;
            for (const auto& dj : jd["devices"]) {
                std::string hw_id = dj.value("hardware_id", "");
                std::string ip    = dj.value("ip_address", "");
                int cmd_port      = dj.value("cmd_port", 0);
                if (hw_id.empty() || ip.empty() || cmd_port <= 0) continue;
                g_hybrid_cmd->register_wifi_device(hw_id, ip, (uint16_t)cmd_port);
            }
            break;
        }
    } catch (const std::exception& e) {
        MLOG_WARN("gui", "WiFi TCP sender init failed: %s", e.what());
    }
}

// =============================================================================
// Route Evaluation Thread
// =============================================================================
// Polls bandwidth stats every second and calls RouteController::evaluate().
// Sends FPS and route commands via callbacks registered in initializeRouting().
static void startRouteEvalThread() {
    g_route_eval_running.store(true);
    g_route_eval_thread = std::thread([]() {
      try {
        MLOG_INFO("RouteEval", "Evaluation thread started");
        uint64_t prev_usb_bytes = 0;
        uint64_t prev_wifi_bytes = 0;
        int log_counter = 0;

        while (g_route_eval_running.load() && g_running.load()) {
            if (g_bandwidth_monitor && g_route_controller) {
                // Feed USB bandwidth data
                if (g_hybrid_cmd) {
                    uint64_t usb_bytes = g_hybrid_cmd->total_bytes_received();
                    if (usb_bytes > prev_usb_bytes) {
                        g_bandwidth_monitor->recordUsbRecv(usb_bytes - prev_usb_bytes);
                        prev_usb_bytes = usb_bytes;
                    }
                }

                // Feed WiFi bandwidth data
                if (g_multi_receiver) {
                    auto stats = g_multi_receiver->getStats();
                    uint64_t wifi_bytes = 0;
                    for (const auto& s : stats) {
                        wifi_bytes += s.bytes;
                    }
                    if (wifi_bytes > prev_wifi_bytes) {
                        g_bandwidth_monitor->recordWifiRecv(wifi_bytes - prev_wifi_bytes);
                        prev_wifi_bytes = wifi_bytes;
                    }
                }

                g_bandwidth_monitor->updateStats();

                auto usb_stats = g_bandwidth_monitor->getUsbStats();
                auto wifi_stats = g_bandwidth_monitor->getWifiStats();
                auto decision = g_route_controller->evaluate(usb_stats, wifi_stats);

                // Log state every 10 seconds
                if (++log_counter % 10 == 0) {
                    MLOG_INFO("RouteEval", "State=%d USB=%.1fMbps(cong=%d,alive=%d) WiFi=%.1fMbps(alive=%d) MainFPS=%d SubFPS=%d",
                        (int)decision.state,
                        usb_stats.bandwidth_mbps, usb_stats.is_congested, usb_stats.is_alive,
                        wifi_stats.bandwidth_mbps, wifi_stats.is_alive,
                        decision.main_fps, decision.sub_fps);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        MLOG_INFO("RouteEval", "Evaluation thread ended");
      } catch (const std::exception& e) {
        MLOG_ERROR("RouteEval", "Exception: %s", e.what());
      } catch (...) {
        MLOG_ERROR("RouteEval", "Unknown exception");
      }
    });
    MLOG_INFO("gui", "Route evaluation started");
}

// =============================================================================
// FPS Command Callback
// =============================================================================

static int computePolicyMaxSizeFromNative(int native_w, int native_h, bool reduced_mode) {
    const int short_side = std::min(native_w, native_h);
    const int long_side = std::max(native_w, native_h);
    if (short_side <= 0 || long_side <= 0) return reduced_mode ? 1800 : 2000;

    const bool exceeds_fhd = (short_side > 1080) || (long_side > 1920);
    if (!exceeds_fhd) {
        return long_side;  // FHD以下はネイティブ維持
    }

    // FHD超えは百分率縮小。現在は90%ルール。
    const double scale = reduced_mode ? 0.90 : 0.90;
    int scaled = static_cast<int>(long_side * scale + 0.5);
    if (scaled < 1440) scaled = 1440;
    if (scaled > long_side) scaled = long_side;
    return scaled;
}

static void onQualityCommand(const std::string& device_id, int encode_max_size) {
    // Send ACTION_VIDEO_MAXSIZE broadcast to Android using common policy:
    // native > FHD => percentage downscale, FHD-or-less => native resolution.
    if (!g_adb_manager) return;
    const bool reduced_mode = (encode_max_size <= 1500);
    auto devices = g_adb_manager->getUniqueDevices();
    for (const auto& dev : devices) {
        if (dev.hardware_id != device_id && dev.preferred_adb_id != device_id) continue;
        if (dev.wifi_connections.empty() && dev.preferred_adb_id.empty()) continue;
        std::string adb_id = dev.wifi_connections.empty() ? dev.preferred_adb_id : dev.wifi_connections[0];
        const int native_w = dev.screen_width;
        const int native_h = dev.screen_height;
        const int target_max_size = computePolicyMaxSizeFromNative(native_w, native_h, reduced_mode);
        std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE"
                          " -p com.mirage.capture --ei max_size " + std::to_string(target_max_size);
        std::thread([adb_id, cmd, device_id, target_max_size, native_w, native_h, reduced_mode]() {
            if (g_adb_manager) g_adb_manager->adbCommand(adb_id, cmd);
            MLOG_INFO("RouteCtrl", "Phase E: max_size=%d sent to %s via %s (native=%dx%d reduced=%d)",
                      target_max_size, device_id.c_str(), adb_id.c_str(), native_w, native_h, reduced_mode ? 1 : 0);
        }).detach();
        break;
    }
}

static void onFpsCommand(const std::string& device_id, int fps) {
    const bool useTcpOnly = g_route_controller && g_route_controller->isTcpOnlyMode();
    bool sent_via_adb = false;
    if (useTcpOnly && g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        std::string adb_id;
        for (const auto& dev : devices) {
            if (dev.hardware_id == device_id) {
                adb_id = dev.preferred_adb_id;
                break;
            }
        }
        if (!adb_id.empty()) {
            std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS -p com.mirage.capture --ei target_fps " + std::to_string(fps) + " --ei fps " + std::to_string(fps);
            std::thread([adb_id, cmd, device_id, fps]() {
                if (g_adb_manager) { g_adb_manager->adbCommand(adb_id, cmd); }
                MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s via ADB broadcast (%s)", fps, device_id.c_str(), adb_id.c_str());
            }).detach();
            sent_via_adb = true;
        }
    }
    if (!sent_via_adb && g_hybrid_cmd) {
        g_hybrid_cmd->send_video_fps(device_id, fps);
        MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s (USB)", fps, device_id.c_str());
    }
}

// =============================================================================
// Route Command Callback
// =============================================================================
static void onRouteCommand(const std::string& device_id,
                           ::gui::RouteController::VideoRoute route,
                           const std::string& host, int port) {
    // Skip RouteController auto-switching for TCP-direct devices (USBLAN/WiFi with ip_address)
    // These devices use explicit VIDEO_ROUTE(TCP) sent in initializeMultiReceiver()
    if (g_adb_manager) {
        auto devs = g_adb_manager->getUniqueDevices();
        for (const auto& d : devs) {
            if (d.hardware_id == device_id && !d.ip_address.empty()) {
                MLOG_INFO("RouteCtrl", "Skipped Route=%s for TCP-direct device %s (has ip_address)",
                          (route == ::gui::RouteController::VideoRoute::WIFI) ? "WiFi" : "USB", device_id.c_str());
                return;
            }
        }
    }
    if (g_hybrid_cmd) {
        uint8_t mode = (route == ::gui::RouteController::VideoRoute::WIFI) ? 1 : 0;
        g_hybrid_cmd->send_video_route(device_id, mode, host, port);
        MLOG_INFO("RouteCtrl", "Sent Route=%s to %s (%s:%d)", mode ? "WiFi" : "USB", device_id.c_str(), host.c_str(), port);
    }
}

// =============================================================================
// Device Registration for RouteController
// =============================================================================
// Registers USB-AOA devices (preferred) or TCP/WiFi devices (TCP-only fallback)
// into the RouteController. Called once at startup.
static void registerDevicesForRouteController() {
    const int base_port = mirage::config::getConfig().network.video_base_port;

    if (!g_route_controller->isTcpOnlyMode() && g_hybrid_cmd) {
        // USB AOA mode: register by USB serial, X1 (93020523431940) always main
        auto device_ids = g_hybrid_cmd->get_device_ids();
        // Find X1 serial among registered USB devices
        const std::string X1_SERIAL = "93020523431940";
        bool hasX1 = false;
        for (const auto& id : device_ids) {
            if (id.find(X1_SERIAL) != std::string::npos) { hasX1 = true; break; }
        }
        int wifi_port = base_port;
        bool first = true;
        for (const auto& id : device_ids) {
            const bool isX1 = (id.find(X1_SERIAL) != std::string::npos);
            const bool mainFlag = hasX1 ? isX1 : first;
            g_route_controller->registerDevice(id, mainFlag, wifi_port);
            MLOG_INFO("gui", "RouteController USB AOA: registered %s main=%d", id.c_str(), (int)mainFlag);
            wifi_port++;
            first = false;
        }
        if (device_ids.empty()) {
            MLOG_INFO("gui", "RouteController: no USB devices, operating in WiFi-only mode (ADB fallback active)");
        } else {
            MLOG_INFO("gui", "RouteController: registered %d USB device(s)", (int)device_ids.size());
        }
        return;
    }

    // TCP-only mode: register by ADB hardware_id (must match FrameDispatcher)
    if (!g_adb_manager) {
        MLOG_INFO("gui", "RouteController: no USB command sender, WiFi-only mode");
        return;
    }

    auto devices = g_adb_manager->getUniqueDevices();
    if (devices.empty()) {
        MLOG_INFO("gui", "RouteController: no USB command sender, WiFi-only mode");
        return;
    }

    // Clear any USB-serial registrations left from non-TCP-only path
    if (g_hybrid_cmd) {
        for (const auto& uid : g_hybrid_cmd->get_device_ids()) {
            g_route_controller->unregisterDevice(uid);
        }
    }

    // Prefer X1 as main device
    auto isX1 = [](const auto& dev) {
        if (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) return true;
        for (const auto& w : dev.wifi_connections)  { if (w.find("192.168.0.3:5555") != std::string::npos) return true; }
        for (const auto& u : dev.usb_connections)   { if (u.find("93020523431940") != std::string::npos) return true; }
        return (dev.display_name.find("Npad X1") != std::string::npos) ||
               (dev.display_name.find("N-one Npad X1") != std::string::npos);
    };

    bool hasX1 = std::any_of(devices.begin(), devices.end(), isX1);
    int wifi_port = base_port;
    bool first = true;
    for (const auto& dev : devices) {
        const bool mainFlag = hasX1 ? isX1(dev) : first;
        g_route_controller->registerDevice(dev.hardware_id, mainFlag, wifi_port);
        MLOG_INFO("gui", "RouteController TCP_ONLY: registered %s (%s) main=%d",
                  dev.hardware_id.c_str(), dev.display_name.c_str(), (int)mainFlag);
        wifi_port++;
        first = false;
    }
}

void initializeRouting() {
    // Initialize routing even without USB devices - WiFi-only mode is valid
    g_bandwidth_monitor = std::make_unique<::gui::BandwidthMonitor>();
    g_route_controller = std::make_unique<::gui::RouteController>();


    // Set TCP-only mode based on USB device availability.
    // USB AOA (dual-channel) is preferred when devices are available.
    {
        bool hasUsbDevices = g_hybrid_cmd && (g_hybrid_cmd->device_count() > 0);
        g_route_controller->setTcpOnlyMode(!hasUsbDevices);
        if (hasUsbDevices) {
            MLOG_INFO("gui", "RouteController: USB AOA mode (%d device(s)) - dual-channel active",
                      g_hybrid_cmd->device_count());
        } else {
            MLOG_INFO("gui", "RouteController: TCP-only mode (no USB devices available)");
        }
    }


    // FPS command callback
    g_route_controller->setFpsCommandCallback([](const std::string& device_id, int fps) { onFpsCommand(device_id, fps); });

    // Quality command callback (Phase E)
    g_route_controller->setQualityCommandCallback([](const std::string& device_id, int max_size) { onQualityCommand(device_id, max_size); });

    // Route command callback
    g_route_controller->setRouteCommandCallback([](const std::string& device_id, ::gui::RouteController::VideoRoute route, const std::string& host, int port) { onRouteCommand(device_id, route, host, port); });

    registerDevicesForRouteController();

    startRouteEvalThread();
}

// =============================================================================
// Start Mirroring Handler (extracted from initializeGUI callback)
// =============================================================================
static void onStartMirroring() {
    auto gui = g_gui;
    if (!g_adb_manager) {
        if (gui) gui->logError(u8"ADB manager not initialized");
        return;
    }
    if (gui) gui->logInfo(u8"Starting mirroring on all devices...");
    auto all_devs = g_adb_manager->getUniqueDevices();
    for (const auto& dev : all_devs) {
        autoStartCaptureService(dev.preferred_adb_id, dev.display_name, dev.assigned_tcp_port);
    }
    if (gui) gui->logInfo(u8"Mirroring started on " + std::to_string(all_devs.size()) + u8" device(s)");
}

// =============================================================================
// Event Bus Subscription Registration (extracted from initializeGUI)
// =============================================================================
static void registerEventBusSubscriptions() {
    // DeviceConnectedEvent: add to GUI (hardware_id format only, skip raw USB serials)
    auto sub_connect = mirage::bus().subscribe<mirage::DeviceConnectedEvent>(
        [](const mirage::DeviceConnectedEvent& e) {
            auto gui = g_gui;
            if (!gui) return;
            const auto& id = e.device_id;
            bool is_hardware_id = (id.size() > 9 && id[8] == '_');
            if (!is_hardware_id) {
                MLOG_DEBUG("gui", "Skipping USB device (not hardware_id): %s", id.c_str());
                return;
            }
            gui->addDevice(e.device_id, e.display_name);
            gui->logInfo(u8"Device connected: " + e.display_name);
        });
    sub_connect.release();

    // FrameReadyEvent: forward frames to GUI
    auto sub_frame = mirage::bus().subscribe<mirage::FrameReadyEvent>(
        [](const mirage::FrameReadyEvent& e) {
            auto gui = g_gui;
            static std::mutex s_mu;
            static std::unordered_map<std::string, std::tuple<int,int,int>> s_last;
            static std::unordered_map<std::string, uint64_t> s_last_log_ms;
            {
                std::lock_guard<std::mutex> lk(s_mu);
                auto cur = std::make_tuple(e.source_port, e.width, e.height);
                auto it = s_last.find(e.device_id);
                bool changed = (it == s_last.end() || it->second != cur);
                uint64_t now_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                uint64_t& last_ms = s_last_log_ms[e.device_id];
                bool due = (now_ms - last_ms) >= 2000; // log at most once per 2s per device
                if (changed || due) {
                    s_last[e.device_id] = cur;
                    last_ms = now_ms;
                    std::string s = "id=" + e.device_id + " port=" + std::to_string(e.source_port) + " " + std::to_string(e.width) + "x" + std::to_string(e.height) + " frame_id=" + std::to_string((unsigned long long)e.frame_id);
                    MLOG_INFO("ui", "FrameIn: %s", s.c_str());
                }
            }
            // queueFrame now handled by setDirectCallback (bypasses EventBus)
            // if (gui) { if (e.frame) { gui->queueFrame(e.device_id, e.frame); } }
        });
    sub_frame.release();

    // DeviceDisconnectedEvent: log warning
    auto sub_disconnect = mirage::bus().subscribe<mirage::DeviceDisconnectedEvent>(
        [](const mirage::DeviceDisconnectedEvent& e) {
            auto gui = g_gui;
            if (gui) gui->logWarning(u8"Device disconnected: " + e.device_id);
        });
    sub_disconnect.release();

    // DeviceStatusEvent: update device stats and status
    auto sub_status = mirage::bus().subscribe<mirage::DeviceStatusEvent>(
        [](const mirage::DeviceStatusEvent& e) {
            auto gui = g_gui;
            if (gui) {
                gui->updateDeviceStatus(e.device_id, static_cast<mirage::gui::DeviceStatus>(e.status));
                gui->updateDeviceStats(e.device_id, e.fps, e.latency_ms, e.bandwidth_mbps);
            }
        });
    sub_status.release();

    MLOG_INFO("main", "Event bus subscriptions registered");
}

// =============================================================================
// Device Selection Handler (extracted from initializeGUI callback)
// =============================================================================
// Updates RouteController main device, sends 60/30 fps commands to all devices
// via ADB broadcast (TCP-only mode) or USB AOA (hybrid mode).
static void onDeviceSelected(const std::string& device_id) {
    auto gui = g_gui;
    if (gui) gui->logInfo(u8"Selected: " + device_id);

    // Update RouteController main device
    // RouteController may be registered by USB serial (AOA mode), not hardware_id.
    // Resolve hardware_id -> USB serial if needed.
    if (g_route_controller) {
        std::string rc_device_id = device_id;
        if (!g_route_controller->isRegistered(device_id) && g_adb_manager) {
            // Try to find USB serial for this hardware_id
            ::gui::AdbDeviceManager::UniqueDevice ud;
            if (g_adb_manager->getUniqueDevice(device_id, ud) && !ud.usb_serial.empty()) {
                rc_device_id = ud.usb_serial;
                MLOG_INFO("gui", "setMainDevice: resolved %s -> %s (USB serial)", device_id.c_str(), rc_device_id.c_str());
            } else {
                // Last resort: X1 known serial
                if (device_id.find("f1925da3") != std::string::npos) {
                    rc_device_id = "93020523431940";
                    MLOG_INFO("gui", "setMainDevice: X1 hardcoded serial fallback -> %s", rc_device_id.c_str());
                }
            }
        }
        g_route_controller->setMainDevice(rc_device_id);
    }

    // Update FPS: main=60fps, sub=30fps
    if (g_route_controller && g_route_controller->isTcpOnlyMode() && g_adb_manager) {
        // TCP-only: ADB broadcast (async to avoid blocking GUI thread)
        auto devices = g_adb_manager->getUniqueDevices();
        std::string sel_id = device_id;
        std::thread([devices, sel_id]() {
            for (const auto& dev : devices) {
                const bool isX1sel = (dev.hardware_id.find("f1925da3") != std::string::npos);
                int target_fps = (dev.hardware_id == sel_id) ? (isX1sel ? 90 : 60) : 30;
                std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS -p com.mirage.capture --ei target_fps "
                                  + std::to_string(target_fps) + " --ei fps " + std::to_string(target_fps);
                if (g_adb_manager) g_adb_manager->adbCommand(dev.preferred_adb_id, cmd);
                MLOG_INFO("gui", "FPS update (ADB): %s -> %d fps (%s)",
                          dev.hardware_id.c_str(), target_fps,
                          (dev.hardware_id == sel_id) ? "MAIN" : "sub");
            }
        }).detach();
    } else if (g_hybrid_cmd) {
        // USB AOA: build USB serial -> hardware_id map, then send via HybridCommandSender
        std::map<std::string, std::string> usb_to_hw;
        if (g_adb_manager) {
            for (const auto& dev : g_adb_manager->getUniqueDevices()) {
                for (const auto& serial : dev.usb_connections) {
                    usb_to_hw[serial] = dev.hardware_id;
                }
            }
        }
        auto all_ids = g_hybrid_cmd->get_device_ids();
        for (const auto& uid : all_ids) {
            std::string hw_id = uid;
            auto it = usb_to_hw.find(uid);
            if (it != usb_to_hw.end()) hw_id = it->second;
            int target_fps = (hw_id == device_id) ? 60 : 30;
            g_hybrid_cmd->send_video_fps(uid, target_fps);
            MLOG_INFO("gui", "FPS update (USB): %s -> %d fps (%s)",
                      uid.c_str(), target_fps,
                      (hw_id == device_id) ? "MAIN" : "sub");
        }
        // ADB broadcast fallback: covers WiFi-only devices (e.g. X1 in TiledEncoder/TCP mode)
        // that have no USB AOA connection but are reachable via ADB.
        if (g_adb_manager) {
            auto adb_devices = g_adb_manager->getUniqueDevices();
            std::thread([adb_devices, device_id]() {
                for (const auto& dev : adb_devices) {
                    int target_fps = (dev.hardware_id == device_id) ? 60 : 30;
                    std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS"
                        " -p com.mirage.capture --ei target_fps " + std::to_string(target_fps)
                        + " --ei fps " + std::to_string(target_fps);
                    if (g_adb_manager) {
                        g_adb_manager->adbCommand(dev.preferred_adb_id, cmd);
                        MLOG_INFO("gui", "FPS update (ADB fallback): %s -> %d fps",
                                  dev.hardware_id.c_str(), target_fps);
                    }
                }
            }).detach();
        }
    }
}

void initializeGUI(HWND hwnd) {
    MLOG_INFO("gui", "=== initializeGUI called, hwnd=%p ===", (void*)hwnd);
    g_gui = std::make_shared<mirage::gui::GuiApplication>();

    mirage::gui::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;
    config.vsync = false;

    if (!g_gui->initialize(hwnd, config)) {
        MLOG_ERROR("gui", "GUI initialization failed");
        return;
    }

    if (g_adb_manager) {
        g_gui->setAdbDeviceManager(g_adb_manager.get());
    }

    // Set callbacks
    g_gui->setTapCallback([](const std::string& device_id, int x, int y) {
        sendTapCommand(device_id, x, y);
    });

    g_gui->setSwipeCallback([](const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms) {
        sendSwipeCommand(device_id, x1, y1, x2, y2, duration_ms);
    });

    g_gui->setDeviceSelectCallback([](const std::string& device_id) { onDeviceSelected(device_id); });

    g_gui->setLearningDataCallback([](const mirage::gui::LearningClickData& data) {
        std::string msg = u8"Learning: (" + std::to_string(data.click_x) + "," +
                          std::to_string(data.click_y) + u8") scene=" + data.scene_name;
        auto gui = g_gui;
        if (gui) gui->logDebug(msg);
    });

    g_gui->setStartMirroringCallback([]() { onStartMirroring(); });

    g_gui->logInfo(u8"MirageSystem v2 GUI started");

    // === Event Bus Subscriptions (app-lifetime) ===
    registerEventBusSubscriptions();
    g_gui->logInfo(u8"Ctrl+L: Toggle learning mode");

    if (g_hybrid_cmd) {
        int usb_count = g_hybrid_cmd->device_count();
        if (usb_count > 0) {
            g_gui->logInfo(u8"Control: USB AOA x" + std::to_string(usb_count) + u8" (all devices synchronized)");
        } else {
            g_gui->logInfo(u8"Control: ADB fallback mode (USB AOA not connected)");
        }
    } else {
        g_gui->logInfo(u8"Control: ADB fallback mode");
    }
}

#ifdef USE_AI
void initializeAI() {
    auto gui = g_gui;
    g_ai_engine = std::make_unique<mirage::ai::AIEngine>();
    mirage::ai::AIConfig ai_config;
    ai_config.templates_dir     = mirage::config::getConfig().ai.templates_dir;
    ai_config.default_threshold = mirage::config::getConfig().ai.default_threshold;
    ai_config.enable_multi_scale = true;

    // Apply VDE config from system config (if available)
    const auto& ai_cfg = mirage::config::getConfig().ai;
    if (ai_cfg.vde_confirm_count > 0)
        ai_config.vde_confirm_count = ai_cfg.vde_confirm_count;
    if (ai_cfg.vde_cooldown_ms > 0)
        ai_config.vde_cooldown_ms = ai_cfg.vde_cooldown_ms;
    if (ai_cfg.vde_debounce_window_ms > 0)
        ai_config.vde_debounce_window_ms = ai_cfg.vde_debounce_window_ms;

    // Layer 3 (OllamaVision) 設定反映
    ai_config.vde_enable_layer3          = ai_cfg.vde_enable_layer3;
    ai_config.vde_layer3_no_match_frames = ai_cfg.vde_layer3_no_match_frames;
    ai_config.vde_layer3_stuck_frames    = ai_cfg.vde_layer3_stuck_frames;
    ai_config.vde_layer3_no_match_ms     = ai_cfg.vde_layer3_no_match_ms;
    ai_config.vde_layer3_cooldown_ms     = ai_cfg.vde_layer3_cooldown_ms;

    // OllamaVision 設定 (config.json の ollama ブロック)
    {
        const auto& oc = mirage::config::getConfig().ollama;
        if (!oc.host.empty())   ai_config.ollama_host       = oc.host;
        if (oc.port > 0)        ai_config.ollama_port       = oc.port;
        if (!oc.model.empty())  ai_config.ollama_model      = oc.model;
        if (oc.timeout_ms > 0)  ai_config.ollama_timeout_ms = oc.timeout_ms;
        if (oc.max_tokens > 0)  ai_config.ollama_max_tokens = oc.max_tokens;
    }

    // Pass VulkanContext for GPU compute backend
    mirage::vk::VulkanContext* vk_ctx = gui ? gui->vulkanContext() : nullptr;
    auto initResult = g_ai_engine->initialize(ai_config, vk_ctx);
    if (initResult.is_ok()) {
        if (gui) gui->logInfo(u8"AI engine initialized");
        auto loadResult = g_ai_engine->loadTemplatesFromDir(ai_config.templates_dir);
        if (loadResult.is_ok()) {
            auto stats = g_ai_engine->getStats();
            if (gui) gui->logInfo(u8"AI templates loaded: " + std::to_string(stats.templates_loaded));
        }

        // 無視リスト読込 (永続化)
        std::string ignore_path = ai_config.templates_dir + "/ignored_templates.json";
        g_ai_engine->loadIgnoredTemplates(ignore_path);

        // NOTE: AI繧｢繧ｯ繧ｷ繝ｧ繝ｳ螳溯｡後・EventBus邨檎罰繝代う繝励Λ繧､繝ｳ
        // (decideAction 竊・TapCommandEvent/KeyCommandEvent 竊・gui_command雉ｼ隱ｭ)
        // action_callback_縺ｯ蠕梧婿莠呈鋤繝ｻ繝ｭ繧ｰ逕ｨ縺ｮ縺ｿ
        g_ai_engine->setActionCallback([](int slot, const mirage::ai::AIAction& action) {
            MLOG_DEBUG("ai", "ActionCallback(蠕梧婿莠呈鋤): slot=%d type=%d",
                       slot, (int)action.type);
        });

        // Wire CanSendCallback: allow AI actions when USB or ADB path is available
        g_ai_engine->setCanSendCallback([]() -> bool {
            // USB AOA path
            if (g_hybrid_cmd && g_hybrid_cmd->device_count() > 0) return true;
            // ADB fallback path (sendTapCommand handles ADB fallback)
            if (g_adb_manager) {
                auto devices = g_adb_manager->getUniqueDevices();
                if (!devices.empty()) return true;
            }
            return false;
        });
    } else {
        if (gui) gui->logWarning(u8"AI engine failed to initialize");
        g_ai_engine.reset();
    }

    // Initialize AiJpegReceiver (lazy start from GUI)
    g_ai_jpeg_receiver = std::make_unique<mirage::ai::AiJpegReceiver>();
    g_ai_jpeg_receiver->setFrameCallback([](const std::string& device_id,
                                            const std::vector<uint8_t>& jpeg,
                                            int width, int height,
                                            int64_t /*timestamp_us*/) {
        // Log frame receipt (future: feed to AIEngine for analysis)
        static uint64_t s_frame_count = 0;
        if (++s_frame_count % 30 == 1) {  // Log every 30 frames
            MLOG_DEBUG("ai.jpeg", "Frame #%llu from %s: %dx%d, %zu bytes",
                (unsigned long long)s_frame_count, device_id.c_str(),
                width, height, jpeg.size());
        }
    });
    MLOG_INFO("gui", "AiJpegReceiver initialized (start from GUI)");
}
#endif

#ifdef MIRAGE_OCR_ENABLED
void initializeOCR() {
    auto gui = g_gui;
    g_frame_analyzer = std::make_unique<mirage::FrameAnalyzer>();
    if (g_frame_analyzer->init("jpn+eng")) {
        g_frame_analyzer->startCapture();  // EventBus subscribe (x1_canonical)
        if (gui) gui->logInfo(u8"FrameAnalyzer (Tesseract) initialized");
        MLOG_INFO("gui", "FrameAnalyzer initialized (jpn+eng)");
    } else {
        if (gui) gui->logWarning(u8"FrameAnalyzer init failed");
        MLOG_WARN("gui", "FrameAnalyzer init failed");
        g_frame_analyzer.reset();
    }
}
#endif

} // namespace mirage::gui::init









