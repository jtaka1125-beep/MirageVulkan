// =============================================================================
// AdbH264Receiver - adb exec-out screenrecord → ffmpeg pipe → JPEG cache
// =============================================================================
// screenrecord H264 を ffmpeg でデコードしてJPEGをキャッシュ
// MirrorReceiver/Vulkan不要。外部ffmpegパイプ方式。
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

    void setAdbPath(const std::string& p)    { adb_path_    = p; }
    void setFfmpegPath(const std::string& p) { ffmpeg_path_ = p; }
    void setDeviceManager(::gui::AdbDeviceManager* mgr) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        adb_mgr_ = mgr;
    }
    bool has_manager() const { return adb_mgr_ != nullptr; }

    bool start() {
        if (running_.load()) return true;
        running_ = true;
        supervisor_thread_ = std::thread(&AdbH264Receiver::supervisorLoop, this);
        MLOG_INFO("adb_h264", "AdbH264Receiver started");
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (supervisor_thread_.joinable()) supervisor_thread_.join();
        std::lock_guard<std::mutex> lk(devices_mtx_);
        for (auto it = devices_.begin(); it != devices_.end(); ++it)
            stopEntry(it->second);
        devices_.clear();
        MLOG_INFO("adb_h264", "AdbH264Receiver stopped");
    }

    bool running() const { return running_.load(); }

    int device_count() const {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        return (int)devices_.size();
    }
    int active_count() const {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        int n = 0;
        for (auto it = devices_.begin(); it != devices_.end(); ++it)
            if (it->second.proc_adb != INVALID_HANDLE_VALUE) n++;
        return n;
    }

    // 最新フレームをJPEGで取得
    bool get_latest_jpeg(const std::string& hw_id,
                         std::vector<uint8_t>& out_jpeg, int& out_w, int& out_h) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        if (it == devices_.end() || it->second.latest_jpeg.empty()) return false;
        out_jpeg = it->second.latest_jpeg;
        out_w = it->second.width;
        out_h = it->second.height;
        return true;
    }

    float get_fps(const std::string& hw_id) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        return (it != devices_.end()) ? it->second.fps : 0.f;
    }

private:
    struct DeviceEntry {
        std::string hardware_id;
        std::string adb_serial;
        HANDLE proc_adb    = INVALID_HANDLE_VALUE;
        HANDLE proc_ffmpeg = INVALID_HANDLE_VALUE;
        HANDLE pipe_adb2ff = INVALID_HANDLE_VALUE;  // adb stdout → ffmpeg stdin
        HANDLE pipe_ff_out = INVALID_HANDLE_VALUE;  // ffmpeg stdout
        std::thread reader_thread;
        std::vector<uint8_t> latest_jpeg;
        int width = 0, height = 0;
        float fps = 0.f;
        uint64_t frame_count = 0;

        DeviceEntry() = default;
        DeviceEntry(DeviceEntry&&) = default;
        DeviceEntry& operator=(DeviceEntry&&) = default;
        DeviceEntry(const DeviceEntry&) = delete;
        DeviceEntry& operator=(const DeviceEntry&) = delete;
    };

    std::string adb_path_;
    std::string ffmpeg_path_;
    ::gui::AdbDeviceManager* adb_mgr_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread supervisor_thread_;
    mutable std::mutex devices_mtx_;
    std::map<std::string, DeviceEntry> devices_;

    std::string getAdb() const {
        return adb_path_.empty() ? "adb" : adb_path_;
    }
    std::string getFfmpeg() const {
        if (!ffmpeg_path_.empty()) return ffmpeg_path_;
        return "C:/msys64/mingw64/bin/ffmpeg.exe";
    }

    void supervisorLoop() {
        for (int i = 0; i < 30 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (adb_mgr_) break;
        }
        while (running_.load()) {
            syncDevices();
            for (int i = 0; i < 150 && running_.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void syncDevices() {
        // Phase1: mutex保持でstartEntry
        std::vector<std::string> to_start_threads;
        {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            if (!adb_mgr_) return;
            auto devs = adb_mgr_->getUniqueDevices();
            MLOG_INFO("adb_h264", "syncDevices: %d devices", (int)devs.size());

            for (size_t i = 0; i < devs.size(); i++) {
                const auto& ud = devs[i];
                if (ud.preferred_adb_id.empty()) continue;
                auto it = devices_.find(ud.hardware_id);
                bool need_start = (it == devices_.end() ||
                    it->second.proc_adb == INVALID_HANDLE_VALUE);
                if (need_start) {
                    if (it != devices_.end()) {
                        stopEntry(it->second);
                        devices_.erase(it);
                    }
                    if (startEntry(ud))
                        to_start_threads.push_back(ud.hardware_id);
                }
            }

            // 削除
            for (auto it = devices_.begin(); it != devices_.end(); ) {
                bool found = false;
                for (size_t i = 0; i < devs.size(); i++)
                    if (devs[i].hardware_id == it->first) { found = true; break; }
                if (!found) { stopEntry(it->second); it = devices_.erase(it); }
                else ++it;
            }
        }

        // Phase2: mutex外でreaderThread起動
        for (auto& hw_id : to_start_threads) {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            auto it = devices_.find(hw_id);
            if (it != devices_.end() && !it->second.reader_thread.joinable()) {
                std::string id = hw_id;
                it->second.reader_thread = std::thread([this, id]() { readerLoop(id); });
            }
        }
    }

    // mutex内で呼ぶ。成功時trueを返す
    bool startEntry(const ::gui::AdbDeviceManager::UniqueDevice& ud) {
        DeviceEntry entry;
        entry.hardware_id = ud.hardware_id;
        entry.adb_serial  = ud.preferred_adb_id;

        int w = ud.screen_width  > 0 ? ud.screen_width  : 800;
        int h = ud.screen_height > 0 ? ud.screen_height : 1344;
        w = ((w + 15) / 16) * 16;
        h = ((h + 15) / 16) * 16;
        std::string size_str = std::to_string(w) + "x" + std::to_string(h);

        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        // パイプ: adb stdout → ffmpeg stdin
        HANDLE pipe_w1, pipe_r1;
        if (!CreatePipe(&pipe_r1, &pipe_w1, &sa, 1 * 1024 * 1024)) {
            MLOG_ERROR("adb_h264", "CreatePipe(adb->ff) failed: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(pipe_r1, HANDLE_FLAG_INHERIT, 0);  // read end non-inherited

        // パイプ: ffmpeg stdout → reader
        HANDLE pipe_w2, pipe_r2;
        if (!CreatePipe(&pipe_r2, &pipe_w2, &sa, 256 * 1024)) {
            CloseHandle(pipe_r1); CloseHandle(pipe_w1);
            MLOG_ERROR("adb_h264", "CreatePipe(ff->reader) failed: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(pipe_r2, HANDLE_FLAG_INHERIT, 0);

        // adbプロセス起動
        std::string adb_cmd = "\"" + getAdb() + "\""
            " -s " + ud.preferred_adb_id +
            " exec-out screenrecord --output-format=h264"
            " --bit-rate 4M --size " + size_str + " --time-limit 0 -";

        STARTUPINFOA si_adb{}; si_adb.cb = sizeof(si_adb);
        si_adb.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_adb.hStdOutput = pipe_w1;
        si_adb.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_adb.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_adb{};
        if (!CreateProcessA(nullptr, adb_cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si_adb, &pi_adb)) {
            MLOG_ERROR("adb_h264", "CreateProcess(adb) failed: %lu cmd=[%s]",
                       GetLastError(), adb_cmd.c_str());
            CloseHandle(pipe_r1); CloseHandle(pipe_w1);
            CloseHandle(pipe_r2); CloseHandle(pipe_w2);
            return false;
        }
        CloseHandle(pipe_w1);
        CloseHandle(pi_adb.hThread);
        entry.proc_adb = pi_adb.hProcess;

        // ffmpegプロセス: H264 → MJPEG (rawjpeg stream)
        // ffmpeg -f h264 -i pipe:0 -vf fps=15 -q:v 3 -f mjpeg pipe:1
        std::string ff_cmd = "\"" + getFfmpeg() + "\""
            " -f h264 -i pipe:0"
            " -vf fps=15"
            " -q:v 3"
            " -f mjpeg pipe:1";

        STARTUPINFOA si_ff{}; si_ff.cb = sizeof(si_ff);
        si_ff.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_ff.hStdInput  = pipe_r1;
        si_ff.hStdOutput = pipe_w2;
        si_ff.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_ff.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_ff{};
        if (!CreateProcessA(nullptr, ff_cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si_ff, &pi_ff)) {
            MLOG_ERROR("adb_h264", "CreateProcess(ffmpeg) failed: %lu", GetLastError());
            TerminateProcess(entry.proc_adb, 0);
            CloseHandle(entry.proc_adb); entry.proc_adb = INVALID_HANDLE_VALUE;
            CloseHandle(pipe_r1);
            CloseHandle(pipe_r2); CloseHandle(pipe_w2);
            return false;
        }
        CloseHandle(pipe_r1);
        CloseHandle(pipe_w2);
        CloseHandle(pi_ff.hThread);
        entry.proc_ffmpeg = pi_ff.hProcess;
        entry.pipe_ff_out = pipe_r2;
        entry.width  = w;
        entry.height = h;

        MLOG_INFO("adb_h264", "Stream+ffmpeg started: %s (%s) %s",
                  ud.display_name.c_str(), ud.preferred_adb_id.c_str(), size_str.c_str());

        devices_.insert(std::make_pair(ud.hardware_id, std::move(entry)));
        return true;
    }

    void readerLoop(const std::string& hw_id) {
        // MJPEG = 連続したJPEGバイナリ。各JPEGはFFD8...FFD9で区切られる
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
                if (it == devices_.end()) break;
                hRead = it->second.pipe_ff_out;
            }

            DWORD n = 0;
            if (!ReadFile(hRead, chunk.data(), (DWORD)chunk.size(), &n, nullptr) || n == 0)
                break;

            buf.insert(buf.end(), chunk.begin(), chunk.begin() + n);

            // JPEG SOI(FFD8) / EOI(FFD9)を探してフレームを切り出す
            size_t start = 0;
            while (start + 4 <= buf.size()) {
                if (buf[start] != 0xFF || buf[start+1] != 0xD8) { start++; continue; }
                // SOIを見つけた。EOIを探す
                size_t end = start + 2;
                while (end + 2 <= buf.size()) {
                    if (buf[end] == 0xFF && buf[end+1] == 0xD9) {
                        end += 2;
                        // JPEGフレーム完成
                        std::vector<uint8_t> jpeg(buf.begin() + start, buf.begin() + end);
                        frames++;
                        float elapsed = std::chrono::duration<float>(
                            std::chrono::steady_clock::now() - t0).count();
                        {
                            std::lock_guard<std::mutex> lk(devices_mtx_);
                            auto it = devices_.find(hw_id);
                            if (it != devices_.end()) {
                                it->second.latest_jpeg = std::move(jpeg);
                                it->second.frame_count = frames;
                                if (elapsed > 0.5f) it->second.fps = frames / elapsed;
                            }
                        }
                        start = end;
                        break;
                    }
                    end++;
                }
                if (end + 2 > buf.size()) break;  // EOI未到着
            }
            if (start > 0) buf.erase(buf.begin(), buf.begin() + start);
            if (buf.size() > 4 * 1024 * 1024) buf.clear();  // overflow防止
        }

        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        if (it != devices_.end()) {
            it->second.pipe_ff_out = INVALID_HANDLE_VALUE;
        }
        MLOG_INFO("adb_h264", "Reader exited: %s", hw_id.c_str());
    }

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
        if (e.reader_thread.joinable()) e.reader_thread.join();
    }
};

} // namespace mirage
