// =============================================================================
// AdbH264Receiver - adb exec-out screenrecord → MirrorReceiver decoder
// =============================================================================
// screenrecordのraw H.264をMirrorReceiver(FFmpeg)でデコード → JPEG変換
// MacroAPI screenshot fast path (25-40 FPS)
// ffmpeg外部プロセス不要。MediaProjection許可不要。
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
#include "mirror_receiver.hpp"
#include "adb_device_manager.hpp"
#include "mirage_log.hpp"

// stb_image_write forward declaration
extern "C" int stbi_write_jpg_to_func(void (*func)(void*,void*,int), void* context,
    int x, int y, int comp, const void* data, int quality);

namespace mirage {

class AdbH264Receiver {
public:
    AdbH264Receiver() = default;
    ~AdbH264Receiver() { stop(); }

    void setAdbPath(const std::string& path) { adb_path_ = path; }
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
            if (it->second.active) n++;
        return n;
    }

    // 最新フレームをJPEGで取得
    bool get_latest_jpeg(const std::string& hw_id,
                         std::vector<uint8_t>& out_jpeg, int& out_w, int& out_h) {
        ::gui::MirrorFrame frame;
        {
            std::lock_guard<std::mutex> lk(devices_mtx_);
            auto it = devices_.find(hw_id);
            if (it == devices_.end() || !it->second.decoder) return false;
            if (!it->second.decoder->get_latest_frame(frame)) return false;
        }
        if (frame.rgba.empty() || frame.width <= 0) return false;

        out_w = frame.width;
        out_h = frame.height;

        // RGBAをJPEGへ
        out_jpeg.clear();
        out_jpeg.reserve(frame.width * frame.height / 4);
        stbi_write_jpg_to_func([](void* ctx, void* data, int sz) {
            auto* v = static_cast<std::vector<uint8_t>*>(ctx);
            const uint8_t* p = static_cast<const uint8_t*>(data);
            v->insert(v->end(), p, p + sz);
        }, &out_jpeg, frame.width, frame.height, 4, frame.rgba.data(), 80);

        return !out_jpeg.empty();
    }

    float get_fps(const std::string& hw_id) {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        if (it == devices_.end()) return 0.f;
        return it->second.fps;
    }

private:
    struct DeviceEntry {
        std::string hardware_id;
        std::string adb_serial;
        std::string size_str;
        HANDLE proc_handle  = INVALID_HANDLE_VALUE;
        HANDLE stdout_read  = INVALID_HANDLE_VALUE;
        std::unique_ptr<::gui::MirrorReceiver> decoder;
        std::thread reader_thread;
        bool active = false;
        float fps = 0.f;
        uint64_t frame_count = 0;

        DeviceEntry() = default;
        DeviceEntry(DeviceEntry&&) = default;
        DeviceEntry& operator=(DeviceEntry&&) = default;
        DeviceEntry(const DeviceEntry&) = delete;
        DeviceEntry& operator=(const DeviceEntry&) = delete;
    };

    std::string adb_path_;                    // "adb" or full path
    ::gui::AdbDeviceManager* adb_mgr_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread supervisor_thread_;
    mutable std::mutex devices_mtx_;
    std::map<std::string, DeviceEntry> devices_;

    std::string getAdb() const {
        if (!adb_path_.empty()) return adb_path_;
        return "adb";
    }

    void supervisorLoop() {
        // 最初の数秒はadb_mgr_を待つ
        for (int i = 0; i < 30 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (adb_mgr_) break;
        }
        while (running_.load()) {
            syncDevices();
            for (int i = 0; i < 30 && running_.load(); i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    void syncDevices() {
        std::lock_guard<std::mutex> lk(devices_mtx_);
        if (!adb_mgr_) return;

        auto devs = adb_mgr_->getUniqueDevices();
        MLOG_INFO("adb_h264", "syncDevices: %d unique devices", (int)devs.size());

        // 追加・再起動
        for (size_t i = 0; i < devs.size(); i++) {
            const ::gui::AdbDeviceManager::UniqueDevice& ud = devs[i];
            if (ud.preferred_adb_id.empty()) continue;

            auto it = devices_.find(ud.hardware_id);
            if (it != devices_.end()) {
                if (it->second.active) continue;
                stopEntry(it->second);
                devices_.erase(it);
            }
            startEntry(ud);
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

    void startEntry(const ::gui::AdbDeviceManager::UniqueDevice& ud) {
        DeviceEntry entry;
        entry.hardware_id = ud.hardware_id;
        entry.adb_serial  = ud.preferred_adb_id;

        int w = ud.screen_width  > 0 ? ud.screen_width  : 800;
        int h = ud.screen_height > 0 ? ud.screen_height : 1344;
        // 16の倍数に切り上げ (H.264制限)
        w = ((w + 15) / 16) * 16;
        h = ((h + 15) / 16) * 16;
        entry.size_str = std::to_string(w) + "x" + std::to_string(h);

        // デコーダ初期化
        entry.decoder = std::make_unique<::gui::MirrorReceiver>();
        if (!entry.decoder->start_decoder_only()) {
            MLOG_ERROR("adb_h264", "Decoder init failed for %s", ud.hardware_id.c_str());
            return;
        }

        // パイプ作成
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE stdout_write = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&entry.stdout_read, &stdout_write, &sa, 0)) {
            MLOG_ERROR("adb_h264", "CreatePipe failed: %lu", GetLastError());
            return;
        }
        SetHandleInformation(entry.stdout_read, HANDLE_FLAG_INHERIT, 0);

        // コマンド: cmd /c adb -s <serial> exec-out screenrecord ...
        std::string cmd = "cmd /c " + getAdb() +
            " -s " + entry.adb_serial +
            " exec-out screenrecord --output-format=h264"
            " --bit-rate 4M"
            " --size " + entry.size_str +
            " --time-limit 0 -";

        MLOG_INFO("adb_h264", "Launching: %s", cmd.c_str());

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = stdout_write;
        si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi{};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr,
                            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            MLOG_ERROR("adb_h264", "CreateProcess failed: %lu  cmd=[%s]",
                       GetLastError(), cmd.c_str());
            CloseHandle(entry.stdout_read);
            CloseHandle(stdout_write);
            return;
        }
        CloseHandle(stdout_write);
        CloseHandle(pi.hThread);
        entry.proc_handle = pi.hProcess;
        entry.active = true;

        MLOG_INFO("adb_h264", "Stream started: %s (%s) %s",
                  ud.display_name.c_str(), entry.adb_serial.c_str(),
                  entry.size_str.c_str());

        std::string hw_id = ud.hardware_id;
        entry.reader_thread = std::thread([this, hw_id]() { readerLoop(hw_id); });
        devices_.insert(std::make_pair(ud.hardware_id, std::move(entry)));
    }

    void readerLoop(const std::string& hw_id) {
        const size_t BUF = 65536;
        std::vector<uint8_t> buf(BUF);
        auto t0 = std::chrono::steady_clock::now();
        uint64_t frames = 0;

        while (running_.load()) {
            HANDLE hRead;
            {
                std::lock_guard<std::mutex> lk(devices_mtx_);
                auto it = devices_.find(hw_id);
                if (it == devices_.end() || !it->second.active) break;
                hRead = it->second.stdout_read;
            }

            DWORD n = 0;
            if (!ReadFile(hRead, buf.data(), (DWORD)BUF, &n, nullptr) || n == 0)
                break;

            // NALカウント
            for (size_t i = 0; i + 4 < (size_t)n; i++)
                if (buf[i]==0&&buf[i+1]==0&&buf[i+2]==0&&buf[i+3]==1) {
                    uint8_t t = buf[i+4] & 0x1F;
                    if (t==1||t==5) frames++;
                }

            {
                std::lock_guard<std::mutex> lk(devices_mtx_);
                auto it = devices_.find(hw_id);
                if (it == devices_.end()) break;
                if (it->second.decoder)
                    it->second.decoder->process_raw_h264(buf.data(), n);
                it->second.frame_count = frames;
                float elapsed = std::chrono::duration<float>(
                    std::chrono::steady_clock::now() - t0).count();
                if (elapsed > 0.5f) it->second.fps = frames / elapsed;
            }
        }

        std::lock_guard<std::mutex> lk(devices_mtx_);
        auto it = devices_.find(hw_id);
        if (it != devices_.end()) it->second.active = false;
        MLOG_INFO("adb_h264", "Reader exited: %s", hw_id.c_str());
    }

    void stopEntry(DeviceEntry& e) {
        e.active = false;
        if (e.proc_handle != INVALID_HANDLE_VALUE) {
            TerminateProcess(e.proc_handle, 0);
            WaitForSingleObject(e.proc_handle, 2000);
            CloseHandle(e.proc_handle);
            e.proc_handle = INVALID_HANDLE_VALUE;
        }
        if (e.stdout_read != INVALID_HANDLE_VALUE) {
            CloseHandle(e.stdout_read);
            e.stdout_read = INVALID_HANDLE_VALUE;
        }
        if (e.reader_thread.joinable()) e.reader_thread.join();
        e.decoder.reset();
    }
};

} // namespace mirage
