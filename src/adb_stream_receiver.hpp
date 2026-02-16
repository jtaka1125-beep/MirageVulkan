// =============================================================================
// ADB Stream Receiver - Reads H.264 from adb exec-out screenrecord
// =============================================================================
#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <cstdint>
#include <mutex>
#include "mirage_log.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace mirage {

/**
 * Reads raw H.264 stream from `adb exec-out screenrecord --output-format=h264 -`
 * and feeds NAL units to a callback (typically the decoder).
 */
class AdbStreamReceiver {
public:
    using NalCallback = std::function<void(const uint8_t* data, size_t size)>;

    AdbStreamReceiver(const std::string& serial, const std::string& hardware_id,
                      int width = 720, int height = 1280, int bitrate = 2000000)
        : serial_(serial), hardware_id_(hardware_id),
          width_(width), height_(height), bitrate_(bitrate) {}

    ~AdbStreamReceiver() { stop(); }

    void setNalCallback(NalCallback cb) { nal_callback_ = std::move(cb); }

    bool start() {
        if (running_.load()) return true;
        running_ = true;
        thread_ = std::thread(&AdbStreamReceiver::readerLoop, this);
        return true;
    }

    void stop() {
        running_ = false;
#ifdef _WIN32
        if (process_handle_ != INVALID_HANDLE_VALUE) {
            TerminateProcess(process_handle_, 0);
            CloseHandle(process_handle_);
            process_handle_ = INVALID_HANDLE_VALUE;
        }
        if (pipe_read_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_read_);
            pipe_read_ = INVALID_HANDLE_VALUE;
        }
#endif
        if (thread_.joinable()) thread_.join();
    }

    bool isRunning() const { return running_.load(); }
    uint64_t getFrameCount() const { return frame_count_.load(); }
    uint64_t getBytesRead() const { return bytes_read_.load(); }
    const std::string& getHardwareId() const { return hardware_id_; }

private:
    std::string serial_;
    std::string hardware_id_;
    int width_, height_, bitrate_;
    NalCallback nal_callback_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frame_count_{0};
    std::atomic<uint64_t> bytes_read_{0};

#ifdef _WIN32
    HANDLE process_handle_ = INVALID_HANDLE_VALUE;
    HANDLE pipe_read_ = INVALID_HANDLE_VALUE;
#endif

    void readerLoop() {
        MLOG_INFO("adbstream", "Starting adb stream for %s (%s) %dx%d",
                  hardware_id_.c_str(), serial_.c_str(), width_, height_);

        while (running_.load()) {
            if (!startAdbProcess()) {
                MLOG_ERROR("adbstream", "Failed to start adb process for %s", serial_.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(3));
                continue;
            }

            readH264Stream();

            // Clean up process
#ifdef _WIN32
            if (process_handle_ != INVALID_HANDLE_VALUE) {
                TerminateProcess(process_handle_, 0);
                CloseHandle(process_handle_);
                process_handle_ = INVALID_HANDLE_VALUE;
            }
            if (pipe_read_ != INVALID_HANDLE_VALUE) {
                CloseHandle(pipe_read_);
                pipe_read_ = INVALID_HANDLE_VALUE;
            }
#endif
            if (running_.load()) {
                MLOG_WARN("adbstream", "Stream ended for %s, restarting in 2s", serial_.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
        MLOG_INFO("adbstream", "Stream reader ended for %s", hardware_id_.c_str());
    }

    bool startAdbProcess() {
#ifdef _WIN32
        // Build command
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
            "adb -s %s exec-out screenrecord --output-format=h264 --size %dx%d --bit-rate %d -",
            serial_.c_str(), width_, height_, bitrate_);

        MLOG_INFO("adbstream", "CMD: %s", cmd);

        // Create pipe for stdout
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE pipe_write = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&pipe_read_, &pipe_write, &sa, 64 * 1024)) {
            MLOG_ERROR("adbstream", "CreatePipe failed: %lu", GetLastError());
            return false;
        }
        SetHandleInformation(pipe_read_, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = pipe_write;
        si.hStdError = pipe_write;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
        CloseHandle(pipe_write);

        if (!ok) {
            MLOG_ERROR("adbstream", "CreateProcess failed: %lu", GetLastError());
            CloseHandle(pipe_read_);
            pipe_read_ = INVALID_HANDLE_VALUE;
            return false;
        }

        process_handle_ = pi.hProcess;
        CloseHandle(pi.hThread);
        MLOG_INFO("adbstream", "ADB process started (PID %lu) for %s",
                  pi.dwProcessId, serial_.c_str());
        return true;
#else
        return false;
#endif
    }

    void readH264Stream() {
#ifdef _WIN32
        // Read H.264 byte stream and split into NAL units
        // NAL units delimited by 00 00 00 01 or 00 00 01
        std::vector<uint8_t> buffer;
        buffer.reserve(256 * 1024);  // 256KB
        uint8_t read_buf[8192];
        
        auto start_time = std::chrono::steady_clock::now();
        bool first_data = true;

        while (running_.load()) {
            DWORD bytes_available = 0;
            if (!PeekNamedPipe(pipe_read_, NULL, 0, NULL, &bytes_available, NULL)) {
                break;  // Pipe broken
            }
            if (bytes_available == 0) {
                // Check if process still alive
                DWORD exit_code;
                if (GetExitCodeProcess(process_handle_, &exit_code) && exit_code != STILL_ACTIVE) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            DWORD bytes_read = 0;
            DWORD to_read = (bytes_available < sizeof(read_buf)) ? bytes_available : sizeof(read_buf);
            if (!ReadFile(pipe_read_, read_buf, to_read, &bytes_read, NULL) || bytes_read == 0) {
                break;
            }

            if (first_data) {
                MLOG_INFO("adbstream", "First data received from %s (%lu bytes)",
                          serial_.c_str(), bytes_read);
                first_data = false;
            }

            bytes_read_ += bytes_read;

            // Append to buffer
            buffer.insert(buffer.end(), read_buf, read_buf + bytes_read);

            // Extract NAL units
            extractAndDeliverNalUnits(buffer);

            // Stats every 5 seconds
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            if (elapsed >= 5 && frame_count_.load() > 0) {
                double fps = frame_count_.load() * 1.0 / elapsed;
                double mbps = bytes_read_.load() * 8.0 / elapsed / 1000000.0;
                MLOG_INFO("adbstream", "%s: frames=%llu fps=%.1f bitrate=%.1f Mbps",
                          hardware_id_.c_str(), frame_count_.load(), fps, mbps);
                // Reset stats
                start_time = now;
                frame_count_ = 0;
                bytes_read_ = 0;
            }
        }
#endif
    }

    void extractAndDeliverNalUnits(std::vector<uint8_t>& buffer) {
        // Find NAL start codes (00 00 00 01) and split
        size_t pos = 0;
        size_t nal_start = 0;
        bool found_first = false;

        while (pos + 3 < buffer.size()) {
            bool is_start_code = false;
            size_t sc_len = 0;

            if (buffer[pos] == 0 && buffer[pos+1] == 0 && buffer[pos+2] == 0 &&
                pos + 3 < buffer.size() && buffer[pos+3] == 1) {
                is_start_code = true;
                sc_len = 4;
            } else if (buffer[pos] == 0 && buffer[pos+1] == 0 && buffer[pos+2] == 1) {
                is_start_code = true;
                sc_len = 3;
            }

            if (is_start_code) {
                if (found_first && pos > nal_start) {
                    // Deliver previous NAL unit (including its start code)
                    deliverNal(buffer.data() + nal_start, pos - nal_start);
                }
                nal_start = pos;
                found_first = true;
                pos += sc_len;
            } else {
                pos++;
            }
        }

        // Keep unprocessed data in buffer
        if (found_first) {
            buffer.erase(buffer.begin(), buffer.begin() + nal_start);
        }
    }

    void deliverNal(const uint8_t* data, size_t size) {
        if (size < 5) return;  // Too small
        frame_count_++;
        if (nal_callback_) {
            nal_callback_(data, size);
        }
    }
};

} // namespace mirage
