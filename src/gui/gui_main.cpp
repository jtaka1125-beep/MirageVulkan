// =============================================================================
// MirageSystem v2 GUI - Main Entry Point (Refactored)
// =============================================================================
// Modular GUI system with separated concerns:
//   - gui_state.hpp/cpp    : Global state management
//   - gui_command.hpp/cpp  : Device command functions
//   - gui_window.hpp/cpp   : Window procedure
//   - gui_threads.hpp/cpp  : Background threads
//   - gui_device_control.hpp/cpp : AOA/ADB control (NEW)
// =============================================================================

#pragma comment(lib, "ws2_32.lib")

#include "mirage_log.hpp"
#include "mirage_config.hpp"
#include "event_bus.hpp"
#include "frame_dispatcher.hpp"
#include "gui_state.hpp"
#include "gui_command.hpp"
#include "gui_window.hpp"
#include "gui_threads.hpp"
#include "gui_device_control.hpp"
#include "config_loader.hpp"
#include "bandwidth_monitor.hpp"
#include "winusb_checker.hpp"
#include "route_controller.hpp"
#include "vid0_parser.hpp"

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <thread>
#include <cstdio>

using namespace mirage::gui::state;
using namespace mirage::gui::command;
using namespace mirage::gui::window;
using namespace mirage::gui::threads;
using namespace mirage::gui::device_control;

// =============================================================================
// USB Video Callback Setup
// =============================================================================

static void setupUsbVideoCallback() {
    if (!g_hybrid_cmd) return;

    g_hybrid_cmd->set_video_callback([](const std::string& device_id, const uint8_t* data, size_t len) {
        // Accumulate data in per-device buffer and parse VID0 packets
        mirage::video::ParseResult parse_result;

        {
            std::lock_guard<std::mutex> buf_lock(g_usb_video_buffers_mutex);
            auto& buffer = g_usb_video_buffers[device_id];
            buffer.insert(buffer.end(), data, data + len);
            parse_result = mirage::video::parseVid0Packets(buffer);
        }

        auto& rtp_packets = parse_result.rtp_packets;
        
        if (rtp_packets.empty()) return;
        
        std::lock_guard<std::mutex> lock(g_usb_decoders_mutex);

        auto it = g_usb_decoders.find(device_id);
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
                auto [inserted_it, success] = g_usb_decoders.emplace(device_id, std::move(decoder));
                it = inserted_it;
                MLOG_INFO("gui", "Created USB decoder for device: %s", device_id.c_str());
            } else {
                MLOG_ERROR("gui", "Failed to create decoder for device: %s", device_id.c_str());
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

static bool initializeMultiReceiver() {
    g_multi_receiver = std::make_unique<::gui::MultiDeviceReceiver>();
    if (!g_adb_manager) {
        g_multi_receiver.reset();
        return false;
    }

    g_multi_receiver->setDeviceManager(g_adb_manager.get());

    if (g_multi_receiver->start(mirage::config::getConfig().network.video_base_port)) {
        auto device_ids = g_multi_receiver->getDeviceIds();
        MLOG_INFO("gui", "マルチ受信: %zu 台で開始", device_ids.size());

        auto devices = g_adb_manager->getUniqueDevices();
        for (const auto& dev : devices) {
            MLOG_INFO("gui", "  - %s -> ポート %d", dev.display_name.c_str(), dev.assigned_port);
        }

        // Auto-start screen capture
        std::string host_ip = mirage::config::getConfig().network.pc_ip;
        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, mirage::config::getConfig().network.video_base_port);
        MLOG_INFO("gui", "スクリーンキャプチャ開始: %d/%zu 台", success, devices.size());
        return true;
    }

    g_multi_receiver.reset();
    return false;
}

static bool initializeTcpReceiver() {
    g_tcp_video_receiver = std::make_unique<::gui::TcpVideoReceiver>();
    if (!g_adb_manager) {
        g_tcp_video_receiver.reset();
        return false;
    }

    g_tcp_video_receiver->setDeviceManager(g_adb_manager.get());

    if (g_tcp_video_receiver->start()) {
        auto device_ids = g_tcp_video_receiver->getDeviceIds();
        MLOG_INFO("gui", "TCP video receiver: %zu device(s) started", device_ids.size());
        return true;
    }

    MLOG_WARN("gui", "TCP video receiver: failed to start (no USB devices or app not running)");
    g_tcp_video_receiver.reset();
    return false;
}

static void initializeHybridCommand() {
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

static void initializeRouting() {
    // Initialize routing even without USB devices - WiFi-only mode is valid
    g_bandwidth_monitor = std::make_unique<::gui::BandwidthMonitor>();
    g_route_controller = std::make_unique<::gui::RouteController>();

    // FPS command callback
    g_route_controller->setFpsCommandCallback([](const std::string& device_id, int fps) {
        if (g_hybrid_cmd) {
            g_hybrid_cmd->send_video_fps(device_id, fps);
            MLOG_INFO("RouteCtrl", "Sent FPS=%d to %s", fps, device_id.c_str());
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

    // Start route evaluation thread
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

static void initializeGUI(HWND hwnd) {
    MLOG_INFO("gui", "=== initializeGUI called, hwnd=%p ===", (void*)hwnd);
    g_gui = std::make_shared<mirage::gui::GuiApplication>();

    mirage::gui::GuiConfig config;
    config.window_width = 1920;
    config.window_height = 1080;
    config.vsync = true;

    if (!g_gui->initialize(hwnd, config)) {
        MLOG_INFO("gui", "GUI初期化失敗");
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

    g_gui->setDeviceSelectCallback([](const std::string& device_id) {
        auto gui = g_gui;
        if (gui) gui->logInfo(u8"選択: " + device_id);

        // Update RouteController main device
        if (g_route_controller) {
            g_route_controller->setMainDevice(device_id);
        }

        // Update FPS: main=60fps, sub=30fps
        if (g_hybrid_cmd) {
            // Build USB serial -> hardware_id map from ADB manager
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
                MLOG_INFO("gui", "FPS update: %s -> %d fps (%s)",
                        uid.c_str(), target_fps,
                        (hw_id == device_id) ? "MAIN" : "sub");
            }
        }
    });

    g_gui->setLearningDataCallback([](const mirage::gui::LearningClickData& data) {
        std::string msg = u8"学習: (" + std::to_string(data.click_x) + "," +
                          std::to_string(data.click_y) + u8") シーン=" + data.scene_name;
        auto gui = g_gui;
        if (gui) gui->logDebug(msg);
    });

    g_gui->setStartMirroringCallback([]() {
        auto gui = g_gui;
        if (!g_adb_manager) {
            if (gui) gui->logError(u8"ADBマネージャ未初期化");
            return;
        }

        if (gui) gui->logInfo(u8"全デバイスでミラーリング開始中...");
        std::string host_ip = mirage::config::getConfig().network.pc_ip;
        int success = g_adb_manager->startScreenCaptureOnAll(host_ip, mirage::config::getConfig().network.video_base_port);
        auto devices = g_adb_manager->getUniqueDevices();
        if (gui) gui->logInfo(u8"ミラーリング開始: " + std::to_string(success) + "/" +
                              std::to_string(devices.size()) + u8" デバイス");
    });

    g_gui->logInfo(u8"MirageSystem v2 GUI 起動");

    // === Event Bus Subscriptions (app-lifetime) ===
    auto sub_connect = mirage::bus().subscribe<mirage::DeviceConnectedEvent>(
        [](const mirage::DeviceConnectedEvent& e) {
            auto gui = g_gui;
            if (gui) {
                gui->addDevice(e.device_id, e.display_name);
                gui->logInfo(u8"デバイス接続(event): " + e.display_name);
            }
        });
    sub_connect.release();

    auto sub_frame = mirage::bus().subscribe<mirage::FrameReadyEvent>(
        [](const mirage::FrameReadyEvent& e) {
            auto gui = g_gui;
            if (gui) {
                gui->queueFrame(e.device_id, e.rgba_data, e.width, e.height);
            }
        });
    sub_frame.release();

    auto sub_disconnect = mirage::bus().subscribe<mirage::DeviceDisconnectedEvent>(
        [](const mirage::DeviceDisconnectedEvent& e) {
            auto gui = g_gui;
            if (gui) {
                gui->logWarning(u8"デバイス切断: " + e.device_id);
            }
        });
    sub_disconnect.release();

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
    g_gui->logInfo(u8"Ctrl+L: 学習モード切替");

    if (g_hybrid_cmd) {
        int usb_count = g_hybrid_cmd->device_count();
        if (usb_count > 0) {
            g_gui->logInfo(u8"\u64cd\u4f5c: USB AOA x" + std::to_string(usb_count) + u8" (\u5168\u30c7\u30d0\u30a4\u30b9\u540c\u6642\u5236\u5fa1)");
        } else {
            g_gui->logInfo(u8"\u64cd\u4f5c: ADB\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af\u30e2\u30fc\u30c9 (USB AOA\u672a\u63a5\u7d9a)");
        }
    } else {
        g_gui->logInfo(u8"\u64cd\u4f5c: ADB\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af\u30e2\u30fc\u30c9");
    }
}

#ifdef USE_AI
static void initializeAI() {
    auto gui = g_gui;
    g_ai_engine = std::make_unique<mirage::ai::AIEngine>();
    mirage::ai::AIConfig ai_config;
    ai_config.templates_dir = "templates";
    ai_config.default_threshold = 0.80f;
    ai_config.enable_multi_scale = true;

    std::string ai_error;
    // Pass VulkanContext for GPU compute backend
    mirage::vk::VulkanContext* vk_ctx = gui ? gui->vulkanContext() : nullptr;
    if (g_ai_engine->initialize(ai_config, ai_error, vk_ctx)) {
        if (gui) gui->logInfo(u8"AIエンジン 初期化完了");
        if (g_ai_engine->loadTemplatesFromDir(ai_config.templates_dir, ai_error)) {
            auto stats = g_ai_engine->getStats();
            if (gui) gui->logInfo(u8"AI テンプレート " + std::to_string(stats.templates_loaded) + u8"件 読込");
        }

        g_ai_engine->setActionCallback([](int slot, const mirage::ai::AIAction& action) {
            std::string device_id = getDeviceIdFromSlot(slot);
            if (action.type == mirage::ai::AIAction::Type::TAP) {
                sendTapCommand(device_id, action.x, action.y);
            }
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
        if (gui) gui->logWarning(u8"AIエンジン 失敗: " + ai_error);
        g_ai_engine.reset();
    }
}
#endif

#ifdef USE_OCR
static void initializeOCR() {
    auto gui = g_gui;
    g_ocr_engine = std::make_unique<mirage::ocr::OCREngine>();
    mirage::ocr::OCRConfig ocr_config;
    ocr_config.language = "eng+jpn";

    std::string ocr_error;
    if (g_ocr_engine->initialize(ocr_config, ocr_error)) {
        if (gui) gui->logInfo(u8"OCRエンジン 初期化完了");
    } else {
        if (gui) gui->logWarning(u8"OCRエンジン 失敗: " + ocr_error);
        g_ocr_engine.reset();
    }
}
#endif

// =============================================================================
// Main Entry Point
// =============================================================================

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {

    // Initialize Winsock
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    // Initialize configuration and open log file
    auto& sys_config = mirage::config::getSystemConfig();
    mirage::config::applyEnvironmentOverrides(sys_config);
    std::string log_path = sys_config.log_directory + "\\" + sys_config.log_filename;
    mirage::log::openLogFile(log_path.c_str());

    try {

    // Initialize state
    initializeState();

    // Start ADB detection
    std::thread adbThread(adbDetectionThread);
    MLOG_INFO("gui", "ADB検出待機中...");
    while (!g_adb_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    adbThread.join();
    MLOG_INFO("gui", "ADB検出完了");

    // WinUSB driver check - diagnose driver issues before USB init
    int winusb_needs_count = 0;
    {
        auto usb_devices = mirage::WinUsbChecker::checkDevices();
        for (const auto& dev : usb_devices) {
            if (dev.needs_winusb) winusb_needs_count++;
        }
        if (winusb_needs_count > 0) {
            MLOG_WARN("gui", "WinUSB driver missing on %d device(s)! USB AOA will not work.", winusb_needs_count);
            MLOG_WARN("gui", "Commands will use ADB fallback. Video will use WiFi.");
            auto summary = mirage::WinUsbChecker::getDiagnosticSummary();
            MLOG_WARN("gui", "Driver status: %s", summary.c_str());
        } else if (!usb_devices.empty()) {
            MLOG_INFO("gui", "WinUSB driver check: all %d device(s) OK", (int)usb_devices.size());
        }
    }


    // Start WiFi ADB watchdog (joins at shutdown via g_running=false)
    std::thread watchdogThread(wifiAdbWatchdogThread);
    // watchdog joins at cleanup (no more detach)

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;  // Removed CS_HREDRAW|CS_VREDRAW - Vulkan handles all rendering
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MirageSystemV2";
    
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"Failed to register window class", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    
    // Create window - adapt to actual screen resolution
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    MLOG_INFO("main", "Screen resolution: %dx%d", screenW, screenH);

    // Target 16:9 aspect ratio, fit within screen (leave room for taskbar)
    int clientW, clientH;
    if (screenW >= 1920 && screenH >= 1080) {
        // Full HD or larger: use 1920x1080
        clientW = 1920;
        clientH = 1080;
    } else {
        // Smaller screen: fit within available space
        int availH = screenH - 40;  // Reserve for taskbar
        clientW = screenW;
        clientH = static_cast<int>(clientW / (16.0f / 9.0f));
        if (clientH > availH) {
            clientH = availH;
            clientW = static_cast<int>(clientH * (16.0f / 9.0f));
        }
    }
    MLOG_INFO("main", "Window client size: %dx%d", clientW, clientH);

    RECT rect = {0, 0, clientW, clientH};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    // Center on screen
    int winW = rect.right - rect.left;
    int winH = rect.bottom - rect.top;
    int posX = (screenW - winW) / 2;
    int posY = (screenH - winH) / 2;
    if (posX < 0) posX = 0;
    if (posY < 0) posY = 0;

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"MirageSystem v2 - Control Panel",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        winW, winH,
        nullptr, nullptr, hInstance, nullptr
    );
    
    if (!hwnd) {
        MessageBoxW(nullptr, L"Failed to create window", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Initialize components
    bool multi_recv_active = initializeMultiReceiver();
    initializeHybridCommand();
    bool tcp_recv_active = initializeTcpReceiver();
    MLOG_INFO("gui", "TCP receiver: %s", tcp_recv_active ? "active" : "inactive");
    initializeRouting();

    // Initialize IPC (fallback)
    g_ipc = std::make_unique<::gui::MirageIpcClient>();
    g_ipc->connect();

    // Show window BEFORE Vulkan init (AMD driver requires visible window for surface)
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Initialize GUI
    initializeGUI(hwnd);

    // Show deferred WinUSB warning in GUI (g_gui is now initialized)
    if (winusb_needs_count > 0 && g_gui) {
        g_gui->logWarning(u8"USB\u76f4\u63a5\u5236\u5fa1: WinUSB\u30c9\u30e9\u30a4\u30d0\u304c\u672a\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb ("
            + std::to_string(winusb_needs_count) + u8"\u53f0) - [\u30c9\u30e9\u30a4\u30d0\u8a2d\u5b9a]\u30dc\u30bf\u30f3\u3067\u30a4\u30f3\u30b9\u30c8\u30fc\u30eb\u3057\u3066\u304f\u3060\u3055\u3044");
        g_gui->logInfo(u8"ADB\u30d5\u30a9\u30fc\u30eb\u30d0\u30c3\u30af\u3067\u64cd\u4f5c\u4e2d (\u30bf\u30c3\u30d7/\u30b9\u30ef\u30a4\u30d7\u306fADB\u7d4c\u7531)");
    }

#ifdef USE_AI
    initializeAI();
#endif

#ifdef USE_OCR
    initializeOCR();
#endif

    // Start device update thread
    std::thread updateThread(deviceUpdateThread);
    UpdateWindow(hwnd);
    
    // Main loop
    MSG msg = {};
    while (g_running && g_gui->isRunning()) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }

        if (!g_running) break;

        g_gui->processPendingFrames();
        g_gui->beginFrame();
        
        // Render device control panel (NEW - AOA/ADB buttons)
        renderDeviceControlPanel();
        
        g_gui->render();
        g_gui->endFrame();
    }
    
    // Cleanup
    g_running = false;
    if (updateThread.joinable()) {
        updateThread.join();
    }
    
    // Join watchdog thread
    if (watchdogThread.joinable()) {
        watchdogThread.join();
        MLOG_INFO("gui", "Watchdog thread joined");
    }

    // Join route evaluation thread
    g_route_eval_running.store(false);
    if (g_route_eval_thread.joinable()) {
        g_route_eval_thread.join();
        MLOG_INFO("gui", "Route eval thread joined");
    }

    cleanupState();

    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    

    } catch (const std::exception& e) {
        MLOG_ERROR("gui", "FATAL unhandled exception: %s", e.what());
        MessageBoxA(nullptr, e.what(), "MirageSystem Fatal Error", MB_OK | MB_ICONERROR);
    } catch (...) {
        MLOG_ERROR("gui", "FATAL unknown exception");
        MessageBoxA(nullptr, "Unknown fatal error", "MirageSystem Fatal Error", MB_OK | MB_ICONERROR);
    }

    WSACleanup();
    
    mirage::log::closeLogFile();
    return 0;
}

#ifdef _CONSOLE
int main(int argc, char* argv[]) {
    // Initialize structured logging
    mirage::log::openLogFile("mirage_gui.log");
#ifdef _DEBUG
    mirage::log::setLogLevel(mirage::log::Level::Debug);
#endif
    MLOG_INFO("main", "MirageSystem v2.2 starting...");
    return WinMain(GetModuleHandle(nullptr), nullptr, GetCommandLineA(), SW_SHOWNORMAL);
}
#endif
