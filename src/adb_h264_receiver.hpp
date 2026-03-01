// =============================================================================
// AdbH264Receiver v9 - H264ストリーミング方式 (デッドロック修正版)
// デッドロック修正: killEntry内でreader.join()をmap_mtx_の外で行う
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
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
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
        { std::lock_guard<std::mutex> lk(mgr_mtx_); adb_mgr_ = mgr; }
        sync_now_.store(true);
    }
    bool has_manager() const { return adb_mgr_ != nullptr; }

    bool start() {
        if (running_.exchange(true)) return true;
        supervisor_ = std::thread(&AdbH264Receiver::supervisorLoop, this);
        MLOG_INFO("adb_h264", "v9: H264 streaming started");
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        sync_now_.store(false);
        if (supervisor_.joinable()) supervisor_.join();
        // 残ったストリームをすべて停止
        std::vector<std::thread> to_join;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            for (auto& kv : streams_) {
                closeHandles(kv.second);
                if (kv.second.reader.joinable())
                    to_join.push_back(std::move(kv.second.reader));
            }
            streams_.clear();
        }
        for (auto& t : to_join) t.join();
    }

    bool running()      const { return running_.load(); }
    int  device_count() const {
        std::lock_guard<std::mutex> lk(map_mtx_);
        return (int)streams_.size();
    }

    bool get_latest_jpeg(const std::string& hw_or_adb,
                         std::vector<uint8_t>& out, int& out_w, int& out_h) {
        std::string hw_id, adb_serial;
        resolveIds(hw_or_adb, hw_id, adb_serial, out_w, out_h);
        if (adb_serial.empty()) return false;

        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            auto it = streams_.find(hw_id);
            if (it != streams_.end() && !it->second.jpeg.empty()) {
                out = it->second.jpeg;
                return true;
            }
        }
        // フォールバック: on-demand screencap
        MLOG_INFO("adb_h264", "cache miss %s -> fallback", hw_id.c_str());
        return captureOneFrame(adb_serial, out);
    }

    float get_fps(const std::string& hw_or_adb) {
        std::string hw_id, adb_serial; int w=0,h=0;
        resolveIds(hw_or_adb, hw_id, adb_serial, w, h);
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = streams_.find(hw_id);
        return (it != streams_.end()) ? it->second.fps : 0.f;
    }

private:
    struct StreamEntry {
        std::string  hw_id;
        std::string  adb_serial;
        HANDLE       proc_adb  = INVALID_HANDLE_VALUE;
        HANDLE       proc_ff   = INVALID_HANDLE_VALUE;
        HANDLE       ff_stdout = INVALID_HANDLE_VALUE;
        std::thread  reader;
        std::vector<uint8_t> jpeg;
        float        fps    = 0.f;
        uint64_t     frames = 0;
        std::chrono::steady_clock::time_point t_start;
        StreamEntry() = default;
        StreamEntry(const StreamEntry&) = delete;
        StreamEntry& operator=(const StreamEntry&) = delete;
        StreamEntry(StreamEntry&&) = default;
        StreamEntry& operator=(StreamEntry&&) = default;
    };

    std::string  adb_path_, ffmpeg_path_;
    ::gui::AdbDeviceManager* adb_mgr_ = nullptr;
    mutable std::mutex mgr_mtx_;
    std::atomic<bool>  running_{false};
    std::atomic<bool>  sync_now_{false};
    std::thread        supervisor_;
    mutable std::mutex map_mtx_;
    std::map<std::string, StreamEntry> streams_;

    std::string getAdb()    const { return adb_path_.empty()    ? "adb"                             : adb_path_; }
    std::string getFfmpeg() const { return ffmpeg_path_.empty() ? "C:/msys64/mingw64/bin/ffmpeg.exe": ffmpeg_path_; }

    void resolveIds(const std::string& in,
                    std::string& hw_id, std::string& adb_serial,
                    int& out_w, int& out_h) const {
        std::lock_guard<std::mutex> lk(mgr_mtx_);
        if (!adb_mgr_) return;
        for (auto& ud : adb_mgr_->getUniqueDevices()) {
            if (ud.hardware_id == in || ud.preferred_adb_id == in) {
                hw_id = ud.hardware_id; adb_serial = ud.preferred_adb_id;
                out_w = ud.screen_width  > 0 ? ud.screen_width  : 1080;
                out_h = ud.screen_height > 0 ? ud.screen_height : 1920;
                return;
            }
        }
    }

    // ハンドルだけ閉じる (reader.join()はしない → map_mtx_外で行う)
    void closeHandles(StreamEntry& e) {
        if (e.ff_stdout != INVALID_HANDLE_VALUE) {
            CloseHandle(e.ff_stdout); e.ff_stdout = INVALID_HANDLE_VALUE;
        }
        if (e.proc_ff != INVALID_HANDLE_VALUE) {
            TerminateProcess(e.proc_ff, 0);
            WaitForSingleObject(e.proc_ff, 2000);
            CloseHandle(e.proc_ff); e.proc_ff = INVALID_HANDLE_VALUE;
        }
        if (e.proc_adb != INVALID_HANDLE_VALUE) {
            TerminateProcess(e.proc_adb, 0);
            WaitForSingleObject(e.proc_adb, 2000);
            CloseHandle(e.proc_adb); e.proc_adb = INVALID_HANDLE_VALUE;
        }
    }

    void supervisorLoop() {
        for (int i = 0; i < 50 && running_.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (adb_mgr_) break;
        }
        while (running_.load()) {
            syncDevices();
            for (int i = 0; i < 150 && running_.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (sync_now_.exchange(false)) break;
            }
        }
    }

    void syncDevices() {
        std::vector<std::string> new_hw_ids;
        std::vector<std::thread> dead_threads;  // map_mtx_の外でjoinするために退避

        std::vector<::gui::AdbDeviceManager::UniqueDevice> devs;
        {
            std::lock_guard<std::mutex> mgr_lk(mgr_mtx_);
            if (!adb_mgr_) return;
            devs = adb_mgr_->getUniqueDevices();
        }
        MLOG_INFO("adb_h264", "syncDevices: %d devs", (int)devs.size());

        {
            std::lock_guard<std::mutex> map_lk(map_mtx_);

            // 死亡エントリを検出してクリーンアップ
            for (auto it = streams_.begin(); it != streams_.end(); ) {
                DWORD code = 0;
                bool dead = (it->second.proc_adb == INVALID_HANDLE_VALUE ||
                             !GetExitCodeProcess(it->second.proc_adb, &code) ||
                             code != STILL_ACTIVE);
                if (dead) {
                    MLOG_WARN("adb_h264", "dead stream: %s", it->first.c_str());
                    closeHandles(it->second);  // ハンドル閉じる
                    if (it->second.reader.joinable())
                        dead_threads.push_back(std::move(it->second.reader));
                    it = streams_.erase(it);
                } else {
                    ++it;
                }
            }

            // 削除されたデバイスをクリーンアップ
            for (auto it = streams_.begin(); it != streams_.end(); ) {
                bool found = false;
                for (auto& ud : devs)
                    if (ud.hardware_id == it->first) { found=true; break; }
                if (!found) {
                    closeHandles(it->second);
                    if (it->second.reader.joinable())
                        dead_threads.push_back(std::move(it->second.reader));
                    it = streams_.erase(it);
                } else { ++it; }
            }

            // 新規デバイスを起動
            for (auto& ud : devs) {
                if (ud.preferred_adb_id.empty()) continue;
                if (streams_.find(ud.hardware_id) == streams_.end()) {
                    if (startStream(ud))
                        new_hw_ids.push_back(ud.hardware_id);
                }
            }
        }  // map_lk解放

        // 死亡スレッドをjoin (map_mtx_の外)
        for (auto& t : dead_threads) t.join();

        // readerスレッド起動 (map_mtx_の外)
        for (auto& hw_id : new_hw_ids) {
            std::lock_guard<std::mutex> lk(map_mtx_);
            auto it = streams_.find(hw_id);
            if (it != streams_.end() && !it->second.reader.joinable()) {
                it->second.t_start = std::chrono::steady_clock::now();
                std::string id = hw_id;
                it->second.reader = std::thread([this, id]{ readerLoop(id); });
            }
        }
    }

    // map_mtx_保持中に呼ぶ
    bool startStream(const ::gui::AdbDeviceManager::UniqueDevice& ud) {
        int w = ud.screen_width  > 0 ? ud.screen_width  : 1080;
        int h = ud.screen_height > 0 ? ud.screen_height : 1920;
        w = ((w+15)/16)*16; h = ((h+15)/16)*16;
        // screenrecordの最大サイズは1920
        if (w > 1920 || h > 1920) { w = w/2; h = h/2; }
        std::string sz = std::to_string(w) + "x" + std::to_string(h);

        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;

        HANDLE p1r,p1w;
        if (!CreatePipe(&p1r,&p1w,&sa,2*1024*1024)) return false;
        SetHandleInformation(p1r, HANDLE_FLAG_INHERIT, 0);  // 非継承 (C++用)

        HANDLE p2r,p2w;
        if (!CreatePipe(&p2r,&p2w,&sa,512*1024)) {
            CloseHandle(p1r); CloseHandle(p1w); return false;
        }
        SetHandleInformation(p2r, HANDLE_FLAG_INHERIT, 0);  // 非継承 (C++用)

        // adb起動 (p1wは継承ON = CreatePipe時のsa.bInheritHandle=TRUE)
        std::string adb_cmd = "\"" + getAdb() + "\" -s " + ud.preferred_adb_id +
            " exec-out screenrecord --output-format=h264"
            " --bit-rate 4M --size " + sz + " --time-limit 0 -";
        STARTUPINFOA si_a{}; si_a.cb=sizeof(si_a);
        si_a.dwFlags    = STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
        si_a.hStdOutput = p1w;
        si_a.hStdError  = nullptr;
        si_a.wShowWindow= SW_HIDE;
        PROCESS_INFORMATION pi_a{};
        if (!CreateProcessA(nullptr,adb_cmd.data(),nullptr,nullptr,
                            TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si_a,&pi_a)) {
            MLOG_ERROR("adb_h264","adb CreateProcess failed: %lu",GetLastError());
            CloseHandle(p1r); CloseHandle(p1w); CloseHandle(p2r); CloseHandle(p2w);
            return false;
        }
        CloseHandle(pi_a.hThread);
        CloseHandle(p1w);  // adbが持つ

        // ffmpeg起動前にp1rを継承ON
        SetHandleInformation(p1r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        // p2wはsa.bInheritHandle=TRUEで既に継承ON

        std::string ff_cmd = "\"" + getFfmpeg() + "\""
            " -loglevel error"
            " -f h264 -i pipe:0"
            " -vf fps=15,format=yuvj420p"
            " -q:v 3"
            " -f mjpeg pipe:1";
        STARTUPINFOA si_f{}; si_f.cb=sizeof(si_f);
        si_f.dwFlags    = STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
        si_f.hStdInput  = p1r;
        si_f.hStdOutput = p2w;
        si_f.hStdError  = nullptr;
        si_f.wShowWindow= SW_HIDE;
        PROCESS_INFORMATION pi_f{};
        if (!CreateProcessA(nullptr,ff_cmd.data(),nullptr,nullptr,
                            TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si_f,&pi_f)) {
            MLOG_ERROR("adb_h264","ffmpeg CreateProcess failed: %lu",GetLastError());
            TerminateProcess(pi_a.hProcess,0); WaitForSingleObject(pi_a.hProcess,1000);
            CloseHandle(pi_a.hProcess);
            CloseHandle(p1r); CloseHandle(p2r); CloseHandle(p2w);
            return false;
        }
        CloseHandle(pi_f.hThread);
        CloseHandle(p1r);  // ffmpegが持つ
        CloseHandle(p2w);  // ffmpegが持つ

        StreamEntry e;
        e.hw_id      = ud.hardware_id;
        e.adb_serial = ud.preferred_adb_id;
        e.proc_adb   = pi_a.hProcess;
        e.proc_ff    = pi_f.hProcess;
        e.ff_stdout  = p2r;
        MLOG_INFO("adb_h264","stream started: %s %s adb=%lu ff=%lu",
                  ud.preferred_adb_id.c_str(), sz.c_str(),
                  pi_a.dwProcessId, pi_f.dwProcessId);
        streams_.emplace(ud.hardware_id, std::move(e));
        return true;
    }

    void readerLoop(const std::string& hw_id) {
        std::vector<uint8_t> buf;
        buf.reserve(512*1024);
        std::vector<uint8_t> chunk(32768);

        while (running_.load()) {
            HANDLE hRead;
            {
                std::lock_guard<std::mutex> lk(map_mtx_);
                auto it = streams_.find(hw_id);
                if (it==streams_.end() || it->second.ff_stdout==INVALID_HANDLE_VALUE) break;
                hRead = it->second.ff_stdout;
            }

            DWORD nr=0;
            if (!ReadFile(hRead,chunk.data(),(DWORD)chunk.size(),&nr,nullptr)||nr==0) break;
            buf.insert(buf.end(),chunk.begin(),chunk.begin()+nr);

            // JPEG切り出し
            size_t pos=0;
            while (pos+4<=buf.size()) {
                if (!(buf[pos]==0xFF&&buf[pos+1]==0xD8)) { pos++; continue; }
                size_t eoi=std::string::npos;
                for (size_t j=pos+2; j+1<buf.size(); j++)
                    if (buf[j]==0xFF&&buf[j+1]==0xD9) { eoi=j+2; break; }
                if (eoi==std::string::npos) break;

                {
                    std::lock_guard<std::mutex> lk(map_mtx_);
                    auto it=streams_.find(hw_id);
                    if (it!=streams_.end()) {
                        it->second.jpeg.assign(buf.begin()+pos,buf.begin()+eoi);
                        it->second.frames++;
                        float el=std::chrono::duration<float>(
                            std::chrono::steady_clock::now()-it->second.t_start).count();
                        if (el>0.5f) it->second.fps=it->second.frames/el;
                    }
                }
                pos=eoi;
            }
            if (pos>0) buf.erase(buf.begin(),buf.begin()+pos);
            if (buf.size()>8*1024*1024) { buf.clear(); }
        }
        MLOG_INFO("adb_h264","reader exited: %s",hw_id.c_str());
    }

    // フォールバック: screencap on-demand
    bool captureOneFrame(const std::string& adb_serial, std::vector<uint8_t>& jpeg_out) {
        SECURITY_ATTRIBUTES sa{}; sa.nLength=sizeof(sa); sa.bInheritHandle=FALSE;
        HANDLE p1r,p1w,p2r,p2w;
        if (!CreatePipe(&p1r,&p1w,&sa,2*1024*1024)) return false;
        if (!CreatePipe(&p2r,&p2w,&sa,512*1024)) { CloseHandle(p1r);CloseHandle(p1w);return false; }

        SetHandleInformation(p1w,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
        std::string acmd="\""+getAdb()+"\" -s "+adb_serial+" exec-out screencap -p";
        STARTUPINFOA si_a{}; si_a.cb=sizeof(si_a);
        si_a.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
        si_a.hStdOutput=p1w; si_a.hStdError=nullptr; si_a.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION pi_a{};
        bool aok=CreateProcessA(nullptr,acmd.data(),nullptr,nullptr,
                                TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si_a,&pi_a);
        SetHandleInformation(p1w,HANDLE_FLAG_INHERIT,0);
        CloseHandle(p1w);
        if (!aok) { CloseHandle(p1r);CloseHandle(p2r);CloseHandle(p2w);return false; }
        CloseHandle(pi_a.hThread);

        SetHandleInformation(p1r,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
        SetHandleInformation(p2w,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT);
        std::string fcmd="\""+getFfmpeg()+"\" -loglevel error -i pipe:0"
                         " -vframes 1 -f image2 -vcodec mjpeg -q:v 5 pipe:1";
        STARTUPINFOA si_f{}; si_f.cb=sizeof(si_f);
        si_f.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
        si_f.hStdInput=p1r; si_f.hStdOutput=p2w; si_f.hStdError=nullptr; si_f.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION pi_f{};
        bool fok=CreateProcessA(nullptr,fcmd.data(),nullptr,nullptr,
                                TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si_f,&pi_f);
        SetHandleInformation(p1r,HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(p2w,HANDLE_FLAG_INHERIT,0);
        CloseHandle(p1r); CloseHandle(p2w);
        if (!fok) {
            TerminateProcess(pi_a.hProcess,0);WaitForSingleObject(pi_a.hProcess,1000);
            CloseHandle(pi_a.hProcess);CloseHandle(p2r);return false;
        }
        CloseHandle(pi_f.hThread);

        jpeg_out.clear(); jpeg_out.reserve(256*1024);
        std::vector<uint8_t> chunk(32768); DWORD nr=0;
        while (ReadFile(p2r,chunk.data(),(DWORD)chunk.size(),&nr,nullptr)&&nr>0)
            jpeg_out.insert(jpeg_out.end(),chunk.begin(),chunk.begin()+nr);
        CloseHandle(p2r);

        WaitForSingleObject(pi_f.hProcess,5000);
        TerminateProcess(pi_a.hProcess,0);WaitForSingleObject(pi_a.hProcess,1000);
        CloseHandle(pi_f.hProcess);CloseHandle(pi_a.hProcess);
        return jpeg_out.size()>1000;
    }
};

} // namespace mirage
