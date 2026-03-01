#pragma once
// =============================================================================
// OllamaVision - Layer 3 Popup Detection via Local LLM
// =============================================================================
// llava:7b (Ollama) を使用して未知のポップアップを検出。
// Layer 1 (テンプレート) / Layer 2 (OCR) で検出できない場合のフォールバック。
// 検出成功時はテンプレートとして自動保存 → 次回からLayer 1で高速検出。
// =============================================================================

#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace mirage::ai {

// 検出結果
struct OllamaVisionResult {
    bool found = false;              // ポップアップ検出したか
    std::string type;                // ad/permission/error/notification/other
    std::string button_text;         // 閉じるボタンのテキスト (X, OK, 閉じる等)
    int x_percent = 0;               // ボタンX座標 (画面幅に対する%)
    int y_percent = 0;               // ボタンY座標 (画面高さに対する%)
    std::string raw_response;        // LLMの生レスポンス
    int elapsed_ms = 0;              // 処理時間(ms)
    std::string error;               // エラーメッセージ (あれば)
};

// 設定
struct OllamaVisionConfig {
    std::string host = "127.0.0.1";
    int port = 11434;
    std::string model = "llava:7b";
    int timeout_sec = 120;           // LLM応答タイムアウト
    float temperature = 0.1f;        // 低いほど決定論的
    int max_tokens = 200;            // 応答トークン上限
};

class OllamaVision {
public:
    explicit OllamaVision(const OllamaVisionConfig& config = {});
    ~OllamaVision() = default;

    // RGBAフレームからポップアップを検出
    // @param rgba: RGBA画像データ (width * height * 4 bytes)
    // @param width, height: 画像サイズ
    // @return 検出結果
    OllamaVisionResult detectPopup(const uint8_t* rgba, int width, int height);

    // PNGファイルからポップアップを検出
    OllamaVisionResult detectPopupFromFile(const std::string& png_path);

    // Ollamaサーバーの接続確認
    bool isAvailable();

    // 設定変更
    void setConfig(const OllamaVisionConfig& config) { config_ = config; }
    const OllamaVisionConfig& config() const { return config_; }

private:
    OllamaVisionConfig config_;

    // RGBAをBase64 PNG文字列に変換
    std::string encodeRgbaToPngBase64(const uint8_t* rgba, int width, int height);

    // ファイルをBase64に変換
    std::string encodeFileToBase64(const std::string& path);

    // Ollama APIを呼び出し
    std::optional<std::string> callOllamaApi(const std::string& prompt,
                                              const std::string& image_base64);

    // LLMレスポンスをパース
    OllamaVisionResult parseResponse(const std::string& response, int elapsed_ms);
};

} // namespace mirage::ai
