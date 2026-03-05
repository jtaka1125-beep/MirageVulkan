// =============================================================================
// LfmClassifier - Implementation
// =============================================================================
#include "lfm_classifier.hpp"
#include "../mirage_log.hpp"

#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <nlohmann/json.hpp>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cctype>

namespace mirage::ai {

// WinHTTPハンドルRAIIラッパー（ollama_vision.cppと同じパターン）
struct WinHttpHandleLfm {
    HINTERNET h = nullptr;
    explicit WinHttpHandleLfm(HINTERNET h_) : h(h_) {}
    ~WinHttpHandleLfm() { if (h) WinHttpCloseHandle(h); }
    HINTERNET get() const { return h; }
};

// =============================================================================
// プロンプト構築
// =============================================================================
static const char* SYSTEM_PROMPT =
    "You are a mobile UI automation agent. "
    "Analyze the UI text and return exactly one word.\n"
    "Allowed outputs: close tap ignore unknown\n\n";

std::string LfmClassifier::buildPrompt(const std::string& ocr_text,
                                        const std::string& task_context) {
    std::string prompt = SYSTEM_PROMPT;
    prompt += "UI text: \"" + ocr_text + "\"\n";
    if (!task_context.empty()) {
        prompt += "Task context: " + task_context + "\n";
    }
    prompt += "Action?";
    return prompt;
}

// =============================================================================
// アクション文字列のパース
// =============================================================================
UiAction LfmClassifier::parseAction(const std::string& response) {
    // 小文字化して先頭トークンを取る
    std::string lower = response;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    // 先頭の空白除去
    size_t start = lower.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return UiAction::Unknown;
    lower = lower.substr(start);

    if (lower.find("close") == 0) return UiAction::Close;
    if (lower.find("tap")   == 0) return UiAction::Tap;
    if (lower.find("ignore")== 0) return UiAction::Ignore;
    return UiAction::Unknown;
}

// =============================================================================
// Ollama HTTP呼び出し（テキスト専用 - imagesフィールドなし）
// =============================================================================
std::optional<std::string> LfmClassifier::callOllama(const std::string& model,
                                                      const std::string& prompt) {
    WinHttpHandleLfm hSession(WinHttpOpen(L"MirageVulkan/1.0",
                                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                          WINHTTP_NO_PROXY_NAME,
                                          WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession.h) return std::nullopt;

    std::wstring host_w(config_.host.begin(), config_.host.end());
    WinHttpHandleLfm hConnect(WinHttpConnect(hSession.get(), host_w.c_str(),
                                              static_cast<INTERNET_PORT>(config_.port), 0));
    if (!hConnect.h) return std::nullopt;

    WinHttpHandleLfm hRequest(WinHttpOpenRequest(hConnect.get(), L"POST", L"/api/generate",
                                                  nullptr, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, 0));
    if (!hRequest.h) return std::nullopt;

    // タイムアウト
    DWORD timeout_ms = config_.timeout_sec * 1000;
    WinHttpSetOption(hRequest.get(), WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout_ms, sizeof(timeout_ms));
    WinHttpSetOption(hRequest.get(), WINHTTP_OPTION_SEND_TIMEOUT,    &timeout_ms, sizeof(timeout_ms));

    // JSONボディ（imagesフィールドなし = テキスト専用）
    nlohmann::json req;
    req["model"]  = model;
    req["prompt"] = prompt;
    req["stream"] = false;
    req["options"] = {
        {"temperature", config_.temperature},
        {"num_predict", config_.max_tokens}
    };
    std::string body = req.dump();

    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (!WinHttpSendRequest(hRequest.get(), headers, -1,
                            const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        MLOG_ERROR("lfm", "WinHttpSendRequest failed: %lu", GetLastError());
        return std::nullopt;
    }
    if (!WinHttpReceiveResponse(hRequest.get(), nullptr)) {
        MLOG_ERROR("lfm", "WinHttpReceiveResponse failed");
        return std::nullopt;
    }

    // レスポンス読み込み
    std::string body_resp;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest.get(), &avail) && avail > 0) {
        std::vector<char> buf(avail + 1, 0);
        DWORD read = 0;
        if (WinHttpReadData(hRequest.get(), buf.data(), avail, &read))
            body_resp.append(buf.data(), read);
    }

    try {
        auto j = nlohmann::json::parse(body_resp);
        if (j.contains("response"))
            return j["response"].get<std::string>();
    } catch (...) {}

    return std::nullopt;
}

// =============================================================================
// 分類メイン
// =============================================================================
LfmClassifier::LfmClassifier(const LfmConfig& config) : config_(config) {
    MLOG_INFO("lfm", "LfmClassifier初期化: fast=%s smart=%s",
              config_.model_fast.c_str(), config_.model_smart.c_str());
}

LfmResult LfmClassifier::classifyWithModel(const std::string& model,
                                            const std::string& ocr_text,
                                            const std::string& task_context) {
    LfmResult result;
    auto t0 = std::chrono::steady_clock::now();

    auto prompt = buildPrompt(ocr_text, task_context);
    auto resp   = callOllama(model, prompt);

    auto t1 = std::chrono::steady_clock::now();
    result.elapsed_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    if (!resp) {
        result.error = "Ollama呼び出し失敗";
        MLOG_WARN("lfm", "classify失敗 model=%s text='%s'",
                  model.c_str(), ocr_text.c_str());
        return result;
    }

    result.raw_response = *resp;
    result.action       = parseAction(*resp);
    result.action_str   = *resp;
    result.success      = true;

    MLOG_INFO("lfm", "classify完了: model=%s text='%.30s' -> %s (%dms)",
              model.c_str(), ocr_text.c_str(), resp->c_str(), result.elapsed_ms);
    return result;
}

LfmResult LfmClassifier::classify(const std::string& ocr_text,
                                   const std::string& task_context) {
    return classifyWithModel(config_.model_fast, ocr_text, task_context);
}

LfmResult LfmClassifier::classifySmart(const std::string& ocr_text,
                                        const std::string& task_context) {
    return classifyWithModel(config_.model_smart, ocr_text, task_context);
}

bool LfmClassifier::isAvailable() {
    auto resp = callOllama(config_.model_fast, "ping");
    return resp.has_value();
}

} // namespace mirage::ai
