// =============================================================================
// AdbH264Receiver v2
// adb exec-out screenrecord (H264) → ffmpeg pipe (MJPEG) → JPEG cache
// MirrorReceiver/Vulkan不要。クラッシュしない外部プロセス方式。
// =============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <vector>
#include <map>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <chrono>
#include "adb_device_manager.hpp"
#include "mirage_log.hpp"

namespace mirage {

class AdbH264Receiver {
public:
    AdbH264Receiver() = default;
    ~AdbH264Receiver() { stop(); }

    // --- 設定 ---
    void setAdbPath(const std::string& p)    { adb_path_    = p; }
    void setFfmpegPath(const std::string& p) { ffmpeg_path_ = p; }

    void setDeviceManager(::gui::AdbDeviceManager* mgr) {
        {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            adb_mgr_ = mgr;
        }
        // 即座にsyncDevicesをトリガー
        sync_requested_.store(true);
    }
    bool has_manager() const { return adb_mgr_ != nullptr; }

    // --- ライフサイクル ---
    bool start() {
        if (running_.load()) return true;
        running_ = true;
        supervisor_thread_ = std::thread(&AdbH264Receiver::supervisorLoop, this);
        MLOG_INFO("adb_h264", "AdbH264Receiver started (ffmpeg-pipe mode)");
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        sync_requested_.store(false);
        if (supervisor_thread_.joinable()) supervisor_thread_.join();
        std::lock_guard<std::mutex> lk(devices_mtx_);
        for (auto& kv : devices_) stopEntry(kv.second);
        devices_.clear();
        MLOG_INFO("adb_h264", "AdbH264Receiver stopped");
    }

    bool running()      const { return running_.load(); }
    int  device_count() const {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        return (int)devices_.size();
    }
    int  active_count() const {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        int n = 0;
        for (auto& kv : devices_)
            if (kv.second.proc_adb != INVALID_HANDLE_VALUE) n++;
        return n;
    }

    // 最新フレームをJPEGで取得（スレッドセーフ）
    bool get_latest_jpeg(const std::string& hw_id,
                         std::vector<uint8_t>& out_jpeg, int& out_w, int& out_h) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        if (it == devices_.end() || it->second.latest_jpeg.empty()) return false;
        out_jpeg = it->second.latest_jpeg;
        out_w    = it->second.width;
        out_h    = it->second.height;
        return true;
    }

    float get_fps(const std::string& hw_id) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        return (it != devices_.end()) ? it->second.fps : 0.f;
    }

private:
    // ----------------------------------------------------------------
    struct DeviceEntry {
        std::string hardware_id;
        std::string adb_serial;
        HANDLE proc_adb     = INVALID_HANDLE_VALUE;
        HANDLE proc_ffmpeg  = INVALID_HANDLE_VALUE;
        HANDLE pipe_ff_out  = INVALID_HANDLE_VALUE;  // ffmpeg stdout (MJPEG)
        std::thread reader_thread;
        std::vector<uint8_t> latest_jpeg;
        int     width  = 0;
        int     height = 0;
        float   fps    = 0.f;
        uint64_t frame_count = 0;

        DeviceEntry() = default;
        DeviceEntry(DeviceEntry&&) = default;
        DeviceEntry& operator=(DeviceEntry&&) = default;
        DeviceEntry(const DeviceEntry&) = delete;
        DeviceEntry& operator=(const DeviceEntry&) = delete;
    };

    // ----------------------------------------------------------------
    std::string adb_path_;
    std::string ffmpeg_path_;
    ::gui::AdbDeviceManager* adb_mgr_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> sync_requested_{false};  // setDeviceManager後の即座sync用
    std::thread supervisor_thread_;
    mutable std::mutex devices_mtx_;
    std::map<std::string, DeviceEntry> devices_;

    // ----------------------------------------------------------------
    std::string getAdb() const {
        return adb_path_.empty() ? "adb" : adb_path_;
    }
    std::string getFfmpeg() const {
        if (!ffmpeg_path_.empty()) return ffmpeg_path_;
        return "C:/msys64/mingw64/bin/ffmpeg.exe";
    }

    // ----------------------------------------------------------------
    void supervisorLoop() {
        // adb_mgrが設定されるまで最大10秒待つ
        for (int i = 0; i < 50 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (adb_mgr_) break;
        }

        while (running_.load()) {
            syncDevices();

            // 次のsyncまで待つ（30秒、ただしsync_requestedで短縮可）
            for (int i = 0; i < 150 && running_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (sync_requested_.exchange(false)) {
                    // setDeviceManagerが呼ばれた → 即座にsync
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    break;
                }
            }
        }
    }

    // ----------------------------------------------------------------
    void syncDevices() {
        std::vector<std::string> to_start_threads;

        // Phase1: mutex保持でプロセス起動
        {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            if (!adb_mgr_) return;

            auto devs = adb_mgr_->getUniqueDevices();
            MLOG_INFO("adb_h264", "syncDevices: %d unique devices", (int)devs.size());

            // 新規デバイスを起動
            for (const auto& ud : devs) {
                if (ud.preferred_adb_id.empty()) continue;
                auto it = devices_.find(ud.hardware_id);
                bool need_start = (it == devices_.end());
                // 既存でも死んでたら再起動
                if (!need_start && it->second.proc_adb != INVALID_HANDLE_VALUE) {
                    DWORD code = 0;
                    if (!GetExitCodeProcess(it->second.proc_adb, &code) ||
                        code != STILL_ACTIVE) {
                        MLOG_WARN("adb_h264", "Process died for %s, restarting",
                                  ud.hardware_id.c_str());
                        stopEntry(it->second);
                        devices_.erase(it);
                        need_start = true;
                    }
                }
                if (need_start) {
                    if (startEntry(ud))
                        to_start_threads.push_back(ud.hardware_id);
                }
            }

            // 削除されたデバイスを停止
            for (auto it = devices_.begin(); it != devices_.end(); ) {
                bool found = false;
                for (const auto& ud : devs)
                    if (ud.hardware_id == it->first) { found = true; break; }
                if (!found) {
                    MLOG_INFO("adb_h264", "Device removed: %s", it->first.c_str());
                    stopEntry(it->second);
                    it = devices_.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // Phase2: mutex外でreaderThread起動（デッドロック回避）
        for (const auto& hw_id : to_start_threads) {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            auto it = devices_.find(hw_id);
            if (it != devices_.end() && !it->second.reader_thread.joinable()) {
                std::string id = hw_id;
                it->second.reader_thread = std::thread([this, id]() { readerLoop(id); });
            }
        }
    }

    // ----------------------------------------------------------------
    // mutex保持中に呼ぶ。成功時true。devices_にinsert済み。
    bool startEntry(const ::gui::AdbDeviceManager::UniqueDevice& ud) {
        DeviceEntry entry;
        entry.hardware_id = ud.hardware_id;
        entry.adb_serial  = ud.preferred_adb_id;

        int w = ud.screen_width  > 0 ? ud.screen_width  : 800;
        int h = ud.screen_height > 0 ? ud.screen_height : 1344;
        // H.264制限: 16の倍数
        w = ((w + 15) / 16) * 16;
        h = ((h + 15) / 16) * 16;
        std::string size_str = std::to_string(w) + "x" + std::to_string(h);
        entry.width  = w;
        entry.height = h;

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // パイプ: adb stdout → ffmpeg stdin (1MB buffer)
        HANDLE pipe_adb_r, pipe_adb_w;
        if (!CreatePipe(&pipe_adb_r, &pipe_adb_w, &sa, 1024 * 1024)) {
            MLOG_ERROR("adb_h264", "CreatePipe(adb->ff) failed: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(pipe_adb_r, HANDLE_FLAG_INHERIT, 0);  // readは非継承

        // パイプ: ffmpeg stdout → reader (256KB buffer)
        HANDLE pipe_ff_r, pipe_ff_w;
        if (!CreatePipe(&pipe_ff_r, &pipe_ff_w, &sa, 256 * 1024)) {
            CloseHandle(pipe_adb_r); CloseHandle(pipe_adb_w);
            MLOG_ERROR("adb_h264", "CreatePipe(ff->reader) failed: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(pipe_ff_r, HANDLE_FLAG_INHERIT, 0);  // readは非継承

        // --- adbプロセス起動 ---
        std::string adb_cmd =
            "\"" + getAdb() + "\""
            " -s " + ud.preferred_adb_id +
            " exec-out screenrecord --output-format=h264"
            " --bit-rate 4M --size " + size_str + " --time-limit 0 -";

        STARTUPINFOA si_adb{};
        si_adb.cb = sizeof(si_adb);
        si_adb.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_adb.hStdOutput = pipe_adb_w;
        si_adb.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_adb.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_adb{};

        if (!CreateProcessA(nullptr, adb_cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &si_adb, &pi_adb)) {
            MLOG_ERROR("adb_h264", "adb launch failed: %lu [%s]",
                       GetLastError(), adb_cmd.c_str());
            CloseHandle(pipe_adb_r); CloseHandle(pipe_adb_w);
            CloseHandle(pipe_ff_r);  CloseHandle(pipe_ff_w);
            return false;
        }
        CloseHandle(pipe_adb_w);      // adbがwriteを保持してるのでreaderCloseHandleのみ
        CloseHandle(pi_adb.hThread);
        entry.proc_adb = pi_adb.hProcess;

        // --- ffmpegプロセス起動: H264 → MJPEG ---
        // -vf fps=15 で15FPS出力（調整可）
        // -q:v 3 でJPEG品質(1=最高, 31=最低)
        std::string ff_cmd =
            "\"" + getFfmpeg() + "\""
            " -loglevel error"
            " -f h264 -i pipe:0"
            " -vf fps=15"
            " -q:v 3"
            " -f mjpeg pipe:1";

        STARTUPINFOA si_ff{};
        si_ff.cb = sizeof(si_ff);
        si_ff.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_ff.hStdInput  = pipe_adb_r;  // adb stdout → ffmpeg stdin
        si_ff.hStdOutput = pipe_ff_w;   // ffmpeg stdout → reader
        si_ff.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_ff.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_ff{};

        if (!CreateProcessA(nullptr, ff_cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &si_ff, &pi_ff)) {
            MLOG_ERROR("adb_h264", "ffmpeg launch failed: %lu", GetLastError());
            TerminateProcess(entry.proc_adb, 0);
            CloseHandle(entry.proc_adb); entry.proc_adb = INVALID_HANDLE_VALUE;
            CloseHandle(pipe_adb_r);
            CloseHandle(pipe_ff_r); CloseHandle(pipe_ff_w);
            return false;
        }
        CloseHandle(pipe_adb_r);      // ffmpegがstdinを保持してるので閉じる
        CloseHandle(pipe_ff_w);       // ffmpegがstdoutを保持してるので閉じる
        CloseHandle(pi_ff.hThread);
        entry.proc_ffmpeg  = pi_ff.hProcess;
        entry.pipe_ff_out  = pipe_ff_r;

        MLOG_INFO("adb_h264", "Started: %s (%s) %s adb_pid=%lu ff_pid=%lu",
                  ud.display_name.c_str(), ud.preferred_adb_id.c_str(),
                  size_str.c_str(), pi_adb.dwProcessId, pi_ff.dwProcessId);

        devices_.insert({ud.hardware_id, std::move(entry)});
        return true;
    }

    // ----------------------------------------------------------------
    // MJPEG streamからJPEGフレームを切り出してキャッシュに保存
    void readerLoop(const std::string& hw_id) {
        std::vector<uint8_t> buf;
        buf.reserve(512 * 1024);
        std::vector<uint8_t> chunk(65536);
        auto t0 = std::chrono::steady_clock::now();
        uint64_t frames = 0;

        while (running_.load()) {
            HANDLE hRead;
            {
                std::lock_guard<std::mutex> lk(devices_mtx_);
                auto it = devices_.find(hw_id);
                if (it == devices_.end() || it->second.pipe_ff_out == INVALID_HANDLE_VALUE)
                    break;
                hRead = it->second.pipe_ff_out;
            }

            DWORD n = 0;
            BOOL ok = ReadFile(hRead, chunk.data(), (DWORD)chunk.size(), &n, nullptr);
            if (!ok || n == 0) {
                MLOG_WARN("adb_h264", "Reader EOF/error for %s", hw_id.c_str());
                break;
            }

            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);

            // MJPEG: SOI(FFD8FF) + ... + EOI(FFD9)の繰り返し
            // bufからJPEGフレームを全部取り出す
            size_t pos = 0;
            while (pos + 4 <= buf.size()) {
                // SOI探索
                if (!(buf[pos] == 0xFF && buf[pos+1] == 0xD8)) {
                    pos++;
                    continue;
                }
                // EOI探索 (FFD9)
                // JPEG内にはEOIが1つだけある
                size_t end = buf.size();
                for (size_t j = pos + 2; j + 1 < buf.size(); j++) {
                    if (buf[j] == 0xFF && buf[j+1] == 0xD9) {
                        end = j + 2;
                        break;
                    }
                }
                if (end == buf.size()) break;  // EOI未到着、次回へ

                // JPEGフレーム完成
                frames++;
                float elapsed = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - t0).count();
                {
                    std::lock_guard<std::mutex> lk(devices_mtx_);
                    auto it = devices_.find(hw_id);
                    if (it != devices_.end()) {
                        it->second.latest_jpeg.assign(buf.begin() + pos,
                                                      buf.begin() + end);
                        it->second.frame_count = frames;
                        if (elapsed > 1.0f)
                            it->second.fps = (float)frames / elapsed;
                    }
                }
                pos = end;
            }

            // 処理済み部分をbufから除去
            if (pos > 0)
                buf.erase(buf.begin(), buf.begin() + pos);

            // バッファ溢れ防止
            if (buf.size() > 8 * 1024 * 1024) {
                MLOG_WARN("adb_h264", "Buffer overflow for %s, clearing", hw_id.c_str());
                buf.clear();
            }
        }

        // 終了時にpipe_ff_outをクリア
        {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            auto it = devices_.find(hw_id);
            if (it != devices_.end())
                it->second.pipe_ff_out = INVALID_HANDLE_VALUE;
        }
        MLOG_INFO("adb_h264", "Reader exited: %s (frames=%llu)", hw_id.c_str(), frames);
    }

    // ----------------------------------------------------------------
    // mutex保持中に呼ぶ
    void stopEntry(DeviceEntry& e) {
        if (e.proc_adb != INVALID_HANDLE_VALUE) {
            TerminateProcess(e.proc_adb, 0);
            WaitForSingleObject(e.proc_adb, 2000);
            CloseHandle(e.proc_adb);
            e.proc_adb = INVALID_HANDLE_VALUE;
        }
        if (e.proc_ffmpeg != INVALID_HANDLE_VALUE) {
            TerminateProcess(e.proc_ffmpeg, 0);
            WaitForSingleObject(e.proc_ffmpeg, 2000);
            CloseHandle(e.proc_ffmpeg);
            e.proc_ffmpeg = INVALID_HANDLE_VALUE;
        }
        if (e.pipe_ff_out != INVALID_HANDLE_VALUE) {
            CloseHandle(e.pipe_ff_out);
            e.pipe_ff_out = INVALID_HANDLE_VALUE;
        }
        if (e.reader_thread.joinable()) {
            // reader_threadはReadFileでブロックしてる可能性
            // pipe_ff_outを閉じたのでReadFileが戻るはず
            e.reader_thread.join();
        }
    }
};

} // namespace mirage
