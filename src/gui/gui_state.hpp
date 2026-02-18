// =============================================================================
// MirageSystem v2 GUI - Global State (Backward Compatibility Wrapper)
// =============================================================================
// This header now wraps MirageContext for backward compatibility.
// Existing code using `using namespace mirage::gui::state;` continues to work.
// New code should use mirage::ctx() directly.
// =============================================================================
#pragma once

// Structured logging (replaces fprintf-based macros)
#include "mirage_log.hpp"

// Legacy macro compatibility
#define MIRAGE_LOG_DEBUG(fmt, ...) MLOG_DEBUG("gui", fmt, ##__VA_ARGS__)
#define MIRAGE_LOG_ERROR(fmt, ...) MLOG_ERROR("gui", fmt, ##__VA_ARGS__)
#define MIRAGE_LOG_WARN(fmt, ...)  MLOG_WARN("gui", fmt, ##__VA_ARGS__)
#define MIRAGE_LOG_INFO(fmt, ...)  MLOG_INFO("gui", fmt, ##__VA_ARGS__)

#include "mirage_context.hpp"

namespace mirage::gui::state {

// Constants
static constexpr int MAX_SLOTS = mirage::constants::MAX_SLOTS;
static constexpr float ASPECT_RATIO = mirage::constants::ASPECT_RATIO;
static constexpr uint32_t USB_VIDEO_MAGIC = mirage::constants::USB_VIDEO_MAGIC;
static constexpr size_t USB_VIDEO_BUFFER_MAX = mirage::constants::USB_VIDEO_BUFFER_MAX;
static constexpr size_t USB_VIDEO_BUFFER_TRIM = mirage::constants::USB_VIDEO_BUFFER_TRIM;
static constexpr size_t RTP_PACKET_MAX_LEN = mirage::constants::RTP_PACKET_MAX_LEN;
static constexpr size_t RTP_PACKET_MIN_LEN = mirage::constants::RTP_PACKET_MIN_LEN;
static constexpr size_t UDP_RECV_BUFFER_SIZE = mirage::constants::UDP_RECV_BUFFER_SIZE;

// Backward compatible references via ctx() singleton
inline auto& g_gui                    = mirage::ctx().gui;
inline auto& g_ipc                    = mirage::ctx().ipc;
inline auto& g_running                = mirage::ctx().running;
inline auto& g_adb_ready              = mirage::ctx().adb_ready;
inline auto& g_receivers              = mirage::ctx().receivers;
inline auto& g_slot_active            = mirage::ctx().slot_active;
inline auto& g_hybrid_receiver        = mirage::ctx().hybrid_receiver;
inline auto& g_hybrid_cmd             = mirage::ctx().hybrid_cmd;
inline auto& g_multi_receiver         = mirage::ctx().multi_receiver;
inline auto& g_usb_video_receiver     = mirage::ctx().usb_video_receiver;
inline auto& g_tcp_video_receiver     = mirage::ctx().tcp_video_receiver;
inline auto& g_usb_decoders           = mirage::ctx().usb_decoders;
inline auto& g_usb_decoders_mutex     = mirage::ctx().usb_decoders_mutex;
inline auto& g_usb_video_buffers      = mirage::ctx().usb_video_buffers;
inline auto& g_usb_video_buffers_mutex = mirage::ctx().usb_video_buffers_mutex;
inline auto& g_adb_manager            = mirage::ctx().adb_manager;
inline auto& g_usb_device_id          = mirage::ctx().usb_device_id;
inline auto& g_registered_usb_devices = mirage::ctx().registered_usb_devices;
inline auto& g_main_device_set        = mirage::ctx().main_device_set;
inline auto& g_multi_devices_added    = mirage::ctx().multi_devices_added;
inline auto& g_tcp_devices_added      = mirage::ctx().tcp_devices_added;
inline auto& g_fallback_device_id     = mirage::ctx().fallback_device_id;
inline auto& g_fallback_device_added  = mirage::ctx().fallback_device_added;
inline auto& g_bandwidth_monitor      = mirage::ctx().bandwidth_monitor;
inline auto& g_route_controller       = mirage::ctx().route_controller;
inline auto& g_route_eval_running     = mirage::ctx().route_eval_running;
inline auto& g_route_eval_thread      = mirage::ctx().route_eval_thread;
inline auto& g_macro_api_server       = mirage::ctx().macro_api_server;

#ifdef USE_AI
inline auto& g_ai_engine              = mirage::ctx().ai_engine;
inline auto& g_ai_enabled             = mirage::ctx().ai_enabled;
#endif

#ifdef USE_OCR
inline auto& g_ocr_engine             = mirage::ctx().ocr_engine;
inline auto& g_ocr_enabled            = mirage::ctx().ocr_enabled;
#endif

// Backward compatible functions
inline void initializeState() { mirage::ctx().initialize(); }
inline void cleanupState()    { mirage::ctx().shutdown(); }

} // namespace mirage::gui::state
