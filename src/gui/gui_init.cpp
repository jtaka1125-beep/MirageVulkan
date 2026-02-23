// =============================================================================
// MirageSystem - GUI Initialization Helpers
// =============================================================================
// Split from gui_main.cpp for maintainability
// Contains: Component initialization functions
// =============================================================================

#include "gui_init.hpp"
#include "gui_state.hpp"
#include "gui_command.hpp"
#include "mirage_log.hpp"
#include "mirage_config.hpp"
#include "config_loader.hpp"
#include "event_bus.hpp"
#include "bandwidth_monitor.hpp"
#include "winusb_checker.hpp"
#include "route_controller.hpp"
#include "vid0_parser.hpp"

#include <thread>
#include <map>

using namespace mirage::gui::state;
using namespace mirage::gui::command;

namespace mirage::gui::init {

// =========================================================================
// Helper: Auto-start MirageCapture ScreenCaptureService on one device
// =========================================================================
static void autoStartCaptureService(const std::string& adb_id, const std::string& display_name) {
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
        "--ez auto_mirror true --es mirror_mode tcp");

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    std::string ui_check = g_adb_manager->adbCommand(adb_id, "shell dumpsys activity top");
    if (ui_check.find("MediaProjectionPermissionActivity") != std::string::npos ||
        ui_check.find("GrantPermissionsActivity") != std::string::npos) {
        // Parse screen resolution to compute tap coordinates
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
        int tap_x = static_cast<int>(screen_w * 0.73f);
        int tap_y = static_cast<int>(screen_h * 0.61f);
        g_adb_manager->adbCommand(adb_id,
            "shell input tap " + std::to_string(tap_x) + " " + std::to_string(tap_y));
        MLOG_INFO("gui", "Auto-tapped MediaProjection dialog on %s (%dx%d -> %d,%d)",
                  display_name.c_str(), screen_w, screen_h, tap_x, tap_y);
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

    // VulkanコンテキストをMultiDeviceReceiverに設定（GPUデコード用）
    auto gui = g_gui;
    if (gui && gui->vulkanContext()) {
        auto* vk_ctx = gui->vulkanContext();
        g_multi_receiver->setVulkanContext(
            vk_ctx->physicalDevice(), vk_ctx->device(),
            vk_ctx->queueFamilies().graphics, vk_ctx->queueFamilies().compute,
            vk_ctx->graphicsQueue(), vk_ctx->computeQueue());
    }

    // MirrorReceiverスロット初期化 (エントリ作成 + デコーダ準備)
    g_multi_receiver->start(mirage::config::getConfig().network.video_base_port);

    // MirageCapture APK直接受信モード
    // 各デバイスに adb forward を設定し、VID0 TCP受信を開始
    // MirageCapture APK がキャプチャ・送信を担う (scrcpy不使用)
    // === Auto-start MirageCapture ScreenCaptureService on all devices ===
    for (const auto& dev : g_adb_manager->getUniqueDevices()) {
        autoStartCaptureService(dev.preferred_adb_id, dev.display_name);
    }

    auto devices = g_adb_manager->getUniqueDevices();
    if (devices.empty()) {
        MLOG_WARN("gui", "Multi-receiver: デバイスが見つかりません");
        g_multi_receiver.reset();
        return false;
    }

    constexpr int REMOTE_PORT = 50100;  // MirageCapture TcpVideoSender
    constexpr int BASE_LOCAL_PORT = 50100;

    int success = 0;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];

        // assigned_tcp_port (devices.json由来) があれば使う、なければ動的割当
        int local_port = (dev.assigned_tcp_port > 0)
            ? dev.assigned_tcp_port
            : BASE_LOCAL_PORT + static_cast<int>(i) * 2;

        const std::string& adb_id = dev.preferred_adb_id;

        // adb forward設定
        std::string fwd_cmd = "forward tcp:" + std::to_string(local_port) +
                              " tcp:" + std::to_string(REMOTE_PORT);
        std::string fwd_result = g_adb_manager->adbCommand(adb_id, fwd_cmd);
        MLOG_INFO("gui", "adb forward: %s -> %s (result: %s)",
                  adb_id.c_str(), fwd_cmd.c_str(), fwd_result.c_str());

        // VID0 TCP受信開始 (既存スロットをTCPモードに切替、なければ新規作成)
        if (g_multi_receiver->restart_as_tcp_vid0(dev.hardware_id, local_port)) {
            MLOG_INFO("gui", "VID0 TCP受信開始: %s port=%d", dev.display_name.c_str(), local_port);
            success++;
        } else {
            MLOG_ERROR("gui", "VID0 TCP受信失敗: %s port=%d", dev.display_name.c_str(), local_port);
        }
    }

    MLOG_INFO("gui", "Multi-receiver: %d/%zu devices VID0 TCP mode",
              success, devices.size());

    // フレームコールバック設定: デコード済みフレームをEventBus経由でGUIに配信
    g_multi_receiver->setFrameCallback([](const std::string& hardware_id, const ::gui::MirrorFrame& frame) {
        mirage::FrameReadyEvent evt;
        evt.device_id = hardware_id;
        evt.rgba_data = frame.rgba.data();
        evt.width = frame.width;
        evt.height = frame.height;
        evt.frame_id = frame.frame_id;
        mirage::bus().publish(evt);
    });

    return success > 0;
}

// DISABLED: Using TCP direct mode via restart_as_tcp instead
// bool initializeTcpReceiver() {
//     g_tcp_video_receiver = std::make_unique<::gui::TcpVideoReceiver>();
//     if (!g_adb_manager) {
//         g_tcp_video_receiver.reset();
//         return false;
//     }
//
//     g_tcp_video_receiver->setDeviceManager(g_adb_manager.get());
//
//     if (g_tcp_video_receiver->start()) {
//         auto device_ids = g_tcp_video_receiver->getDeviceIds();
//         MLOG_INFO("gui", "TCP video receiver: %zu device(s) started", device_ids.size());
//         return true;
//     }
//
//     MLOG_WARN("gui", "TCP video receiver: failed to start (no USB devices or app not running)");
//     g_tcp_video_receiver.reset();
//     return false;
// }

void initializeHybridCommand() {
    g_hybrid_cmd = std::make_unique<::gui::HybridCommandSender>();

    g_hybrid_cmd->set_ack_callback([](const std::string& device_id, uint32_t seq, uint8_t status) {
        MLOG_INFO("usbcmd", "ACK %s seq=%u status=%d", device_id.c_str(), seq, status);
    });

    setupUsbVideoCallback();

    if (g_hybrid_cmd->start()) {
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
        g_hybrid_cmd.reset();
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

void initializeRouting() {
    // Initialize routing even without USB devices - WiFi-only mode is valid
    g_bandwidth_monitor = std::make_unique<::gui::BandwidthMonitor>();
    g_route_controller = std::make_unique<::gui::RouteController>();

    // TCP-only mode determined after USB device registration

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

    // TCP-only mode is set dynamically below based on USB device availability

    // FPS command callback
    g_route_controller->setFpsCommandCallback([](const std::string& device_id, int fps) {
        // TCP-onlyモード: ADB broadcast経由でFPS送信（USB経由はスキップ）
        if (g_route_controller && g_route_controller->isTcpOnlyMode() && g_adb_manager) {
            auto devices = g_adb_manager->getUniqueDevices();
            for (const auto& dev : devices) {
                if (dev.hardware_id == device_id) {
                    // Async: ADB broadcast can take 1-2s over WiFi, must not block GUI/RouteCtrl thread
                    std::string adb_id = dev.preferred_adb_id;
                    // Force X1 to stay at high quality (main) even if adaptive logic mislabels fps.
                    const bool isX1 = (dev.display_name.find("Npad X1") != std::string::npos) ||
                                       (dev.display_name.find("N-one Npad X1") != std::string::npos) ||
                                       (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) ||
                                       (dev.preferred_adb_id.find("93020523431940") != std::string::npos);
                    int send_fps = fps;
                    if (isX1 && send_fps < 60) send_fps = 60;
                    std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS -p com.mirage.capture --ei fps " + std::to_string(send_fps);
                    std::string cmd2;
                    if (isX1) {
                        // Keep max_size at 2000 and request IDR so SPS refresh happens.
                        cmd2 = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_MAXSIZE -p com.mirage.capture --ei max_size 2000";
                    }
                    std::string cmd3;
                    if (isX1) {
                        cmd3 = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_IDR -p com.mirage.capture";
                    }
                    std::thread([adb_id, cmd, cmd2, cmd3, device_id, send_fps]() {
                        if (g_adb_manager) { g_adb_manager->adbCommand(adb_id, cmd); if(!cmd2.empty()) g_adb_manager->adbCommand(adb_id, cmd2); if(!cmd3.empty()) g_adb_manager->adbCommand(adb_id, cmd3); }
                        MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s via ADB broadcast (%s)",
                                  send_fps, device_id.c_str(), adb_id.c_str());
                    }).detach();
                    break;
                }
            }
        } else if (g_hybrid_cmd) {
            // USB AOA経由でFPS送信
            g_hybrid_cmd->send_video_fps(device_id, fps);
            MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s (USB)", fps, device_id.c_str());
        }
    });

    // Route command callback
    g_route_controller->setRouteCommandCallback([](const std::string& device_id,
                                                    ::gui::RouteController::VideoRoute route,
                                                    const std::string& host, int port) {
        if (g_hybrid_cmd) {
            uint8_t mode = (route == ::gui::RouteController::VideoRoute::WIFI) ? 1 : 0;
            g_hybrid_cmd->send_video_route(device_id, mode, host, port);
            MLOG_INFO("RouteCtrl", "Sent Route=%s to %s (%s:%d)",
                    mode ? "WiFi" : "USB", device_id.c_str(), host.c_str(), port);
        }
    });

    // Register USB devices if available
    if (g_hybrid_cmd) {
        auto device_ids = g_hybrid_cmd->get_device_ids();
        int wifi_port = mirage::config::getConfig().network.video_base_port;
        bool first = true;
        for (const auto& id : device_ids) {
            g_route_controller->registerDevice(id, first, wifi_port);
            wifi_port++;
            first = false;
        }
        if (device_ids.empty()) {
            MLOG_INFO("gui", "RouteController: no USB devices, operating in WiFi-only mode (ADB fallback active)");
        } else {
            MLOG_INFO("gui", "RouteController: registered %d USB device(s)", (int)device_ids.size());
        }
    } else {
        MLOG_INFO("gui", "RouteController: no USB command sender, WiFi-only mode");
    }

    // TCP-onlyモード: ADB hardware_idで登録（FrameDispatcherと一致させる）
    if (g_route_controller->isTcpOnlyMode() && g_adb_manager) {
        auto devices = g_adb_manager->getUniqueDevices();
        if (!devices.empty()) {
            // USB登録があればクリア（TCP-onlyではADB hardware_idを使う）
            if (g_hybrid_cmd) {
                auto usb_ids = g_hybrid_cmd->get_device_ids();
                for (const auto& uid : usb_ids) {
                    g_route_controller->unregisterDevice(uid);
                }
            }

            int wifi_port = mirage::config::getConfig().network.video_base_port;
            // Choose main device explicitly: prefer X1 (192.168.0.3:5555 / 93020523431940 / name match)
            auto isX1 = [](const auto& dev){
                if (dev.preferred_adb_id.find("192.168.0.3:5555") != std::string::npos) return true;
                for (const auto& w : dev.wifi_connections) { if (w.find("192.168.0.3:5555") != std::string::npos) return true; }
                for (const auto& u : dev.usb_connections) { if (u.find("93020523431940") != std::string::npos) return true; }
                return (dev.display_name.find("Npad X1") != std::string::npos) || (dev.display_name.find("N-one Npad X1") != std::string::npos);
            };

            bool hasX1 = false;
            for (const auto& dev : devices) { if (isX1(dev)) { hasX1 = true; break; } }

            bool first = true;
            for (const auto& dev : devices) {
                const bool mainFlag = hasX1 ? isX1(dev) : first;
                g_route_controller->registerDevice(dev.hardware_id, mainFlag, wifi_port);
                MLOG_INFO("gui", "RouteController TCP_ONLY: registered %s (%s) main=%d",
                          dev.hardware_id.c_str(), dev.display_name.c_str(), (int)mainFlag);
                wifi_port++;
                first = false;
            }}
    }

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
        autoStartCaptureService(dev.preferred_adb_id, dev.display_name);
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
            if (gui) gui->queueFrame(e.device_id, e.rgba_data, e.width, e.height);
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
    if (g_route_controller) {
        g_route_controller->setMainDevice(device_id);
    }

    // Update FPS: main=60fps, sub=30fps
    if (g_route_controller && g_route_controller->isTcpOnlyMode() && g_adb_manager) {
        // TCP-only: ADB broadcast (async to avoid blocking GUI thread)
        auto devices = g_adb_manager->getUniqueDevices();
        std::string sel_id = device_id;
        std::thread([devices, sel_id]() {
            for (const auto& dev : devices) {
                int target_fps = (dev.hardware_id == sel_id) ? 60 : 30;
                std::string cmd = "shell am broadcast -a com.mirage.capture.ACTION_VIDEO_FPS --ei fps "
                                  + std::to_string(target_fps);
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
    }
}

void initializeGUI(HWND hwnd) {
    MLOG_INFO("gui", "=== initializeGUI called, hwnd=%p ===", (void*)hwnd);
    g_gui = std::make_shared<mirage::gui::GuiApplication>();

    mirage::gui::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;
    config.vsync = true;

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

        // NOTE: AIアクション実行はEventBus経由パイプライン
        // (decideAction → TapCommandEvent/KeyCommandEvent → gui_command購読)
        // action_callback_は後方互換・ログ用のみ
        g_ai_engine->setActionCallback([](int slot, const mirage::ai::AIAction& action) {
            MLOG_DEBUG("ai", "ActionCallback(後方互換): slot=%d type=%d",
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
}
#endif

#ifdef USE_OCR
void initializeOCR() {
    auto gui = g_gui;
    g_ocr_engine = std::make_unique<mirage::ocr::OCREngine>();
    mirage::ocr::OCRConfig ocr_config;
    ocr_config.language = "eng+jpn";

    std::string ocr_error;
    if (g_ocr_engine->initialize(ocr_config, ocr_error)) {
        if (gui) gui->logInfo(u8"OCR engine initialized");
    } else {
        if (gui) gui->logWarning(u8"OCR engine failed: " + ocr_error);
        g_ocr_engine.reset();
    }
}
#endif

} // namespace mirage::gui::init
