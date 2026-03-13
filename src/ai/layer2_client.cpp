// =============================================================================
// Layer2Client — Gemini並列投票サブプロセス呼び出し実装
// =============================================================================
#include "layer2_client.hpp"

#include <windows.h>
#include <algorithm>
#include <sstream>
#include <cstring>
#include <cctype>

// stb_image_write for JPEG encoding
// STB_IMAGE_WRITE_IMPLEMENTATION は gui_frame_capture_impl.cpp で定義済み
#include "../stb_image_write.h"

#include "../mirage_log.hpp"

namespace mirage::ai {

// =============================================================================
// Base64エンコード
// =============================================================================
static const char kB64Table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// JSONエスケープ（" と \ をエスケープ）
static std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

std::string Layer2Client::base64Encode(const uint8_t* data, int size) {
    std::string out;
    out.reserve(((size + 2) / 3) * 4);
    for (int i = 0; i < size; i += 3) {
        uint32_t b = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) b |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < size) b |= static_cast<uint32_t>(data[i + 2]);
        out += kB64Table[(b >> 18) & 0x3F];
        out += kB64Table[(b >> 12) & 0x3F];
        out += (i + 1 < size) ? kB64Table[(b >> 6) & 0x3F] : '=';
        out += (i + 2 < size) ? kB64Table[b & 0x3F] : '=';
    }
    return out;
}

// stb_image_write コールバック
void Layer2Client::jpegWriteCallback(void* ctx, void* data, int size) {
    auto* buf = reinterpret_cast<std::vector<uint8_t>*>(ctx);
    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    buf->insert(buf->end(), bytes, bytes + size);
}

// =============================================================================
// Python サブプロセス呼び出し（CreateProcess + stdin/stdout パイプ）
// Layer1コンテキストをJSON形式で送信
// =============================================================================
Layer2Result Layer2Client::callScript(const std::vector<uint8_t>& jpeg_data,
                                       const Layer1Context& l1_ctx) {
    Layer2Result result;

    // base64エンコード
    std::string b64 = base64Encode(jpeg_data.data(), static_cast<int>(jpeg_data.size()));
    // Layer1コンテキスト付きJSON構築
    std::ostringstream json_ss;
    json_ss << "{\"image\":\"" << b64 << "\",\"layer1\":{"
            << "\"template_name\":\"" << escapeJson(l1_ctx.template_name) << "\","
            << "\"match_x\":" << l1_ctx.match_x << ","
            << "\"match_y\":" << l1_ctx.match_y << ","
            << "\"score\":" << l1_ctx.score << ","
            << "\"no_match_frames\":" << l1_ctx.no_match_frames << ","
            << "\"same_match_frames\":" << l1_ctx.same_match_frames << ","
            << "\"tags\":\"" << escapeJson(l1_ctx.tags) << "\""
            << "}}";
    std::string input_data = json_ss.str() + "\n";

    MLOG_DEBUG("ai.layer2", "callScript: layer1 context: template=%s score=%.2f no_match=%d",
               l1_ctx.template_name.c_str(), l1_ctx.score, l1_ctx.no_match_frames);

    // パイプ作成
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hStdinRead = nullptr, hStdinWrite = nullptr;
    HANDLE hStdoutRead = nullptr, hStdoutWrite = nullptr;

    if (!CreatePipe(&hStdinRead, &hStdinWrite, &sa, 0)) {
        result.error = "CreatePipe(stdin) failed";
        return result;
    }
    if (!CreatePipe(&hStdoutRead, &hStdoutWrite, &sa, 0)) {
        CloseHandle(hStdinRead); CloseHandle(hStdinWrite);
        result.error = "CreatePipe(stdout) failed";
        return result;
    }
    // 親側のハンドルは継承しない
    SetHandleInformation(hStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdoutRead, HANDLE_FLAG_INHERIT, 0);

    // コマンドライン構築
    std::string cmd = "\"" + python_exe_ + "\" \"" + script_path_ + "\" vision --stdin";
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput  = hStdinRead;
    si.hStdOutput = hStdoutWrite;
    si.hStdError  = hStdoutWrite;

    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessA(
        nullptr, cmd_buf.data(),
        nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr,
        &si, &pi);

    CloseHandle(hStdinRead);
    CloseHandle(hStdoutWrite);

    if (!ok) {
        CloseHandle(hStdinWrite);
        CloseHandle(hStdoutRead);
        result.error = "CreateProcess failed (" + std::to_string(GetLastError()) + ")";
        return result;
    }

    // JSONデータをstdinに書き込む
    DWORD written = 0;
    WriteFile(hStdinWrite, input_data.data(), static_cast<DWORD>(input_data.size()), &written, nullptr);
    CloseHandle(hStdinWrite);  // EOF通知

    // stdoutを読む（タイムアウト30秒）
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    char buf[4096];
    DWORD read_bytes = 0;
    while (true) {
        if (std::chrono::steady_clock::now() >= deadline) {
            result.error = "Timeout (30s)";
            TerminateProcess(pi.hProcess, 1);
            break;
        }

        DWORD avail = 0;
        if (!PeekNamedPipe(hStdoutRead, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail == 0) {
            DWORD exit_code = STILL_ACTIVE;
            GetExitCodeProcess(pi.hProcess, &exit_code);
            if (exit_code != STILL_ACTIVE) {
                while (ReadFile(hStdoutRead, buf, sizeof(buf), &read_bytes, nullptr) && read_bytes > 0)
                    output.append(buf, read_bytes);
                break;
            }
            Sleep(10);
            continue;
        }
        DWORD to_read = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(buf)));
        if (!ReadFile(hStdoutRead, buf, to_read, &read_bytes, nullptr)) break;
        output.append(buf, read_bytes);
    }

    CloseHandle(hStdoutRead);
    WaitForSingleObject(pi.hProcess, 1000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (!result.error.empty()) return result;

    // --- JSON簡易パース ---
    result.raw_json = output;
    MLOG_DEBUG("ai.layer2", "callScript output: %.200s", output.c_str());

    // "result": "YES"/"NO"
    auto findVal = [&](const std::string& key) -> std::string {
        auto pos = output.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = output.find(":", pos);
        if (pos == std::string::npos) return "";
        size_t vs = output.find_first_not_of(" \t\r\n", pos + 1);
        if (vs == std::string::npos) return "";
        if (output[vs] == '"') {
            size_t ve = output.find('"', vs + 1);
            if (ve == std::string::npos) return "";
            return output.substr(vs + 1, ve - vs - 1);
        }
        size_t ve = output.find_first_of(",}\r\n", vs);
        if (ve == std::string::npos) ve = output.size();
        return output.substr(vs, ve - vs);
    };

    std::string res_val = findVal("result");
    std::string conf_val = findVal("confidence");

    for (auto& c : res_val) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    result.popup_detected = (res_val == "YES");

    if (!conf_val.empty()) {
        try { result.confidence = std::stof(conf_val); } catch (...) {}
    }

    result.click_x = -1;
    result.click_y = -1;
    result.valid = true;
    return result;
}

// =============================================================================
// コンストラクタ / デストラクタ
// =============================================================================
Layer2Client::Layer2Client(const std::string& python_exe, const std::string& script_path)
    : python_exe_(python_exe), script_path_(script_path) {
    MLOG_INFO("ai.layer2", "Layer2Client初期化: python=%s script=%s",
              python_exe_.c_str(), script_path_.c_str());
}

Layer2Client::~Layer2Client() {
    std::vector<std::shared_ptr<TaskState>> tasks;
    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        for (auto& kv : tasks_) tasks.push_back(kv.second);
    }
    for (auto& ts : tasks) {
        if (ts->worker.joinable()) ts->worker.join();
    }
}

// =============================================================================
// 非同期起動（後方互換性: Layer1コンテキストなし）
// =============================================================================
bool Layer2Client::launchAsync(const std::string& device_id,
                                const uint8_t* rgba, int width, int height,
                                std::chrono::steady_clock::time_point trigger_time) {
    // 空のLayer1コンテキストで呼び出し
    return launchAsyncWithContext(device_id, rgba, width, height, Layer1Context{}, trigger_time);
}

// =============================================================================
// 非同期起動（Layer1コンテキスト付き）
// =============================================================================
bool Layer2Client::launchAsyncWithContext(const std::string& device_id,
                                           const uint8_t* rgba, int width, int height,
                                           const Layer1Context& l1_ctx,
                                           std::chrono::steady_clock::time_point /*trigger_time*/) {
    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);

        auto it = tasks_.find(device_id);
        if (it != tasks_.end()) {
            auto& ts = it->second;
            if (ts->running.load()) {
                MLOG_DEBUG("ai.layer2", "launchAsyncWithContext: 既に実行中 device=%s", device_id.c_str());
                return false;
            }
            if (ts->worker.joinable()) ts->worker.join();

            // 冷却チェック
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - ts->last_done_time).count();
            if (ts->last_done_time.time_since_epoch().count() > 0 && elapsed < COOLDOWN_MS) {
                MLOG_DEBUG("ai.layer2", "launchAsyncWithContext: 冷却中 device=%s elapsed=%lldms",
                           device_id.c_str(), static_cast<long long>(elapsed));
                return false;
            }
        }
    }

    // RGBA → JPEG エンコード（メインスレッドで実行）
    std::vector<uint8_t> jpeg_buf;
    jpeg_buf.reserve(width * height / 4);
    const int quality = 75;
    int ok = stbi_write_jpg_to_func(jpegWriteCallback, &jpeg_buf, width, height, 4, rgba, quality);
    if (!ok || jpeg_buf.empty()) {
        MLOG_WARN("ai.layer2", "RGBA→JPEG変換失敗: device=%s %dx%d", device_id.c_str(), width, height);
        return false;
    }

    MLOG_DEBUG("ai.layer2", "JPEG生成: %dx%d -> %zu bytes layer1=%s",
               width, height, jpeg_buf.size(), l1_ctx.template_name.c_str());

    auto ts = std::make_shared<TaskState>();
    ts->running.store(true);
    ts->done.store(false);

    ts->worker = std::thread([this, ts, device_id, jpeg_buf = std::move(jpeg_buf), l1_ctx]() mutable {
        MLOG_INFO("ai.layer2", "Gemini vision呼び出し開始: device=%s jpeg=%zu bytes layer1=%s",
                  device_id.c_str(), jpeg_buf.size(), l1_ctx.template_name.c_str());
        auto r = callScript(jpeg_buf, l1_ctx);
        {
            std::lock_guard<std::mutex> rl(ts->result_mutex);
            ts->result = r;
        }
        ts->last_done_time = std::chrono::steady_clock::now();
        ts->running.store(false);
        ts->done.store(true);
        MLOG_INFO("ai.layer2", "Gemini vision完了: device=%s popup=%s conf=%.2f error=%s",
                  device_id.c_str(),
                  r.popup_detected ? "YES" : "NO",
                  r.confidence,
                  r.error.c_str());
    });

    {
        std::lock_guard<std::mutex> lk(tasks_mutex_);
        tasks_[device_id] = ts;
    }
    return true;
}

// =============================================================================
// ポーリング
// =============================================================================
Layer2Result Layer2Client::pollResult(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = tasks_.find(device_id);
    if (it == tasks_.end()) return {};
    auto& ts = it->second;
    if (!ts->done.load()) return {};

    Layer2Result r;
    {
        std::lock_guard<std::mutex> rl(ts->result_mutex);
        r = ts->result;
    }
    r.valid = true;
    ts->done.store(false);  // 結果は1回だけ返す
    return r;
}

// =============================================================================
// 状態確認
// =============================================================================
bool Layer2Client::isRunning(const std::string& device_id) const {
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = tasks_.find(device_id);
    if (it == tasks_.end()) return false;
    return it->second->running.load();
}

bool Layer2Client::isOnCooldown(const std::string& device_id,
                                 std::chrono::steady_clock::time_point now) const {
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = tasks_.find(device_id);
    if (it == tasks_.end()) return false;
    auto& ts = it->second;
    if (ts->running.load()) return true;
    if (ts->last_done_time.time_since_epoch().count() == 0) return false;
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - ts->last_done_time).count();
    return elapsed < COOLDOWN_MS;
}

void Layer2Client::cancel(const std::string& device_id) {
    std::lock_guard<std::mutex> lk(tasks_mutex_);
    auto it = tasks_.find(device_id);
    if (it != tasks_.end()) {
        it->second->done.store(false);
    }
}

} // namespace mirage::ai
