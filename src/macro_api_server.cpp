// =============================================================================
// MirageSystem - Macro API Server Implementation
// =============================================================================
// TCP JSON-RPC server bridging Macro Editor → HybridCommandSender / ADB
// =============================================================================

#include <winsock2.h>
#include <set>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "macro_api_server.hpp"
#include "adb_h264_receiver.hpp"
#include "gui/mirage_context.hpp"
#include "mirage_log.hpp"
#include "nlohmann/json.hpp"

#ifdef MIRAGE_OCR_ENABLED
#include "frame_analyzer.hpp"
#endif

#include <algorithm>
#include <sstream>
#include <cstdio>

using json = nlohmann::json;

namespace mirage {

// ---------------------------------------------------------------------------
// ADB command helper (hidden window, captures stdout)
// ---------------------------------------------------------------------------
static std::string run_adb_cmd(const std::string& adb_id, const std::string& cmd) {
    // ctx().adb_manager経由でadbパスを取得 (config.json設定を反映)
    auto& _adb_mgr_ref = mirage::ctx().adb_manager;
    std::string _adb_exe = (_adb_mgr_ref && _adb_mgr_ref.get())
        ? _adb_mgr_ref->getAdbPath() : "adb";
    std::string full_cmd = _adb_exe;
    if (!adb_id.empty()) {
        full_cmd += " -s " + adb_id;
    }
    full_cmd += " " + cmd;

    std::string result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return result;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::string cmd_line = "cmd /c " + full_cmd;

    if (CreateProcessA(nullptr, cmd_line.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite); hWrite = nullptr;
        char buf[4096];
        DWORD n;
        while (ReadFile(hRead, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
            buf[n] = '\0';
            result += buf;
        }
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (hWrite) CloseHandle(hWrite);
    CloseHandle(hRead);

    // Trim trailing whitespace
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
        result.pop_back();
    return result;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------
static std::string escape_json_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
MacroApiServer::MacroApiServer() = default;

MacroApiServer::~MacroApiServer() {
    stop();
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------
bool MacroApiServer::start(int port) {
    if (running_.load()) return true;

    // WinSock init (MirageContext likely already did this, but safe to call twice)
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MLOG_ERROR("macro_api", "WSAStartup failed");
        return false;
    }

    // WSA_FLAG_NO_HANDLE_INHERIT: 子プロセス(adb/ffmpeg)へのハンドル継承を禁止
    // これがゾンビソケットの根本解決 (プロセス終了後にソケットが残る問題を防ぐ)
    server_socket_ = WSASocketA(AF_INET, SOCK_STREAM, IPPROTO_TCP,
                               nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (server_socket_ == INVALID_SOCKET) {
        MLOG_ERROR("macro_api", "WSASocket() failed: %d", WSAGetLastError());
        return false;
    }

    // Allow port reuse + instant release (no TIME_WAIT)
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    LINGER ling{};
    ling.l_onoff  = 1;
    ling.l_linger = 0;
    setsockopt(server_socket_, SOL_SOCKET, SO_LINGER, (const char*)&ling, sizeof(ling));

    // Bind to localhost only (security)
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(server_socket_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        MLOG_ERROR("macro_api", "bind() failed on port %d: %d", port, WSAGetLastError());
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }

    if (listen(server_socket_, MAX_CLIENTS) == SOCKET_ERROR) {
        MLOG_ERROR("macro_api", "listen() failed: %d", WSAGetLastError());
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
        return false;
    }

    port_ = port;
    running_ = true;

    // AdbH264Receiver: ffmpegパイプ方式 (MirrorReceiver/Vulkan不要)
    if (!adb_h264_receiver_) {
        adb_h264_receiver_ = std::make_unique<AdbH264Receiver>();
        adb_h264_receiver_->setAdbPath("C:/Users/jun/.local/bin/platform-tools/adb.exe");
        adb_h264_receiver_->setFfmpegPath("C:/msys64/mingw64/bin/ffmpeg.exe");
        // adb_managerはGUI初期化後に遅延バインド (setDeviceManager後に即座sync)
        adb_h264_receiver_->start();
        MLOG_INFO("macro_api", "AdbH264Receiver started (ffmpeg-pipe mode)");
    }

    server_thread_ = std::thread(&MacroApiServer::server_loop, this);

    MLOG_INFO("macro_api", "MacroApiServer started on 127.0.0.1:%d", port);
    return true;
}

void MacroApiServer::stop() {
    if (!running_.load()) return;
    running_ = false;

    if (adb_h264_receiver_) {
        adb_h264_receiver_->stop();
        adb_h264_receiver_.reset();
    }

    // Close server socket to unblock accept()
    if (server_socket_ != INVALID_SOCKET) {
        closesocket(server_socket_);
        server_socket_ = INVALID_SOCKET;
    }

    if (server_thread_.joinable()) server_thread_.join();

    // Join client threads
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& t : client_threads_) {
        if (t.joinable()) t.join();
    }
    client_threads_.clear();

    MLOG_INFO("macro_api", "MacroApiServer stopped");
}

int MacroApiServer::client_count() const {
    std::lock_guard<std::mutex> lk(clients_mutex_);
    return static_cast<int>(client_threads_.size());
}

// ---------------------------------------------------------------------------
// Server loop - accept connections
// ---------------------------------------------------------------------------
void MacroApiServer::server_loop() {
    while (running_.load()) {
        sockaddr_in client_addr{};
        int addr_len = sizeof(client_addr);
        SOCKET client = accept(server_socket_, (sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            if (running_.load()) {
                MLOG_WARN("macro_api", "accept() failed: %d", WSAGetLastError());
            }
            break; // Server socket was closed for shutdown
        }

        // クライアントソケットも非継承 (adb/ffmpegに継承させない)
        SetHandleInformation((HANDLE)client, HANDLE_FLAG_INHERIT, 0);
        MLOG_INFO("macro_api", "Client connected (fd=%llu)", (unsigned long long)client);

        std::lock_guard<std::mutex> lk(clients_mutex_);
        // Clean up finished threads
        client_threads_.erase(
            std::remove_if(client_threads_.begin(), client_threads_.end(),
                [](std::thread& t) {
                    if (t.joinable()) {
                        // Can't check if finished without native handle; just keep them
                        return false;
                    }
                    return true;
                }),
            client_threads_.end()
        );
        client_threads_.emplace_back(&MacroApiServer::handle_client, this, client);
    }
}

// ---------------------------------------------------------------------------
// Handle a single client - read JSON lines, dispatch, respond
// ---------------------------------------------------------------------------
void MacroApiServer::handle_client(SOCKET client_sock) {
    std::string buffer;
    char recv_buf[4096];

    // Set receive timeout (60s idle disconnect)
    DWORD timeout_ms = 60000;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));

    while (running_.load()) {
        int n = recv(client_sock, recv_buf, sizeof(recv_buf) - 1, 0);
        if (n <= 0) break; // Disconnect or error

        recv_buf[n] = '\0';
        buffer += recv_buf;

        // Process complete lines
        size_t pos;
        while ((pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, pos);
            buffer = buffer.substr(pos + 1);

            // Trim \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string response = dispatch(line);
            response += "\n";

            // Send response
            int total = 0;
            int len = static_cast<int>(response.size());
            while (total < len) {
                int sent = send(client_sock, response.data() + total, len - total, 0);
                if (sent <= 0) goto disconnect;
                total += sent;
            }
        }
    }

disconnect:
    closesocket(client_sock);
    MLOG_INFO("macro_api", "Client disconnected");
}

// ---------------------------------------------------------------------------
// JSON-RPC dispatch
// ---------------------------------------------------------------------------
std::string MacroApiServer::dispatch(const std::string& json_line) {
    try {
        auto req = json::parse(json_line);
        int id = req.value("id", 0);
        std::string method = req.value("method", "");
        auto params = req.value("params", json::object());

        MLOG_INFO("macro_api", "RPC: method=%s id=%d", method.c_str(), id);

        if (method == "ping") {
            return make_result(id, handle_ping());
        }
        if (method == "list_devices") {
            return make_result(id, handle_list_devices());
        }

        // All other methods require device_id
        std::string device_id = params.value("device_id", "");
        if (device_id.empty()) {
            return make_error(id, -2, "missing device_id");
        }

        if (method == "device_info") {
            return make_result(id, handle_device_info(device_id));
        }
        if (method == "tap") {
            return make_result(id, handle_tap(device_id,
                params.value("x", 0), params.value("y", 0)));
        }
        if (method == "swipe") {
            return make_result(id, handle_swipe(device_id,
                params.value("x1", 0), params.value("y1", 0),
                params.value("x2", 0), params.value("y2", 0),
                params.value("duration", 300)));
        }
        if (method == "long_press") {
            return make_result(id, handle_long_press(device_id,
                params.value("x", 0), params.value("y", 0),
                params.value("duration", 1000)));
        }
        if (method == "key") {
            return make_result(id, handle_key(device_id, params.value("keycode", 0)));
        }
        if (method == "text") {
            return make_result(id, handle_text(device_id, params.value("text", "")));
        }
        if (method == "ui_tree") {
            return make_result(id, handle_ui_tree(device_id));
        }
                if (method == "click_id") {
            return make_result(id, handle_click_id(device_id, params.value("resource_id", "")));
        }
        if (method == "click_text") {
            return make_result(id, handle_click_text(device_id, params.value("text", "")));
        }
        if (method == "launch_app") {
            return make_result(id, handle_launch_app(device_id, params.value("package", "")));
        }
        if (method == "force_stop") {
            return make_result(id, handle_force_stop(device_id, params.value("package", "")));
        }
        if (method == "screenshot") {
            return make_result(id, handle_screenshot(device_id));
        }
        if (method == "multi_touch") {
            return make_result(id, handle_multi_touch(device_id,
                params.value("x1", 0), params.value("y1", 0),
                params.value("x2", 0), params.value("y2", 0),
                params.value("duration_ms", 200)));
        }
        if (method == "pinch") {
            return make_result(id, handle_pinch(device_id,
                params.value("direction", std::string("in")),
                params.value("cx", 540), params.value("cy", 960),
                params.value("d_start", 400), params.value("d_end", 100)));
        }

        // OCRメソッド
#ifdef MIRAGE_OCR_ENABLED
        if (method == "ocr_analyze") {
            return make_result(id, handle_ocr_analyze(device_id));
        }
        if (method == "ocr_find_text") {
            return make_result(id, handle_ocr_find_text(device_id, params.value("query", "")));
        }
        if (method == "ocr_has_text") {
            return make_result(id, handle_ocr_has_text(device_id, params.value("query", "")));
        }
        if (method == "ocr_tap_text") {
            return make_result(id, handle_ocr_tap_text(device_id, params.value("query", "")));
        }
#endif

        return make_error(id, -1, "unknown method: " + method);

    } catch (const json::exception& e) {
        return make_error(0, -32700, std::string("JSON parse error: ") + e.what());
    } catch (const std::exception& e) {
        return make_error(0, -32603, std::string("internal error: ") + e.what());
    }
}

// ---------------------------------------------------------------------------
// Response builders
// ---------------------------------------------------------------------------
std::string MacroApiServer::make_result(int id, const std::string& result_json) {
    return "{\"id\":" + std::to_string(id) + ",\"result\":" + result_json + "}";
}

std::string MacroApiServer::make_error(int id, int code, const std::string& message) {
    return "{\"id\":" + std::to_string(id)
         + ",\"error\":{\"code\":" + std::to_string(code)
         + ",\"message\":\"" + escape_json_string(message) + "\"}}";
}

// ---------------------------------------------------------------------------
// resolve_device_id: hardware_id or ADB serial → usable ADB serial
// ---------------------------------------------------------------------------
std::string MacroApiServer::resolve_device_id(const std::string& device_id) {
    auto& mgr = ctx().adb_manager;

    // First try: exact match as preferred_adb_id in unique devices
    auto devices = mgr->getUniqueDevices();
    for (auto& ud : devices) {
        if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id) {
            return ud.preferred_adb_id;
        }
        // Also check USB/WiFi connections
        for (auto& c : ud.usb_connections)  if (c == device_id) return ud.preferred_adb_id;
        for (auto& c : ud.wifi_connections) if (c == device_id) return ud.preferred_adb_id;
    }

    // Fallback: assume it's a direct ADB serial
    return device_id;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

std::string MacroApiServer::handle_ping() {
    auto& hybrid = ctx().hybrid_cmd;
    auto& mgr    = ctx().adb_manager;
    int hybrid_count = (hybrid && hybrid.get()) ? hybrid->device_count() : 0;
    auto unique_devs = (mgr && mgr.get()) ? mgr->getUniqueDevices() : std::vector<::gui::AdbDeviceManager::UniqueDevice>();

    json r;
    r["status"]        = "ok";
    r["version"]       = "1.0.0";
    r["adb_devices"]   = (int)unique_devs.size();
    r["aoa_devices"]   = hybrid_count;
    r["port"]          = port_;
    if (adb_h264_receiver_) {
        r["h264_running"] = adb_h264_receiver_->running();
        r["h264_devices"] = adb_h264_receiver_->device_count();
    } else {
        r["h264_running"] = false;
        r["h264_devices"] = 0;
    }
#ifdef MIRAGE_OCR_ENABLED
    r["ocr_available"] = true;
#else
    r["ocr_available"] = false;
#endif
    return r.dump();
}

std::string MacroApiServer::handle_list_devices() {
    auto& mgr = ctx().adb_manager;
    auto devices = mgr->getUniqueDevices();

    auto& hybrid = ctx().hybrid_cmd;
    auto hybrid_ids = hybrid->get_device_ids();

    // Resolve USB serials to hardware_ids for AOA matching
    std::set<std::string> aoa_hw_ids;
    for (const auto& usb_serial : hybrid_ids) {
        std::string hw = mgr->resolveUsbSerial(usb_serial);
        if (!hw.empty()) aoa_hw_ids.insert(hw);
        aoa_hw_ids.insert(usb_serial); // keep raw in case it matches hardware_id
    }

    json arr = json::array();
    for (auto& ud : devices) {
        json d;
        d["id"]         = ud.hardware_id;
        d["adb_id"]     = ud.preferred_adb_id;
        d["model"]      = ud.model;
        d["name"]       = ud.display_name;
        d["ip"]         = ud.ip_address;
        d["connection"]  = (ud.preferred_type == ::gui::AdbDeviceManager::ConnectionType::USB) ? "usb" : "wifi";

        // Check if device is available via AOA using resolved hardware_ids
        bool has_aoa = (aoa_hw_ids.count(ud.hardware_id) > 0);
        d["aoa"] = has_aoa;
        d["usb_serial"] = ud.usb_serial;
        d["usb_serial"] = ud.usb_serial;
        arr.push_back(d);
    }

    json r;
    r["devices"] = arr;
    return r.dump();
}

std::string MacroApiServer::handle_device_info(const std::string& device_id) {
    std::string adb_id = resolve_device_id(device_id);
    auto& mgr = ctx().adb_manager;
    auto devices = mgr->getUniqueDevices();

    for (auto& ud : devices) {
        if (ud.preferred_adb_id == adb_id || ud.hardware_id == device_id) {
            json d;
            d["id"]       = ud.hardware_id;
            d["adb_id"]   = ud.preferred_adb_id;
            d["model"]    = ud.model;
            d["name"]     = ud.display_name;
            d["ip"]       = ud.ip_address;
            d["screen_w"] = ud.screen_width;
            d["screen_h"] = ud.screen_height;
            d["density"]  = ud.screen_density;
            d["android"]  = ud.android_version;
            d["sdk"]      = ud.sdk_level;
            return d.dump();
        }
    }
    return "{\"error\":\"device not found\"}";
}


// Resolve hardware_id to USB serial for AOA HybridCommandSender lookup
static std::string resolve_to_usb_serial(const std::string& device_id) {
    auto& mgr = mirage::ctx().adb_manager;
    auto devs = mgr->getUniqueDevices();
    for (auto& ud : devs) {
        if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id) {
            return ud.usb_serial.empty() ? device_id : ud.usb_serial;
        }
    }
    return device_id;
}

std::string MacroApiServer::handle_tap(const std::string& device_id, int x, int y) {
    auto& hybrid = ctx().hybrid_cmd;

    // Try AOA/HybridCommandSender first
    {
        std::string usb_key = resolve_to_usb_serial(device_id);
        if (hybrid->is_device_connected(usb_key)) {
            uint32_t seq = hybrid->send_tap(usb_key, x, y);
            if (seq > 0) {
                return "{\"status\":\"ok\",\"via\":\"aoa_hid\",\"seq\":" + std::to_string(seq) + "}";
            }
        }
    }

    // Fallback: ADB
    std::string adb_id = resolve_device_id(device_id);
    std::string cmd = "shell input tap " + std::to_string(x) + " " + std::to_string(y);
    run_adb_cmd(adb_id, cmd);
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_swipe(const std::string& device_id,
    int x1, int y1, int x2, int y2, int duration_ms) {
    auto& hybrid = ctx().hybrid_cmd;

    {
        std::string usb_key = resolve_to_usb_serial(device_id);
        if (hybrid->is_device_connected(usb_key)) {
            uint32_t seq = hybrid->send_swipe(usb_key, x1, y1, x2, y2, duration_ms);
            if (seq > 0) {
                return "{\"status\":\"ok\",\"via\":\"aoa_hid\",\"seq\":" + std::to_string(seq) + "}";
            }
        }
    }

    std::string adb_id = resolve_device_id(device_id);
    std::string cmd = "shell input swipe "
        + std::to_string(x1) + " " + std::to_string(y1) + " "
        + std::to_string(x2) + " " + std::to_string(y2) + " "
        + std::to_string(duration_ms);
    run_adb_cmd(adb_id, cmd);
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_long_press(const std::string& device_id,
    int x, int y, int duration_ms) {
    auto& hybrid = ctx().hybrid_cmd;

    {
        std::string usb_key = resolve_to_usb_serial(device_id);
        if (hybrid->is_device_connected(usb_key)) {
            bool ok = hybrid->send_long_press(usb_key, x, y, 0, 0, duration_ms);
            if (ok) return "{\"status\":\"ok\",\"via\":\"aoa_hid\"}";
        }
    }

    // ADB fallback: swipe to same point with duration = long press
    std::string adb_id = resolve_device_id(device_id);
    std::string cmd = "shell input swipe "
        + std::to_string(x) + " " + std::to_string(y) + " "
        + std::to_string(x) + " " + std::to_string(y) + " "
        + std::to_string(duration_ms);
    run_adb_cmd(adb_id, cmd);
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_multi_touch(const std::string& device_id,
    int x1, int y1, int x2, int y2, int duration_ms) {
    auto& hybrid = ctx().hybrid_cmd;

    // AOA HID 2-finger simultaneous touch via AoaHidTouch::touch_down/move/up
    // ADB cannot do true multi-touch.
    auto* hid = hybrid->get_hid_for_device(device_id);
    if (!hid) {
        return R"({"status":"error","message":"multi_touch requires AOA HID - device not connected"})";
    }

    // Get device screen size for HID coordinate scaling
    int sw = 1080, sh = 1920;
    {
        auto& mgr = ctx().adb_manager;
        auto devices = mgr->getUniqueDevices();
        for (auto& ud : devices) {
            if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id) {
                if (ud.screen_width > 0)  sw = ud.screen_width;
                if (ud.screen_height > 0) sh = ud.screen_height;
                break;
            }
        }
    }

    // Scale to HID coordinate space [0, 32767]
    auto to_hid_x = [&](int x) -> uint16_t { return (uint16_t)((int64_t)x * 32767 / sw); };
    auto to_hid_y = [&](int y) -> uint16_t { return (uint16_t)((int64_t)y * 32767 / sh); };

    hid->touch_down(0, to_hid_x(x1), to_hid_y(y1));
    hid->touch_down(1, to_hid_x(x2), to_hid_y(y2));

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    hid->touch_up(0);
    hid->touch_up(1);

    return R"({"status":"ok","via":"aoa_hid","fingers":2})";
}

std::string MacroApiServer::handle_pinch(const std::string& device_id,
    const std::string& direction, int cx, int cy, int d_start, int d_end) {
    auto& hybrid = ctx().hybrid_cmd;

    // Use existing send_pinch which drives AoaHidTouch internally
    int sw = 1080, sh = 1920;
    {
        auto& mgr = ctx().adb_manager;
        auto devices = mgr->getUniqueDevices();
        for (auto& ud : devices) {
            if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id) {
                if (ud.screen_width > 0)  sw = ud.screen_width;
                if (ud.screen_height > 0) sh = ud.screen_height;
                break;
            }
        }
    }

    int start = (direction == "in") ? d_start : d_end;
    int end   = (direction == "in") ? d_end   : d_start;

    bool ok = hybrid->send_pinch(device_id, cx, cy, start, end, sw, sh, 400);
    if (ok) return R"({"status":"ok","via":"aoa_hid"})";
    return R"({"status":"error","message":"pinch requires AOA HID"})";
}

std::string MacroApiServer::handle_key(const std::string& device_id, int keycode) {
    auto& hybrid = ctx().hybrid_cmd;

    {
        std::string usb_key = resolve_to_usb_serial(device_id);
        if (hybrid->is_device_connected(usb_key)) {
            uint32_t seq = hybrid->send_key(usb_key, keycode);
            if (seq > 0) {
                return "{\"status\":\"ok\",\"via\":\"aoa_hid\",\"seq\":" + std::to_string(seq) + "}";
            }
        }
    }

    std::string adb_id = resolve_device_id(device_id);
    run_adb_cmd(adb_id, "shell input keyevent " + std::to_string(keycode));
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_text(const std::string& device_id, const std::string& text) {
    // Text input always goes through ADB (more reliable for multi-byte chars)
    std::string adb_id = resolve_device_id(device_id);

    // Escape special shell characters
    std::string escaped;
    for (char c : text) {
        if (c == ' ')  { escaped += "%s"; continue; }
        if (c == '&' || c == '|' || c == ';' || c == '(' || c == ')' ||
            c == '<' || c == '>' || c == '"' || c == '\'') {
            escaped += '\\';
        }
        escaped += c;
    }
    run_adb_cmd(adb_id, "shell input text \"" + escaped + "\"");
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_ui_tree(const std::string& device_id) {
    auto* hybrid = ctx().hybrid_cmd.get();
    if (!hybrid) return R"({"status":"error","message":"no sender"})";
    uint32_t seq = hybrid->send_ui_tree_req(device_id);
    if (seq == 0) return R"({"status":"error","message":"ui_tree_req requires AOA connection"})";
    // 応答はCMD_UI_TREE_DATAで非同期受信 (seq返却で追跡可能)
    return "{\"status\":\"ok\",\"seq\":" + std::to_string(seq) + "}";
}

std::string MacroApiServer::handle_click_id(const std::string& device_id,
    const std::string& resource_id) {
    auto& hybrid = ctx().hybrid_cmd;

    // AOA-only feature (requires UI Automator on device)
    if (hybrid->is_device_connected(device_id)) {
        uint32_t seq = hybrid->send_click_id(device_id, resource_id);
        if (seq > 0) {
            return "{\"status\":\"ok\",\"via\":\"hybrid\",\"seq\":" + std::to_string(seq) + "}";
        }
    }

    return "{\"status\":\"error\",\"message\":\"click_id requires AOA connection\"}";
}

std::string MacroApiServer::handle_click_text(const std::string& device_id,
    const std::string& text) {
    auto& hybrid = ctx().hybrid_cmd;

    if (hybrid->is_device_connected(device_id)) {
        uint32_t seq = hybrid->send_click_text(device_id, text);
        if (seq > 0) {
            return "{\"status\":\"ok\",\"via\":\"hybrid\",\"seq\":" + std::to_string(seq) + "}";
        }
    }

    return "{\"status\":\"error\",\"message\":\"click_text requires AOA connection\"}";
}

std::string MacroApiServer::handle_launch_app(const std::string& device_id,
    const std::string& package) {
    std::string adb_id = resolve_device_id(device_id);
    std::string out = run_adb_cmd(adb_id,
        "shell monkey -p " + package + " -c android.intent.category.LAUNCHER 1");
    bool ok = out.find("Events injected") != std::string::npos;
    json r;
    r["status"] = ok ? "ok" : "error";
    r["via"]    = "adb";
    if (!ok) r["output"] = out;
    return r.dump();
}

std::string MacroApiServer::handle_force_stop(const std::string& device_id,
    const std::string& package) {
    std::string adb_id = resolve_device_id(device_id);
    run_adb_cmd(adb_id, "shell am force-stop " + package);
    return "{\"status\":\"ok\",\"via\":\"adb\"}";
}

std::string MacroApiServer::handle_screenshot(const std::string& device_id) {
    // Lazy-bind device manager to AdbH264Receiver
    if (adb_h264_receiver_ && !adb_h264_receiver_->has_manager()) {
        auto* mgr = ctx().adb_manager.get();
        if (mgr) {
            adb_h264_receiver_->setDeviceManager(mgr);
            MLOG_INFO("macro_api", "AdbH264Receiver: late-bound adb_manager");
        }
    }

    // Fast path: AdbH264Receiver (25-40 FPS)
    if (adb_h264_receiver_ && adb_h264_receiver_->running()) {
        std::string hw_id = device_id;
        auto* mgr = ctx().adb_manager.get();
        if (mgr) {
            for (auto& ud : mgr->getUniqueDevices())
                if (ud.hardware_id == device_id || ud.preferred_adb_id == device_id)
                    { hw_id = ud.hardware_id; break; }
        }
        std::vector<uint8_t> jpeg;
        int fw = 0, fh = 0;
        if (adb_h264_receiver_->get_latest_jpeg(hw_id, jpeg, fw, fh) && !jpeg.empty()) {
            static const char b64c[] =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            std::string enc; long fs=(long)jpeg.size();
            enc.reserve(((fs+2)/3)*4);
            for (long i=0; i<fs; i+=3) {
                unsigned int n=(unsigned int)jpeg[i]<<16;
                if(i+1<fs) n|=(unsigned int)jpeg[i+1]<<8;
                if(i+2<fs) n|=(unsigned int)jpeg[i+2];
                enc+=b64c[(n>>18)&0x3F]; enc+=b64c[(n>>12)&0x3F];
                enc+=(i+1<fs)?b64c[(n>>6)&0x3F]:'=';
                enc+=(i+2<fs)?b64c[n&0x3F]:'=';
            }
            json r;
            r["status"]="ok"; r["base64"]=enc;
            r["width"]=fw; r["height"]=fh;
            r["via"]="adb_h264";
            r["fps"]=adb_h264_receiver_->get_fps(hw_id);
            return r.dump();
        }
    }

    std::string adb_id = resolve_device_id(device_id);

    // Capture to device, pull to temp, read and base64-encode
    run_adb_cmd(adb_id, "shell screencap -p /sdcard/mirage_macro_cap.png");

    // Pull to local temp
    char tmp_path[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp_path);
    std::string local_path = std::string(tmp_path) + "mirage_macro_cap.png";
    run_adb_cmd(adb_id, "pull /sdcard/mirage_macro_cap.png \"" + local_path + "\"");
    run_adb_cmd(adb_id, "shell rm /sdcard/mirage_macro_cap.png");

    // Read file and base64 encode
    FILE* f = fopen(local_path.c_str(), "rb");
    if (!f) {
        return "{\"status\":\"error\",\"message\":\"failed to read screenshot\"}";
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::vector<unsigned char> data(file_size);
    fread(data.data(), 1, file_size, f);
    fclose(f);
    DeleteFileA(local_path.c_str());

    // Simple base64 encoding
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((file_size + 2) / 3) * 4);
    for (long i = 0; i < file_size; i += 3) {
        unsigned int n = (data[i] << 16);
        if (i + 1 < file_size) n |= (data[i + 1] << 8);
        if (i + 2 < file_size) n |= data[i + 2];
        encoded += b64[(n >> 18) & 0x3F];
        encoded += b64[(n >> 12) & 0x3F];
        encoded += (i + 1 < file_size) ? b64[(n >> 6) & 0x3F] : '=';
        encoded += (i + 2 < file_size) ? b64[n & 0x3F] : '=';
    }

    // Get screen dimensions
    std::string size_out = run_adb_cmd(adb_id, "shell wm size");
    int width = 1080, height = 1920;
    auto pos = size_out.rfind(':');
    if (pos != std::string::npos) {
        std::string s = size_out.substr(pos + 1);
        // trim
        while (!s.empty() && s.front() == ' ') s.erase(0, 1);
        auto xpos = s.find('x');
        if (xpos != std::string::npos) {
            try {
                width  = std::stoi(s.substr(0, xpos));
                height = std::stoi(s.substr(xpos + 1));
            } catch (...) {}
        }
    }

    json r;
    r["status"] = "ok";
    r["base64"] = encoded;
    r["width"]  = width;
    r["height"] = height;
    return r.dump();
}

// ---------------------------------------------------------------------------
// OCR handlers (Tesseract経由テキスト認識)
// ---------------------------------------------------------------------------
#ifdef MIRAGE_OCR_ENABLED

void MacroApiServer::ensure_ocr_initialized() {
    static bool s_initialized = false;
    if (s_initialized) return;
    if (!analyzer().isInitialized()) {
        MLOG_INFO("macro_api", "OCR: Tesseract初期化中 (eng+jpn)...");
        if (!analyzer().init("eng+jpn")) {
            MLOG_ERROR("macro_api", "OCR: Tesseract初期化失敗");
            return;
        }
        analyzer().startCapture();
        MLOG_INFO("macro_api", "OCR: Tesseract初期化完了、フレームキャプチャ開始");
    }
    s_initialized = true;
}

std::string MacroApiServer::handle_ocr_analyze(const std::string& device_id) {
    ensure_ocr_initialized();
    std::string adb_id = resolve_device_id(device_id);

    auto result = analyzer().analyzeText(adb_id);

    json words_arr = json::array();
    for (auto& w : result.words) {
        json wo;
        wo["text"]       = w.text;
        wo["x1"]         = w.x1;
        wo["y1"]         = w.y1;
        wo["x2"]         = w.x2;
        wo["y2"]         = w.y2;
        wo["confidence"] = w.confidence;
        words_arr.push_back(wo);
    }

    json r;
    r["full_text"]   = result.fullText();
    r["words"]       = words_arr;
    r["word_count"]  = (int)result.words.size();
    r["elapsed_ms"]  = result.elapsed_ms;
    return r.dump();
}

std::string MacroApiServer::handle_ocr_find_text(const std::string& device_id,
    const std::string& query) {
    ensure_ocr_initialized();
    std::string adb_id = resolve_device_id(device_id);

    auto matches = analyzer().findText(adb_id, query);

    json matches_arr = json::array();
    for (auto& m : matches) {
        json mo;
        mo["text"]       = m.text;
        mo["x1"]         = m.x1;
        mo["y1"]         = m.y1;
        mo["x2"]         = m.x2;
        mo["y2"]         = m.y2;
        mo["confidence"] = m.confidence;
        mo["center_x"]   = (m.x1 + m.x2) / 2;
        mo["center_y"]   = (m.y1 + m.y2) / 2;
        matches_arr.push_back(mo);
    }

    json r;
    r["matches"] = matches_arr;
    r["count"]   = (int)matches.size();
    return r.dump();
}

std::string MacroApiServer::handle_ocr_has_text(const std::string& device_id,
    const std::string& query) {
    ensure_ocr_initialized();
    std::string adb_id = resolve_device_id(device_id);

    bool found = analyzer().hasText(adb_id, query);

    json r;
    r["found"] = found;
    return r.dump();
}

std::string MacroApiServer::handle_ocr_tap_text(const std::string& device_id,
    const std::string& query) {
    ensure_ocr_initialized();
    std::string adb_id = resolve_device_id(device_id);

    int cx = 0, cy = 0;
    bool found = analyzer().getTextCenter(adb_id, query, cx, cy);

    if (found) {
        // テキスト位置をタップ
        handle_tap(device_id, cx, cy);
        json r;
        r["found"] = true;
        r["x"]     = cx;
        r["y"]     = cy;
        return r.dump();
    }

    json r;
    r["found"] = false;
    r["error"] = "Text not found";
    return r.dump();
}

#endif // MIRAGE_OCR_ENABLED

} // namespace mirage
