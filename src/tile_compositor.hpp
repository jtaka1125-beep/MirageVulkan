
// =============================================================================
// TileCompositor - 上下分割タイルを RTP タイムスタンプで同期合成
// =============================================================================
// APK (TiledEncoder 1x2) が 2つのポートに分割送信したフレームを受信し、
// pts_us が一致したペアを上下結合して 1枚の SharedFrame にして返す。
//
// 同期方式:
//   - tile0 (port0): 上半分  tile1 (port1): 下半分
//   - 両タイルの pts_us が一致 → 即座に合成・コールバック
//   - 片側が TIMEOUT_MS 内に来なければ前フレームで補完
//
// スレッドモデル:
//   - compositor_thread_ がポーリング + 合成
//   - コールバックは compositor_thread_ から呼ばれる
// =============================================================================
#pragma once

#include "mirror_receiver.hpp"
#include "event_bus.hpp"

#include <memory>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>  // memcpy

namespace gui {

class TileCompositor {
public:
    using FrameCallback = std::function<void(std::shared_ptr<mirage::SharedFrame>)>;
    // Zero-copy callback: receives top/bottom tiles directly, skipping intermediate compose buffer
    using TiledCallback = std::function<void(
        const std::shared_ptr<mirage::SharedFrame>& top,
        const std::shared_ptr<mirage::SharedFrame>& bot,
        int slice_h)>;

    TileCompositor() = default;
    ~TileCompositor() { stop(); }

    // Vulkan コンテキストを設定（デコーダ初期化用）
    void setVulkanContext(VkPhysicalDevice phys, VkDevice dev,
                          uint32_t gqf, VkQueue gq,
                          uint32_t cqf, VkQueue cq) {
        vk_phys_ = phys; vk_dev_ = dev;
        vk_gqf_ = gqf;  vk_gq_  = gq;
        vk_cqf_ = cqf;  vk_cq_  = cq;
    }

    // フレーム合成完了時のコールバック設定
    void setFrameCallback(FrameCallback cb) { frame_cb_ = std::move(cb); }
    // Preferred: zero-copy path that bypasses compose() intermediate buffer
    void setTiledCallback(TiledCallback cb) { tiled_cb_ = std::move(cb); }

    // ネイティブ解像度を設定（start()前に呼ぶ）
    // native_w/h: デバイス物理解像度（例: 1200x2000）
    // tiles_y   : 垂直タイル分割数（通常 2）
    void set_native_size(int native_w, int native_h, int tiles_y = 2) {
        native_w_  = native_w;
        native_h_  = native_h;
        tiles_y_   = tiles_y;
    }

    // 2 ポートで受信開始（port0=上半分, port1=下半分）
    // host: 接続先IPアドレス（デフォルト127.0.0.1=adb forward、Wi-Fi直接接続時はデバイスIP）
    bool start(uint16_t port0, uint16_t port1, const std::string& host = "127.0.0.1") {
        if (running_.load()) return true;

        receiver_[0] = std::make_unique<MirrorReceiver>();
        receiver_[1] = std::make_unique<MirrorReceiver>();

        for (int t = 0; t < 2; ++t) {
            if (vk_dev_ != VK_NULL_HANDLE) {
                receiver_[t]->setVulkanContext(vk_phys_, vk_dev_,
                    vk_gqf_, vk_gq_, vk_cqf_, vk_cq_);
            }
            if (!receiver_[t]->start_tcp_vid0(t == 0 ? port0 : port1, host)) {
                MLOG_ERROR("tile", "Failed to start tile receiver %d on port %d host %s",
                           t, t == 0 ? port0 : port1, host.c_str());
                return false;
            }
        }

        running_.store(true);
        compositor_thread_ = std::thread(&TileCompositor::compositorLoop, this);
        MLOG_INFO("tile", "TileCompositor started: port %d/%d host %s (top/bottom)",
                  port0, port1, host.c_str());
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (compositor_thread_.joinable()) compositor_thread_.join();
        receiver_[0].reset();
        receiver_[1].reset();
        MLOG_INFO("tile", "TileCompositor stopped");
    }

    bool running() const { return running_.load(); }

private:
    static constexpr int64_t TIMEOUT_US      = 20000;  // 20ms = 1.25フレーム @60fps
    static constexpr int64_t INTERP_LIMIT_US =  9000;  //  9ms: 補完許容PTS差 (half frame @60fps)
    static constexpr int     PAIR_WAIT_MS    =    5;   //  5ms: 片側到着後、もう片側を最大5ms待つ @60fps
    static constexpr int     POLL_MS         = 1;      // ポーリング間隔

    std::unique_ptr<MirrorReceiver> receiver_[2];
    FrameCallback frame_cb_;
    TiledCallback tiled_cb_;
    std::atomic<bool> running_{false};
    std::thread compositor_thread_;

    // Vulkan context
    VkPhysicalDevice vk_phys_ = VK_NULL_HANDLE;
    VkDevice         vk_dev_  = VK_NULL_HANDLE;
    uint32_t vk_gqf_ = 0; VkQueue vk_gq_ = VK_NULL_HANDLE;
    uint32_t vk_cqf_ = 0; VkQueue vk_cq_ = VK_NULL_HANDLE;

    // ネイティブ解像度（合成出力サイズ）
    // start()前にset_native_size()で設定。0なら tile * tilesY そのまま。
    int native_w_ = 0;
    int native_h_ = 0;
    int tiles_y_   = 2;  // TiledEncoder が送ってくるタイル分割数

    // 最新フレーム（補完用）
    std::shared_ptr<mirage::SharedFrame> last_[2];
    // 直前の合成 frame_id（重複合成防止）
    uint32_t last_fid_top_ = 0;
    uint32_t last_fid_bot_ = 0;

    // compose() double buffer (avoid malloc + prevent data race)
    std::vector<uint8_t> compose_buf_[2];
    int compose_buf_idx_ = 0;

    // ============================================================
    // メインループ
    // ============================================================
    void compositorLoop() {
        MLOG_INFO("tile", "Compositor thread started");

        // デバッグカウンター
        int dbg_got0 = 0, dbg_got1 = 0, dbg_composed = 0, dbg_dup = 0, dbg_ptsdiff = 0;
        auto dbg_last = std::chrono::steady_clock::now();

        while (running_.load()) {
            std::shared_ptr<mirage::SharedFrame> sf[2];
            bool got[2] = {false, false};

            // 両タイルの最新フレームを取得
            for (int t = 0; t < 2; ++t) {
                if (receiver_[t]->get_latest_shared_frame(sf[t])) {
                    got[t] = true;
                    last_[t] = sf[t];  // 補完用に保存
                }
            }
            if (got[0]) dbg_got0++;
            if (got[1]) dbg_got1++;

            // デバッグ: 5秒ごとにログ
            auto now_dbg = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now_dbg - dbg_last).count() >= 5) {
                MLOG_INFO("tile", "TileCompositor: got0=%d got1=%d composed=%d dup=%d ptsdiff=%d",
                    dbg_got0, dbg_got1, dbg_composed, dbg_dup, dbg_ptsdiff);
                dbg_got0 = dbg_got1 = dbg_composed = dbg_dup = dbg_ptsdiff = 0;
                dbg_last = now_dbg;
            }

            // どちらも新フレームなし → スリープして継続
            if (!got[0] && !got[1]) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                continue;
            }

            // pts が揃っている場合は即合成
            // 片側だけの場合はもう片側が last_ にあれば補完
            std::shared_ptr<mirage::SharedFrame> top;
            std::shared_ptr<mirage::SharedFrame> bot;

            if (got[0] && got[1]) {
                // 両方届いた: pts が近ければ合成 (差 < TIMEOUT_US)
                int64_t diff = static_cast<int64_t>(sf[0]->pts_us) -
                               static_cast<int64_t>(sf[1]->pts_us);
                if (diff < 0) diff = -diff;

                if (diff < TIMEOUT_US) {
                    top = sf[0];
                    bot = sf[1];
                } else {
                    // pts ずれ: 古い方は破棄して新しい方をキャッシュのみ
                    dbg_ptsdiff++;
                    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                    continue;
                }
            } else if (got[0] || got[1]) {
                // 片側だけ到着: もう片側を最大 PAIR_WAIT_MS 待ってからペアリング試行
                int t_new = got[0] ? 0 : 1;
                int t_old = 1 - t_new;
                int waited = 0;
                while (waited < PAIR_WAIT_MS) {
                    std::shared_ptr<mirage::SharedFrame> sf2;
                    if (receiver_[t_old]->get_latest_shared_frame(sf2)) {
                        last_[t_old] = sf2;
                        // ペア到着: PTS 差チェック
                        int64_t diff = (int64_t)sf[t_new]->pts_us - (int64_t)sf2->pts_us;
                        if (diff < 0) diff = -diff;
                        if (diff < TIMEOUT_US) {
                            top = (t_new == 0) ? sf[t_new] : sf2;
                            bot = (t_new == 0) ? sf2 : sf[t_new];
                        }
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    waited++;
                }
                // ペア見つからない場合 → 補完 (PTS 差が1フレーム以内のみ)
                if (!top && last_[t_old]) {
                    int64_t pts_gap = (int64_t)sf[t_new]->pts_us - (int64_t)last_[t_old]->pts_us;
                    if (pts_gap < 0) pts_gap = -pts_gap;
                    if (pts_gap <= INTERP_LIMIT_US) {
                        top = (t_new == 0) ? sf[t_new] : last_[t_old];
                        bot = (t_new == 0) ? last_[t_old] : sf[t_new];
                    }
                }
                if (!top) {
                    // ペアも補完も不可 → スキップ
                    std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                    continue;
                }
            } else {
                // 補完用フレームもなし
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                continue;
            }

            // 解像度チェック
            if (!top || !top->rgba || !bot || !bot->rgba) {
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                continue;
            }
            if (top->width != bot->width) {
                MLOG_WARN("tile", "Width mismatch: top=%d bot=%d, skip", top->width, bot->width);
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                continue;
            }

            // 重複合成防止 - frame_idの組み合わせで判定
            // タイル0とタイル1は同じソースフレームから同じptsを持つので、frame_idを使う
            uint32_t fid_top = top->frame_id;
            uint32_t fid_bot = bot->frame_id;
            if (fid_top == last_fid_top_ && fid_bot == last_fid_bot_) {
                dbg_dup++;
                std::this_thread::sleep_for(std::chrono::milliseconds(POLL_MS));
                continue;
            }
            last_fid_top_ = fid_top;
            last_fid_bot_ = fid_bot;

            // 上下結合 or ゼロコピーコールバック
            if (tiled_cb_) {
                // Zero-copy path: skip 9.6MB intermediate compose buffer
                const int slice_h = (native_h_ > 0) ? (native_h_ / tiles_y_) : top->height;
                dbg_composed++;
                tiled_cb_(top, bot, slice_h);
            } else {
                auto combined = compose(top, bot);
                if (combined && frame_cb_) {
                    dbg_composed++;
                    frame_cb_(combined);
                }
            }
        }

        MLOG_INFO("tile", "Compositor thread ended");
    }

    // ============================================================
    // RGBA 上下結合
    // ============================================================
    std::shared_ptr<mirage::SharedFrame>
    compose(const std::shared_ptr<mirage::SharedFrame>& top,
            const std::shared_ptr<mirage::SharedFrame>& bot) {
        const int W = top->width;

        // native_h が設定されている場合は各タイルを slice_h 行にクロップして native_h に合成
        // これによりエンコーダの16px ceil-align パディング行を除去し、
        // 出力が常にデバイスネイティブ解像度になる
        const int slice_h = (native_h_ > 0) ? (native_h_ / tiles_y_) : 0;
        const int Htop    = (slice_h > 0 && slice_h <= top->height) ? slice_h : top->height;
        const int Hbot    = (slice_h > 0 && slice_h <= bot->height) ? slice_h : bot->height;
        const int Htotal  = (native_h_ > 0) ? native_h_ : (Htop + Hbot);
        const size_t row_bytes = static_cast<size_t>(W) * 4;
        const size_t total_bytes = row_bytes * static_cast<size_t>(Htotal);

        // Reuse pool buffer - avoid per-frame ~9.5MB malloc
        // Alternate between two buffers to avoid race when previous frame
        // is still being uploaded to GPU by the GUI thread.
        compose_buf_idx_ ^= 1;
        auto& cur_buf = compose_buf_[compose_buf_idx_];
        if (cur_buf.size() < total_bytes) cur_buf.resize(total_bytes);
        uint8_t* buf_ptr = cur_buf.data();

        // 上半分コピー
        std::memcpy(buf_ptr,
                    top->rgba.get(),
                    row_bytes * static_cast<size_t>(Htop));
        // 下半分コピー
        std::memcpy(buf_ptr + row_bytes * static_cast<size_t>(Htop),
                    bot->rgba.get(),
                    row_bytes * static_cast<size_t>(Hbot));

        auto sf = std::make_shared<mirage::SharedFrame>();
        sf->width    = W;
        sf->height   = Htotal;
        sf->pts_us   = top->pts_us;
        sf->frame_id = top->frame_id;
        sf->rgba     = std::shared_ptr<const uint8_t[]>(buf_ptr, [](const uint8_t*){});

        return sf;
    }
};

} // namespace gui
