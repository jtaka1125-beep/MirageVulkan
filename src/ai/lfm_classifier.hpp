#pragma once
// =============================================================================
// LfmClassifier - Layer 2.5 UI Text Classification via LFM (Ollama)
// =============================================================================
// LFM2-350M / LFM2.5-1.2B-Instruct を使用してOCRテキストからアクションを分類。
// Layer 1 (template/OCR) で判断できない場合のLayer 2.5フォールバック。
// llava:7b (Layer 3) より20倍高速 (20ms vs 700ms)。
// =============================================================================

#include <string>
#include <optional>
#include <chrono>

namespace mirage::ai {

// 分類アクション
enum class UiAction {
    Close,    // ダイアログを閉じる
    Tap,      // タップ/確認
    Ignore,   // 無視
    Unknown   // 判断不能 → Layer 3 (llava) へフォールバック
};

// 分類結果
struct LfmResult {
    UiAction action = UiAction::Unknown;
    std::string action_str;    // "close" / "tap" / "ignore" / "unknown"
    std::string raw_response;
    int elapsed_ms = 0;
    std::string error;
    bool success = false;
};

// 設定
struct LfmConfig {
    std::string host = "127.0.0.1";
    int port = 11434;
    // 軽量分類: LFM2-350M  / 複雑判断: LFM2.5-1.2B-Instruct
    std::string model_fast  = "hf.co/LiquidAI/LFM2-350M-GGUF:Q4_K_M";
    std::string model_smart = "hf.co/LiquidAI/LFM2.5-1.2B-Instruct-GGUF:Q4_K_M";
    int timeout_sec = 10;
    float temperature = 0.0f;  // 分類なので決定論的
    int max_tokens = 4;        // "close"/"tap"/"ignore"/"unknown" の最大長
};

class LfmClassifier {
public:
    explicit LfmClassifier(const LfmConfig& config = {});
    ~LfmClassifier() = default;

    // OCRテキストからアクションを分類 (fast model = LFM2-350M)
    LfmResult classify(const std::string& ocr_text,
                       const std::string& task_context = "");

    // 複雑なケース用 (smart model = LFM2.5-1.2B-Instruct)
    LfmResult classifySmart(const std::string& ocr_text,
                            const std::string& task_context = "");

    // Ollamaサーバー接続確認
    bool isAvailable();

    void setConfig(const LfmConfig& cfg) { config_ = cfg; }
    const LfmConfig& config() const { return config_; }

private:
    LfmConfig config_;

    LfmResult classifyWithModel(const std::string& model,
                                const std::string& ocr_text,
                                const std::string& task_context);

    std::optional<std::string> callOllama(const std::string& model,
                                          const std::string& prompt);

    static UiAction parseAction(const std::string& response);
    static std::string buildPrompt(const std::string& ocr_text,
                                   const std::string& task_context);
};

} // namespace mirage::ai
