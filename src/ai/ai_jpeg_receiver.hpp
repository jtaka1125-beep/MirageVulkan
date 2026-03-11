// =============================================================================
// AiJpegReceiver - Android AIストリームからJPEGフレームを受信
// =============================================================================
// プロトコル: [int32 len][int32 w][int32 h][int64 tsUs][bytes jpeg]
// Android側: AiJpegSender.kt が送信
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <cstdint>

namespace mirage::ai {

// JPEGフレームコールバック
// @param device_id デバイスID
// @param jpeg JPEGデータ
// @param width 幅
// @param height 高さ
// @param timestamp_us タイムスタンプ(マイクロ秒)
using AiFrameCallback = std::function<void(
    const std::string& device_id,
    const std::vector<uint8_t>& jpeg,
    int width, int height,
    int64_t timestamp_us)>;

/**
 * AiJpegReceiver - TCPサーバーでAndroidからのAIストリームを受信
 *
 * 使い方:
 *   AiJpegReceiver receiver;
 *   receiver.setFrameCallback([](auto& id, auto& jpeg, w, h, ts) { ... });
 *   receiver.start("device_123", 51200);  // ポート51200でリッスン
 *   // ...
 *   receiver.stop();
 */
class AiJpegReceiver {
public:
    AiJpegReceiver() = default;
    ~AiJpegReceiver();

    // コールバック設定
    void setFrameCallback(AiFrameCallback cb) { callback_ = cb; }

    // サーバー開始 (指定ポートでリッスン)
    // @param device_id フレームに付与するデバイスID
    // @param port リッスンポート
    // @return 成功時true
    bool start(const std::string& device_id, int port);

    // サーバー停止
    void stop();

    // 実行中か
    bool isRunning() const { return running_.load(); }

    // 統計
    uint64_t framesReceived() const { return frames_received_.load(); }
    uint64_t bytesReceived() const { return bytes_received_.load(); }

private:
    void serverThread();
    void clientThread(int client_sock);
    bool readExact(int sock, void* buf, size_t len);

    std::string device_id_;
    int port_ = 0;
    int server_sock_ = -1;

    std::atomic<bool> running_{false};
    std::thread server_thread_;

    AiFrameCallback callback_;

    std::atomic<uint64_t> frames_received_{0};
    std::atomic<uint64_t> bytes_received_{0};

    mutable std::mutex mutex_;
};

} // namespace mirage::ai
