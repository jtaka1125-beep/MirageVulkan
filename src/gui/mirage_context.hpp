// =============================================================================
// MirageSystem v2 - MirageContext
// =============================================================================
// Centralized application context replacing scattered global state.
// All previously-global variables are now members of this class.
// Access via mirage::ctx() singleton.
// =============================================================================
#pragma once

// WinSock must be included before Windows.h
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "gui_application.hpp"
#include "ipc_client.hpp"
#include "mirror_receiver.hpp"
#include "hybrid_receiver.hpp"
#include "hybrid_command_sender.hpp"
#include "usb_video_receiver.hpp"
#include "tcp_video_receiver.hpp"
#include "adb_device_manager.hpp"
#include "multi_device_receiver.hpp"
#include "bandwidth_monitor.hpp"
#include "route_controller.hpp"

#ifdef USE_AI
#include "ai_engine.hpp"
#endif
#ifdef USE_OCR
#include "ocr_engine.hpp"
#endif

#include <memory>
#include <string>
#include <atomic>
#include <array>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace mirage {

// =============================================================================
// Constants
// =============================================================================
namespace constants {
    static constexpr int MAX_SLOTS = 10;
    static constexpr float ASPECT_RATIO = 16.0f / 9.0f;
    static constexpr uint32_t USB_VIDEO_MAGIC = 0x56494430;  // "VID0"
    static constexpr size_t USB_VIDEO_BUFFER_MAX = 128 * 1024;
    static constexpr size_t USB_VIDEO_BUFFER_TRIM = 32 * 1024;
    static constexpr size_t RTP_PACKET_MAX_LEN = 65535;
    static constexpr size_t RTP_PACKET_MIN_LEN = 12;
    static constexpr size_t UDP_RECV_BUFFER_SIZE = 4 * 1024 * 1024;
}

// =============================================================================
// MirageContext - Centralized Application State
// =============================================================================
class MirageContext {
public:
    static constexpr int MAX_SLOTS = constants::MAX_SLOTS;

    MirageContext();
    ~MirageContext();

    // Lifecycle
    void initialize();
    void shutdown();

    // === Core Components ===
    std::shared_ptr<gui::GuiApplication> gui;
    std::unique_ptr<::gui::MirageIpcClient> ipc;
    std::atomic<bool> running{true};
    std::atomic<bool> adb_ready{false};

    // === Video Receivers ===
    // Per-slot receivers (IPC mode)
    std::array<std::unique_ptr<::gui::MirrorReceiver>, MAX_SLOTS> receivers;
    std::array<bool, MAX_SLOTS> slot_active{};

    // Hybrid receivers (USB priority, WiFi fallback)
    std::unique_ptr<::gui::HybridReceiver> hybrid_receiver;
    std::unique_ptr<::gui::HybridCommandSender> hybrid_cmd;

    // Multi-device receiver
    std::unique_ptr<::gui::MultiDeviceReceiver> multi_receiver;

    // USB video receiver
    std::unique_ptr<::gui::UsbVideoReceiver> usb_video_receiver;

    // TCP video receiver (ADB forward mode)
    std::unique_ptr<::gui::TcpVideoReceiver> tcp_video_receiver;

    // Per-device USB video decoders
    std::map<std::string, std::unique_ptr<::gui::MirrorReceiver>> usb_decoders;
    std::mutex usb_decoders_mutex;

    // USB video buffers for parsing
    std::map<std::string, std::vector<uint8_t>> usb_video_buffers;
    std::mutex usb_video_buffers_mutex;

    // === Device Management ===
    std::unique_ptr<::gui::AdbDeviceManager> adb_manager;
    std::string usb_device_id;

    std::set<std::string> registered_usb_devices;
    std::mutex registered_devices_mutex;
    bool main_device_set = false;

    std::map<std::string, bool> multi_devices_added;
    std::mutex multi_devices_mutex;

    std::map<std::string, bool> tcp_devices_added;
    std::mutex tcp_devices_mutex;

    std::string fallback_device_id = "usb_hybrid";
    bool fallback_device_added = false;

    // === Routing & Bandwidth ===
    std::unique_ptr<::gui::BandwidthMonitor> bandwidth_monitor;
    std::unique_ptr<::gui::RouteController> route_controller;
    std::atomic<bool> route_eval_running{false};
    std::thread route_eval_thread;

    // === AI/OCR Engines ===
#ifdef USE_AI
    std::unique_ptr<mirage::ai::AIEngine> ai_engine;
    std::atomic<bool> ai_enabled{true};
#endif

#ifdef USE_OCR
    std::unique_ptr<mirage::ocr::OCREngine> ocr_engine;
    std::atomic<bool> ocr_enabled{false};
#endif

    // No copy/move
    MirageContext(const MirageContext&) = delete;
    MirageContext& operator=(const MirageContext&) = delete;
    MirageContext(MirageContext&&) = delete;
    MirageContext& operator=(MirageContext&&) = delete;
};

// Global context accessor (singleton)
MirageContext& ctx();

} // namespace mirage
