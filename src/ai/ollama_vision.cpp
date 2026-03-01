// =============================================================================
// OllamaVision - Layer 3 Popup Detection Implementation
// =============================================================================

#include "ollama_vision.hpp"
#include "../mirage_log.hpp"

// Windows HTTP API
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

// nlohmann/json
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <chrono>
#include <vector>
#include <cstring>
#include <memory>

namespace mirage::ai {

// Base64エンコードテーブル
static const char BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64Encode(const uint8_t* data, size_t len) {
    std::string result;
    result.reserve((len + 2) / 3 * 4);

    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<uint32_t>(data[i + 2]);

        result.push_back(BASE64_TABLE[(n >> 18) & 0x3F]);
        result.push_back(BASE64_TABLE[(n >> 12) & 0x3F]);
        result.push_back((i + 1 < len) ? BASE64_TABLE[(n >> 6) & 0x3F] : '=');
        result.push_back((i + 2 < len) ? BASE64_TABLE[n & 0x3F] : '=');
    }
    return result;
}

// ポップアップ検出プロンプト
static const char* POPUP_DETECTION_PROMPT = R"(Look at this Android screenshot carefully.

Task: Find any popup dialog, modal, alert, or overlay that blocks the main content.

If you find a popup/dialog:
1. Describe what kind of popup it is (ad, permission request, error, notification, etc.)
2. Find the close/dismiss button (usually: X, 閉じる, OK, Cancel, キャンセル, 後で, Skip, etc.)
3. Return the button's approximate position as percentage of screen (x%, y%)

Output ONLY valid JSON in this exact format:
{"found": true, "type": "ad/permission/error/notification/other", "button_text": "X", "x_percent": 85, "y_percent": 15}

If NO popup/dialog found:
{"found": false}

IMPORTANT: Output ONLY the JSON, no explanation.)";

// =============================================================================
// Simple PNG encoder (minimal, no compression)
// =============================================================================

// CRC32テーブル
static uint32_t crc32_table[256];
static bool crc32_table_init = false;

static void init_crc32_table() {
    if (crc32_table_init) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) {
            c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
    crc32_table_init = true;
}

static uint32_t calc_crc32(const uint8_t* data, size_t len) {
    init_crc32_table();
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

static void write_be32(std::vector<uint8_t>& out, uint32_t val) {
    out.push_back((val >> 24) & 0xFF);
    out.push_back((val >> 16) & 0xFF);
    out.push_back((val >> 8) & 0xFF);
    out.push_back(val & 0xFF);
}

static void write_chunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t len) {
    write_be32(out, static_cast<uint32_t>(len));
    size_t crc_start = out.size();
    for (int i = 0; i < 4; i++) out.push_back(type[i]);
    for (size_t i = 0; i < len; i++) out.push_back(data[i]);
    uint32_t crc = calc_crc32(out.data() + crc_start, 4 + len);
    write_be32(out, crc);
}

// RGBA -> PNG (非圧縮、シンプル実装)
static std::vector<uint8_t> encodePngUncompressed(const uint8_t* rgba, int w, int h) {
    std::vector<uint8_t> png;

    // PNG signature
    const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    png.insert(png.end(), sig, sig + 8);

    // IHDR
    uint8_t ihdr[13];
    ihdr[0] = (w >> 24) & 0xFF;
    ihdr[1] = (w >> 16) & 0xFF;
    ihdr[2] = (w >> 8) & 0xFF;
    ihdr[3] = w & 0xFF;
    ihdr[4] = (h >> 24) & 0xFF;
    ihdr[5] = (h >> 16) & 0xFF;
    ihdr[6] = (h >> 8) & 0xFF;
    ihdr[7] = h & 0xFF;
    ihdr[8] = 8;   // bit depth
    ihdr[9] = 6;   // color type (RGBA)
    ihdr[10] = 0;  // compression
    ihdr[11] = 0;  // filter
    ihdr[12] = 0;  // interlace
    write_chunk(png, "IHDR", ihdr, 13);

    // IDAT (非圧縮 zlib)
    // 各行: filter byte (0) + RGBA * width
    size_t raw_row_size = 1 + w * 4;
    size_t raw_size = raw_row_size * h;

    std::vector<uint8_t> raw_data;
    raw_data.reserve(raw_size);
    for (int y = 0; y < h; y++) {
        raw_data.push_back(0); // filter: none
        for (int x = 0; x < w * 4; x++) {
            raw_data.push_back(rgba[y * w * 4 + x]);
        }
    }

    // 非圧縮 zlib (store blocks)
    std::vector<uint8_t> zlib_data;
    zlib_data.push_back(0x78); // CMF
    zlib_data.push_back(0x01); // FLG

    // Store block (max 65535 bytes per block)
    size_t pos = 0;
    while (pos < raw_data.size()) {
        size_t chunk_size = std::min<size_t>(raw_data.size() - pos, 65535);
        bool is_last = (pos + chunk_size >= raw_data.size());

        zlib_data.push_back(is_last ? 0x01 : 0x00); // BFINAL + BTYPE
        uint16_t len = static_cast<uint16_t>(chunk_size);
        uint16_t nlen = ~len;
        zlib_data.push_back(len & 0xFF);
        zlib_data.push_back((len >> 8) & 0xFF);
        zlib_data.push_back(nlen & 0xFF);
        zlib_data.push_back((nlen >> 8) & 0xFF);

        for (size_t i = 0; i < chunk_size; i++) {
            zlib_data.push_back(raw_data[pos + i]);
        }
        pos += chunk_size;
    }

    // Adler-32
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_data.size(); i++) {
        s1 = (s1 + raw_data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    uint32_t adler = (s2 << 16) | s1;
    zlib_data.push_back((adler >> 24) & 0xFF);
    zlib_data.push_back((adler >> 16) & 0xFF);
    zlib_data.push_back((adler >> 8) & 0xFF);
    zlib_data.push_back(adler & 0xFF);

    write_chunk(png, "IDAT", zlib_data.data(), zlib_data.size());

    // IEND
    write_chunk(png, "IEND", nullptr, 0);

    return png;
}

// =============================================================================
// WinHTTP helpers
// =============================================================================

struct WinHttpHandleDeleter {
    void operator()(HINTERNET h) { if (h) WinHttpCloseHandle(h); }
};
using WinHttpHandle = std::unique_ptr<void, WinHttpHandleDeleter>;

// =============================================================================
// Constructor
// =============================================================================

OllamaVision::OllamaVision(const OllamaVisionConfig& config)
    : config_(config) {
    MLOG_INFO("ollama", "OllamaVision初期化: %s:%d model=%s",
              config_.host.c_str(), config_.port, config_.model.c_str());
}

// =============================================================================
// Public Methods
// =============================================================================

OllamaVisionResult OllamaVision::detectPopup(const uint8_t* rgba, int width, int height) {
    auto start = std::chrono::steady_clock::now();

    // RGBAをPNG Base64に変換
    std::string image_b64 = encodeRgbaToPngBase64(rgba, width, height);
    if (image_b64.empty()) {
        OllamaVisionResult result;
        result.error = "PNG encoding failed";
        return result;
    }

    // Ollama API呼び出し
    auto response = callOllamaApi(POPUP_DETECTION_PROMPT, image_b64);

    auto end = std::chrono::steady_clock::now();
    int elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    if (!response) {
        OllamaVisionResult result;
        result.error = "Ollama API call failed";
        result.elapsed_ms = elapsed_ms;
        return result;
    }

    return parseResponse(*response, elapsed_ms);
}

OllamaVisionResult OllamaVision::detectPopupFromFile(const std::string& png_path) {
    auto start = std::chrono::steady_clock::now();

    std::string image_b64 = encodeFileToBase64(png_path);
    if (image_b64.empty()) {
        OllamaVisionResult result;
        result.error = "File read/encode failed: " + png_path;
        return result;
    }

    auto response = callOllamaApi(POPUP_DETECTION_PROMPT, image_b64);

    auto end = std::chrono::steady_clock::now();
    int elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());

    if (!response) {
        OllamaVisionResult result;
        result.error = "Ollama API call failed";
        result.elapsed_ms = elapsed_ms;
        return result;
    }

    return parseResponse(*response, elapsed_ms);
}

bool OllamaVision::isAvailable() {
    // 簡易チェック: /api/tags にGETリクエスト
    WinHttpHandle hSession(WinHttpOpen(L"MirageVulkan/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) return false;

    std::wstring host_w(config_.host.begin(), config_.host.end());
    WinHttpHandle hConnect(WinHttpConnect(hSession.get(), host_w.c_str(),
                                           static_cast<INTERNET_PORT>(config_.port), 0));
    if (!hConnect) return false;

    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect.get(), L"GET", L"/api/tags",
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!hRequest) return false;

    DWORD timeout = 5000; // 5秒
    WinHttpSetOption(hRequest.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    if (!WinHttpSendRequest(hRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        return false;
    }

    return WinHttpReceiveResponse(hRequest.get(), nullptr) != FALSE;
}

// =============================================================================
// Private Methods
// =============================================================================

std::string OllamaVision::encodeRgbaToPngBase64(const uint8_t* rgba, int width, int height) {
    auto png = encodePngUncompressed(rgba, width, height);
    if (png.empty()) {
        MLOG_ERROR("ollama", "PNG encoding failed: %dx%d", width, height);
        return "";
    }

    std::string b64 = base64Encode(png.data(), png.size());
    MLOG_DEBUG("ollama", "PNG encoded: %dx%d -> %zu bytes -> %zu chars base64",
               width, height, png.size(), b64.size());
    return b64;
}

std::string OllamaVision::encodeFileToBase64(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        MLOG_ERROR("ollama", "Cannot open file: %s", path.c_str());
        return "";
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    file.close();

    return base64Encode(data.data(), data.size());
}

std::optional<std::string> OllamaVision::callOllamaApi(const std::string& prompt,
                                                        const std::string& image_base64) {
    // WinHTTP session
    WinHttpHandle hSession(WinHttpOpen(L"MirageVulkan/1.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) {
        MLOG_ERROR("ollama", "WinHttpOpen failed");
        return std::nullopt;
    }

    std::wstring host_w(config_.host.begin(), config_.host.end());
    WinHttpHandle hConnect(WinHttpConnect(hSession.get(), host_w.c_str(),
                                           static_cast<INTERNET_PORT>(config_.port), 0));
    if (!hConnect) {
        MLOG_ERROR("ollama", "WinHttpConnect failed");
        return std::nullopt;
    }

    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect.get(), L"POST", L"/api/generate",
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!hRequest) {
        MLOG_ERROR("ollama", "WinHttpOpenRequest failed");
        return std::nullopt;
    }

    // タイムアウト設定
    DWORD timeout_ms = config_.timeout_sec * 1000;
    WinHttpSetOption(hRequest.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(hRequest.get(), WINHTTP_OPTION_SEND_TIMEOUT, &timeout_ms, sizeof(timeout_ms));

    // リクエストJSON構築
    nlohmann::json req_json;
    req_json["model"] = config_.model;
    req_json["prompt"] = prompt;
    req_json["images"] = nlohmann::json::array({image_base64});
    req_json["stream"] = false;
    req_json["options"] = {
        {"temperature", config_.temperature},
        {"num_predict", config_.max_tokens}
    };

    std::string body = req_json.dump();

    MLOG_INFO("ollama", "API呼び出し開始: model=%s, body_size=%zu",
              config_.model.c_str(), body.size());

    // ヘッダー設定
    const wchar_t* headers = L"Content-Type: application/json\r\n";

    // リクエスト送信
    if (!WinHttpSendRequest(hRequest.get(), headers, -1,
                            const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        MLOG_ERROR("ollama", "WinHttpSendRequest failed: %lu", GetLastError());
        return std::nullopt;
    }

    if (!WinHttpReceiveResponse(hRequest.get(), nullptr)) {
        MLOG_ERROR("ollama", "WinHttpReceiveResponse failed: %lu", GetLastError());
        return std::nullopt;
    }

    // ステータスコード確認
    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(hRequest.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX);

    // レスポンスボディ読み込み
    std::string response_body;
    DWORD bytes_available = 0;
    while (WinHttpQueryDataAvailable(hRequest.get(), &bytes_available) && bytes_available > 0) {
        std::vector<char> buffer(bytes_available + 1);
        DWORD bytes_read = 0;
        if (WinHttpReadData(hRequest.get(), buffer.data(), bytes_available, &bytes_read)) {
            response_body.append(buffer.data(), bytes_read);
        }
    }

    if (status_code != 200) {
        MLOG_ERROR("ollama", "API error: status=%lu body=%s",
                   status_code, response_body.substr(0, 200).c_str());
        return std::nullopt;
    }

    // レスポンスJSONパース
    try {
        auto resp_json = nlohmann::json::parse(response_body);
        std::string response = resp_json.value("response", "");
        MLOG_INFO("ollama", "API応答: %s", response.substr(0, 100).c_str());
        return response;
    } catch (const std::exception& e) {
        MLOG_ERROR("ollama", "JSON parse error: %s", e.what());
        return std::nullopt;
    }
}

OllamaVisionResult OllamaVision::parseResponse(const std::string& response, int elapsed_ms) {
    OllamaVisionResult result;
    result.raw_response = response;
    result.elapsed_ms = elapsed_ms;

    // JSON部分を抽出 (LLMが余計なテキストを付けることがある)
    size_t json_start = response.find('{');
    size_t json_end = response.rfind('}');

    if (json_start == std::string::npos || json_end == std::string::npos || json_end <= json_start) {
        MLOG_WARN("ollama", "JSONが見つからない: %s", response.substr(0, 100).c_str());
        return result;
    }

    std::string json_str = response.substr(json_start, json_end - json_start + 1);

    try {
        auto j = nlohmann::json::parse(json_str);
        result.found = j.value("found", false);

        if (result.found) {
            result.type = j.value("type", "");
            result.button_text = j.value("button_text", "");
            result.x_percent = j.value("x_percent", 0);
            result.y_percent = j.value("y_percent", 0);

            MLOG_INFO("ollama", "ポップアップ検出: type=%s button=%s pos=(%d%%, %d%%) elapsed=%dms",
                      result.type.c_str(), result.button_text.c_str(),
                      result.x_percent, result.y_percent, elapsed_ms);
        } else {
            MLOG_INFO("ollama", "ポップアップなし elapsed=%dms", elapsed_ms);
        }
    } catch (const std::exception& e) {
        MLOG_WARN("ollama", "JSON parse失敗: %s", e.what());
    }

    return result;
}

} // namespace mirage::ai
