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
#include <mutex>
#include <vector>

// WinSock forward declaration を避け、SOCKET型だけ定義
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

    // サーバー起動・停止
    bool start(int port = DEFAULT_PORT);
    void stop();
    bool is_running() const { return running_.load(); }
    int port() const { return port_; }
    int client_count() const;

private:
    // サーバースレッド
    void server_loop();
    void handle_client(SOCKET client_sock);

    // JSON-RPCディスパッチ
    std::string dispatch(const std::string& json_line);

    // コマンドハンドラ (JSON結果文字列を返す)
    std::string handle_ping();
    std::string handle_list_devices();
    std::string handle_device_info(const std::string& device_id);
    std::string handle_tap(const std::string& device_id, int x, int y);
    std::string handle_swipe(const std::string& device_id, int x1, int y1, int x2, int y2, int duration_ms);
    std::string handle_long_press(const std::string& device_id, int x, int y, int duration_ms);
    std::string handle_key(const std::string& device_id, int keycode);
    std::string handle_text(const std::string& device_id, const std::string& text);
    std::string handle_click_id(const std::string& device_id, const std::string& resource_id);
    std::string handle_click_text(const std::string& device_id, const std::string& text);
    std::string handle_launch_app(const std::string& device_id, const std::string& package);
    std::string handle_force_stop(const std::string& device_id, const std::string& package);
    std::string handle_screenshot(const std::string& device_id);

    // OCRハンドラ
#ifdef MIRAGE_OCR_ENABLED
    std::string handle_ocr_analyze(const std::string& device_id);
    std::string handle_ocr_find_text(const std::string& device_id, const std::string& query);
    std::string handle_ocr_has_text(const std::string& device_id, const std::string& query);
    std::string handle_ocr_tap_text(const std::string& device_id, const std::string& query);
    void ensure_ocr_initialized();
#endif

    // JSON応答ヘルパー
    std::string make_result(int id, const std::string& result_json);
    std::string make_error(int id, int code, const std::string& message);

    // デバイスID解決: hardware_id → preferred_adb_id
    std::string resolve_device_id(const std::string& device_id);

    std::atomic<bool> running_{false};
    int port_ = DEFAULT_PORT;
    SOCKET server_socket_ = ~0ULL;  // INVALID_SOCKET
    std::thread server_thread_;

    // クライアント追跡
    mutable std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
};

} // namespace mirage
