// =============================================================================
// MirageSystem - ADB Touch Fallback Implementation
// =============================================================================
#include "adb_touch_fallback.hpp"
#include "mirage_log.hpp"
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace mirage {

// ISSUE-5: constructor starts async worker thread
AdbTouchFallback::AdbTouchFallback() {
    async_running_.store(true);
    async_worker_ = std::thread(&AdbTouchFallback::async_worker_loop, this);
}

AdbTouchFallback::~AdbTouchFallback() {
    // Stop async worker before stopping shell
    {
        async_running_.store(false);
        async_queue_cv_.notify_all();
        if (async_worker_.joinable()) async_worker_.join();
    }
    stop_shell();
}

namespace {
// Execute command without showing console window (Windows)
// Returns exit code (0 = success)
int execHidden(const std::string& cmd) {
#ifdef _WIN32
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd;

    if (!CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return -1;
    }

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exit_code);
#else
    return std::system(cmd.c_str());
#endif
}

// execHiddenAsync removed — ISSUE-5: replaced by AdbTouchFallback::enqueue_async()
} // namespace

std::string AdbTouchFallback::adb_prefix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_serial_.empty()) {
        return "adb";
    }
    return "adb -s " + device_serial_;
}


#ifdef _WIN32
static HANDLE asHandle(void* p) { return reinterpret_cast<HANDLE>(p); }
static void* asVoid(HANDLE h) { return reinterpret_cast<void*>(h); }
#endif

bool AdbTouchFallback::start_shell_if_needed() {
#ifndef _WIN32
    return false;
#else
    std::lock_guard<std::mutex> lock(mutex_);
    if (!persistent_shell_.load()) return false;

    std::string dev = device_serial_;

    if (shell_running_ && shell_process_ && shell_stdin_w_ && dev == shell_device_) {
        return true;
    }

    stop_shell();

    std::string cmd = "adb";
    if (!dev.empty()) cmd += " -s " + dev;
    cmd += " shell";

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdin_r = NULL;
    HANDLE child_stdin_w = NULL;
    if (!CreatePipe(&child_stdin_r, &child_stdin_w, &sa, 0)) {
        return false;
    }
    SetHandleInformation(child_stdin_w, HANDLE_FLAG_INHERIT, 0);

    HANDLE nul = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = child_stdin_r;
    si.hStdOutput = nul;
    si.hStdError = nul;

    PROCESS_INFORMATION pi = {};
    std::string cmd_copy = cmd;

    BOOL ok = CreateProcessA(nullptr, cmd_copy.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(child_stdin_r);
    CloseHandle(nul);

    if (!ok) {
        CloseHandle(child_stdin_w);
        return false;
    }

    shell_process_ = asVoid(pi.hProcess);
    shell_thread_  = asVoid(pi.hThread);
    shell_stdin_w_ = asVoid(child_stdin_w);
    shell_running_ = true;
    shell_device_  = dev;

    MLOG_INFO("adb_touch", "Persistent adb shell started for %s", dev.empty() ? "<default>" : dev.c_str());
    return true;
#endif
}

void AdbTouchFallback::stop_shell() {
#ifndef _WIN32
    return;
#else
    if (!shell_running_) return;

    if (shell_stdin_w_) {
        DWORD written = 0;
        const char* exit_cmd = "exit\n";
        WriteFile(asHandle(shell_stdin_w_), exit_cmd, (DWORD)strlen(exit_cmd), &written, nullptr);
        FlushFileBuffers(asHandle(shell_stdin_w_));
        CloseHandle(asHandle(shell_stdin_w_));
        shell_stdin_w_ = nullptr;
    }

    if (shell_process_) {
        WaitForSingleObject(asHandle(shell_process_), 200);
        DWORD code = 0;
        if (GetExitCodeProcess(asHandle(shell_process_), &code) && code == STILL_ACTIVE) {
            TerminateProcess(asHandle(shell_process_), 0);
        }
        CloseHandle(asHandle(shell_process_));
        shell_process_ = nullptr;
    }

    if (shell_thread_) {
        CloseHandle(asHandle(shell_thread_));
        shell_thread_ = nullptr;
    }

    shell_running_ = false;
    shell_device_.clear();
#endif
}

bool AdbTouchFallback::write_shell_line(const std::string& line) {
#ifndef _WIN32
    (void)line;
    return false;
#else
    if (!start_shell_if_needed()) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!shell_running_ || !shell_stdin_w_) return false;

    std::string s = line;
    if (s.empty() || s.back() != '\n') s.push_back('\n');

    DWORD written = 0;
    BOOL ok = WriteFile(asHandle(shell_stdin_w_), s.data(), (DWORD)s.size(), &written, nullptr);
    if (!ok) {
        stop_shell();
        return false;
    }
    FlushFileBuffers(asHandle(shell_stdin_w_));
    return (written == (DWORD)s.size());
#endif
}

bool AdbTouchFallback::exec_adb_sync(const std::string& args) {
    if (!enabled_.load()) return false;
    // Fast path: persistent adb shell for `shell ...` commands
    if (persistent_shell_.load() && args.rfind("shell ", 0) == 0) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = write_shell_line(args.substr(6));
        auto t1 = std::chrono::steady_clock::now();
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        last_latency_ms_.store(ms);
        if (ok) {
            MLOG_DEBUG("adb_touch", "Shell OK (%dms): %s", ms, args.c_str());
            return true;
        }
        // Fallback to spawning adb process
    }

    auto t0 = std::chrono::steady_clock::now();

    std::string cmd = adb_prefix() + " " + args;
    int ret = execHidden(cmd);

    auto t1 = std::chrono::steady_clock::now();
    int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    last_latency_ms_.store(ms);

    if (ret != 0) {
        MLOG_ERROR("adb_touch", "Sync command failed (ret=%d, %dms): %s", ret, ms, args.c_str());
        return false;
    }

    MLOG_DEBUG("adb_touch", "Sync OK (%dms): %s", ms, args.c_str());
    return true;
}

bool AdbTouchFallback::exec_adb_async(const std::string& args) {
    if (!enabled_.load()) return false;
    std::string cmd = adb_prefix() + " " + args;
    enqueue_async(cmd);  // ISSUE-5: queue instead of detach
    return true;
}

// ISSUE-5: bounded async queue enqueuer
void AdbTouchFallback::enqueue_async(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(async_queue_mutex_);
    if (async_queue_.size() >= ASYNC_QUEUE_MAX) {
        async_queue_.pop_front();
        MLOG_WARN("adb_touch", "Async queue overflow, dropped oldest command");
    }
    async_queue_.push_back({cmd});
    async_queue_cv_.notify_one();
}

// ISSUE-5: single worker thread that drains async_queue_
void AdbTouchFallback::async_worker_loop() {
    while (async_running_.load()) {
        std::unique_lock<std::mutex> lk(async_queue_mutex_);
        async_queue_cv_.wait(lk, [this]{
            return !async_queue_.empty() || !async_running_.load();
        });
        if (!async_running_.load() && async_queue_.empty()) break;
        if (async_queue_.empty()) continue;
        auto task = std::move(async_queue_.front());
        async_queue_.pop_front();
        lk.unlock();
        auto t0 = std::chrono::steady_clock::now();
        int ret = execHidden(task.cmd);
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        last_latency_ms_.store(ms);
        if (ret != 0)
            MLOG_ERROR("adb_touch", "Async failed (ret=%d, %dms): %s", ret, ms, task.cmd.c_str());
        else
            MLOG_DEBUG("adb_touch", "Async OK (%dms): %s", ms, task.cmd.c_str());
    }
}

bool AdbTouchFallback::tap(int x, int y) {
    std::ostringstream oss;
    oss << "shell input tap " << x << " " << y;
    return exec_adb_async(oss.str());
}

bool AdbTouchFallback::swipe(int x1, int y1, int x2, int y2, int duration_ms) {
    std::ostringstream oss;
    oss << "shell input swipe " << x1 << " " << y1 << " " << x2 << " " << y2 << " " << duration_ms;
    return exec_adb_async(oss.str());
}

bool AdbTouchFallback::long_press(int x, int y, int hold_ms) {
    // Long press = swipe from same point to same point with duration
    return swipe(x, y, x, y, hold_ms);
}

bool AdbTouchFallback::key(int keycode) {
    std::ostringstream oss;
    oss << "shell input keyevent " << keycode;
    return exec_adb_async(oss.str());
}

bool AdbTouchFallback::back() {
    return key(4); // KEYCODE_BACK = 4
}

} // namespace mirage
