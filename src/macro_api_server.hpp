#pragma once
// =============================================================================
// MirageSystem - Macro API Server
// =============================================================================
// TCP JSON-RPC server for Macro Editor integration.
// Listens on localhost:19840, accepts JSON-line protocol commands,
// and routes them to HybridCommandSender (AOA/ADB) or AdbDeviceManager.
//
// Protocol: Each request is one line of JSON terminated by \n
// Request:  {"id": 1, "method": "tap", "params": {"device_id": "abc", "x": 540, "y": 300}}
// Response: {"id": 1, "result": {"status": "ok"}} or {"id": 1, "error": {"code": -1, "message": "..."}}
// =============================================================================

#include <string>
#include <thread>
#include <atomic>
#include "event_bus.hpp"
#include <mutex>
#include <map>
#include <vector>
#include <vector>
#include "ai/ui_finder.hpp"

// WinSock forward declaration 繧帝∩縺代ヾOCKET蝙九□縺大ｮ夂ｾｩ
#ifndef _WINSOCK2API_
typedef unsigned long long SOCKET;
#endif

namespace mirage {

class MacroApiServer {
public:
    static constexpr int DEFAULT_PORT = 19840;
    static constexpr int MAX_CLIENTS = 4;

    MacroApiServer();
    ~MacroApiServer();

    // 繧ｵ繝ｼ繝舌・襍ｷ蜍輔・蛛懈ｭ｢
    bool start(int port = DEFAULT_PORT);
    void stop();
    bool is_running() const { return running_.load(); }
    int port() const { return port_; }
    int client_count() const;

private:
    // 繧ｵ繝ｼ繝舌・繧ｹ繝ｬ繝・ラ
    void server_loop();
    void handle_client(SOCKET client_sock);

    // JSON-RPC繝・ぅ繧ｹ繝代ャ繝・
    std::string dispatch(const std::string& json_line);

    // 繧ｳ繝槭Φ繝峨ワ繝ｳ繝峨Λ (JSON邨先棡譁・ｭ怜・繧定ｿ斐☆)
    std::string handle_ping();
    std::string handle_list_devices();
    std::string handle_device_info(const std::string& device_id);
    std::string handle_tap(const std::string& device_id, int x, int y);
    std::string handle_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms);
    std::string handle_long_press(const std::string& device_id, int x, int y, int duration_ms);
    std::string handle_multi_touch(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms);
    std::string handle_pinch(const std::string& device_id, const std::string& direction, int cx, int cy, int d_start, int d_end);
    std::string handle_key(const std::string& device_id, int keycode);
    std::string handle_text(const std::string& device_id, const std::string& text);
    std::string handle_ui_tree(const std::string& device_id);
        std::string handle_click_id(const std::string& device_id, const std::string& resource_id);
    std::string handle_click_text(const std::string& device_id, const std::string& text);
    std::string handle_launch_app(const std::string& device_id, const std::string& package);
    std::string handle_force_stop(const std::string& device_id, const std::string& package);
    std::string handle_screenshot(const std::string& device_id);

        // UiFinder - ADB fallback for click_text/click_id
    std::string handle_ui_find(const std::string& device_id, const std::string& query, const std::string& strategy);
    void ensure_ui_finder_initialized(const std::string& adb_id);
    mirage::ai::UiFinder ui_finder_;
    std::string ui_finder_last_adb_id_;

// OCR繝上Φ繝峨Λ
#ifdef MIRAGE_OCR_ENABLED
    std::string handle_ocr_analyze(const std::string& device_id);
    std::string handle_ocr_find_text(const std::string& device_id, const std::string& query);
    std::string handle_ocr_has_text(const std::string& device_id, const std::string& query);
    std::string handle_ocr_tap_text(const std::string& device_id, const std::string& query);
    void ensure_ocr_initialized();
#endif

    // JSON蠢懃ｭ斐・繝ｫ繝代・
    std::string make_result(int id, const std::string& result_json);
    std::string make_error(int id, int code, const std::string& message);

    // 繝・ヰ繧､繧ｹID隗｣豎ｺ: hardware_id 竊・preferred_adb_id
    std::string resolve_device_id(const std::string& device_id);
    std::string resolve_hw_id(const std::string& device_id);

    // Per-device JPEGフレームキャッシュ (FrameReadyEventで更新)
    struct JpegCache {
        std::vector<uint8_t> jpeg;
        int width = 0, height = 0;
        uint64_t frame_id = 0;
    };
    mutable std::mutex jpeg_cache_mutex_;
    std::map<std::string, JpegCache> jpeg_cache_;
    std::atomic<bool> frame_cb_registered_{false};
    mirage::SubscriptionHandle frame_sub_;

    std::atomic<bool> running_{false};
    int port_ = DEFAULT_PORT;
    SOCKET server_socket_ = ~0ULL;  // INVALID_SOCKET
    std::thread server_thread_;

    // 繧ｯ繝ｩ繧､繧｢繝ｳ繝郁ｿｽ霍｡
    mutable std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
};

} // namespace mirage

