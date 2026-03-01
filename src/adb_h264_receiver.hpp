// =============================================================================
// AdbH264Receiver v7 - オンデマンド screencap方式
// screenshotリクエスト時にその場で adb screencap → ffmpeg → JPEG
// パイプ継承制御を正しく実装 (Pythonと同等の動作)
// =============================================================================
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <atomic>
#include "adb_device_manager.hpp"
#include "mirage_log.hpp"

namespace mirage {

class AdbH264Receiver {
public:
    AdbH264Receiver()  = default;
    ~AdbH264Receiver() { stop(); }

    void setAdbPath(const std::string& p)    { adb_path_    = p; }
    void setFfmpegPath(const std::string& p) { ffmpeg_path_ = p; }

    void setDeviceManager(::gui::AdbDeviceManager* mgr) {
        std::lock_guard<std::mutex> lk(mtx_);
        adb_mgr_ = mgr;
    }
    bool has_manager() const { return adb_mgr_ != nullptr; }

    bool start() { running_ = true; return true; }
    void stop()  { running_ = false; }
    bool running() const { return running_.load(); }

    // デバイス数 = adb_managerのデバイス数
    int device_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!adb_mgr_) return 0;
        return (int)adb_mgr_->getUniqueDevices().size();
    }

    // オンデマンドスクリーンキャプチャ → JPEG
    bool get_latest_jpeg(const std::string& hw_id_or_adb,
                         std::vector<uint8_t>& out_jpeg, int& out_w, int& out_h) {
        std::string adb_serial;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!adb_mgr_) return false;
            for (auto& ud : adb_mgr_->getUniqueDevices()) {
                if (ud.hardware_id == hw_id_or_adb ||
                    ud.preferred_adb_id == hw_id_or_adb) {
                    adb_serial = ud.preferred_adb_id;
                    out_w = ud.screen_width  > 0 ? ud.screen_width  : 1080;
                    out_h = ud.screen_height > 0 ? ud.screen_height : 1920;
                    break;
                }
            }
        }
        if (adb_serial.empty()) return false;
        return captureOneFrame(adb_serial, out_jpeg);
    }

    float get_fps(const std::string&) { return 0.f; }

private:
    std::string adb_path_;
    std::string ffmpeg_path_;
    ::gui::AdbDeviceManager* adb_mgr_ = nullptr;
    std::atomic<bool> running_{false};
    mutable std::mutex mtx_;

    std::string getAdb() const {
        return adb_path_.empty() ? "adb" : adb_path_;
    }
    std::string getFfmpeg() const {
        return ffmpeg_path_.empty() ? "C:/msys64/mingw64/bin/ffmpeg.exe" : ffmpeg_path_;
    }

    // ============================================================
    // パイプ継承を正しく制御した pipe chain:
    //   adb [stdout=p1w] → p1r → ffmpeg [stdin=p1r, stdout=p2w] → p2r → ReadFile
    //
    // 継承ルール:
    //   adb起動前:  p1w=Inherit ON, 他=Inherit OFF
    //   ffmpeg起動前: p1r=Inherit ON, p2w=Inherit ON, 他=Inherit OFF
    //   C++は p2r を ReadFile で読む (NonInheritable)
    // ============================================================
    bool captureOneFrame(const std::string& adb_serial, std::vector<uint8_t>& jpeg_out) {
        // 全パイプをInherit=FALSEで作成してから必要なものだけONにする
        SECURITY_ATTRIBUTES sa_no{}; sa_no.nLength = sizeof(sa_no); sa_no.bInheritHandle = FALSE;

        HANDLE p1r = INVALID_HANDLE_VALUE, p1w = INVALID_HANDLE_VALUE;
        HANDLE p2r = INVALID_HANDLE_VALUE, p2w = INVALID_HANDLE_VALUE;

        // Pipe1: adb_stdout → ffmpeg_stdin
        if (!CreatePipe(&p1r, &p1w, &sa_no, 1024*1024)) {
            MLOG_ERROR("adb_h264", "CreatePipe(p1) failed: %lu", GetLastError());
            return false;
        }
        // Pipe2: ffmpeg_stdout → C++_read
        if (!CreatePipe(&p2r, &p2w, &sa_no, 512*1024)) {
            MLOG_ERROR("adb_h264", "CreatePipe(p2) failed: %lu", GetLastError());
            CloseHandle(p1r); CloseHandle(p1w); return false;
        }

        // --- adb 起動: p1wをInheritONにして渡す ---
        SetHandleInformation(p1w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        std::string adb_cmd = "\"" + getAdb() + "\" -s " + adb_serial +
                              " exec-out screencap -p";
        STARTUPINFOA si_a{}; si_a.cb = sizeof(si_a);
        si_a.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_a.hStdOutput = p1w;
        si_a.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_a.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_a{};
        std::string abuf = adb_cmd;
        bool adb_ok = CreateProcessA(nullptr, abuf.data(), nullptr, nullptr,
                                      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si_a, &pi_a);
        // adb起動後にp1wを閉じる (adbが持つのでC++は不要)
        SetHandleInformation(p1w, HANDLE_FLAG_INHERIT, 0);
        CloseHandle(p1w); p1w = INVALID_HANDLE_VALUE;

        if (!adb_ok) {
            MLOG_ERROR("adb_h264", "adb CreateProcess failed: %lu [%s]", GetLastError(), adb_cmd.c_str());
            CloseHandle(p1r); CloseHandle(p2r); CloseHandle(p2w); return false;
        }
        CloseHandle(pi_a.hThread);

        // --- ffmpeg 起動: p1rとp2wをInheritONにして渡す ---
        SetHandleInformation(p1r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        SetHandleInformation(p2w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

        std::string ff_cmd = "\"" + getFfmpeg() + "\""
                             " -loglevel error"
                             " -i pipe:0"
                             " -vframes 1"
                             " -f image2 -vcodec mjpeg -q:v 5"
                             " pipe:1";
        STARTUPINFOA si_f{}; si_f.cb = sizeof(si_f);
        si_f.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si_f.hStdInput  = p1r;
        si_f.hStdOutput = p2w;
        si_f.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
        si_f.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi_f{};
        std::string fbuf = ff_cmd;
        bool ff_ok = CreateProcessA(nullptr, fbuf.data(), nullptr, nullptr,
                                     TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si_f, &pi_f);
        // ffmpeg起動後に不要なハンドルをC++から閉じる
        SetHandleInformation(p1r, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(p2w, HANDLE_FLAG_INHERIT, 0);
        CloseHandle(p1r); p1r = INVALID_HANDLE_VALUE;
        CloseHandle(p2w); p2w = INVALID_HANDLE_VALUE;

        if (!ff_ok) {
            MLOG_ERROR("adb_h264", "ffmpeg CreateProcess failed: %lu", GetLastError());
            TerminateProcess(pi_a.hProcess, 0);
            WaitForSingleObject(pi_a.hProcess, 2000);
            CloseHandle(pi_a.hProcess);
            CloseHandle(p2r); return false;
        }
        CloseHandle(pi_f.hThread);

        // --- ffmpeg stdoutからJPEGを読み取る ---
        jpeg_out.clear();
        jpeg_out.reserve(128*1024);
        std::vector<uint8_t> chunk(32768);
        DWORD nr = 0;
        while (ReadFile(p2r, chunk.data(), (DWORD)chunk.size(), &nr, nullptr) && nr > 0)
            jpeg_out.insert(jpeg_out.end(), chunk.begin(), chunk.begin() + nr);
        CloseHandle(p2r);

        // プロセス後始末
        WaitForSingleObject(pi_f.hProcess, 5000);
        TerminateProcess(pi_a.hProcess, 0);
        WaitForSingleObject(pi_a.hProcess, 2000);
        CloseHandle(pi_f.hProcess);
        CloseHandle(pi_a.hProcess);

        bool ok = jpeg_out.size() > 1000;
        if (!ok) MLOG_WARN("adb_h264", "empty jpeg: %zu bytes from %s",
                            jpeg_out.size(), adb_serial.c_str());
        return ok;
    }
};

} // namespace mirage
