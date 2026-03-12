#pragma once
// =============================================================================
// Layer2Client — Gemini並列投票によるポップアップ検出
// gemini_router.py を stdin/stdout でサブプロセス呼び出し
// =============================================================================

#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <map>
#include <vector>
#include <chrono>
#include <cstdint>

namespace mirage::ai {

struct Layer2Result {
    bool valid         = false;  // 結果取得済み
    bool popup_detected = false; // ポップアップ検出
    float confidence   = 0.0f;
    int click_x        = -1;
    int click_y        = -1;
    std::string raw_json;
    std::string error;
};

class Layer2Client {
public:
    explicit Layer2Client(const std::string& python_exe, const std::string& script_path);
    ~Layer2Client();

    // RGBAフレームを受け取りGemini並列投票を非同期実行（内部でJPEGエンコード）
    // @return true: 起動成功, false: 既に実行中/冷却中
    bool launchAsync(const std::string& device_id,
                     const uint8_t* rgba, int width, int height,
                     std::chrono::steady_clock::time_point trigger_time =
                         std::chrono::steady_clock::now());

    // 結果ポーリング（毎フレーム呼び出し）
    // 完了していれば valid=true、未完了なら valid=false
    Layer2Result pollResult(const std::string& device_id);

    bool isRunning(const std::string& device_id) const;

    bool isOnCooldown(const std::string& device_id,
                      std::chrono::steady_clock::time_point now =
                          std::chrono::steady_clock::now()) const;

    void cancel(const std::string& device_id);

    static constexpr int COOLDOWN_MS = 30000;  // 30秒冷却

private:
    struct TaskState {
        std::thread worker;
        std::atomic<bool> running{false};
        std::atomic<bool> done{false};
        Layer2Result result;
        std::mutex result_mutex;
        std::chrono::steady_clock::time_point last_done_time{};
    };

    std::string python_exe_;
    std::string script_path_;
    mutable std::mutex tasks_mutex_;
    std::map<std::string, std::shared_ptr<TaskState>> tasks_;

    // RGBAをJPEGエンコードしてGemini呼び出し
    Layer2Result callScript(const std::vector<uint8_t>& jpeg_data);
    static std::string base64Encode(const uint8_t* data, int size);

    // stb_image_write用コールバック
    static void jpegWriteCallback(void* ctx, void* data, int size);
};

} // namespace mirage::ai
