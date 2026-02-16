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

// Execute command async without showing console window (Windows)
void execHiddenAsync(const std::string& cmd, std::atomic<int>* latency_out) {
    std::thread([cmd, latency_out]() {
        auto t0 = std::chrono::steady_clock::now();
        int ret = execHidden(cmd);
        auto t1 = std::chrono::steady_clock::now();
        int ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (latency_out) latency_out->store(ms);
        if (ret != 0) {
            MLOG_ERROR("adb_touch", "Async command failed (ret=%d, %dms): %s", ret, ms, cmd.c_str());
        } else {
            MLOG_DEBUG("adb_touch", "Async OK (%dms): %s", ms, cmd.c_str());
        }
    }).detach();
}
} // namespace

std::string AdbTouchFallback::adb_prefix() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (device_serial_.empty()) {
        return "adb";
    }
    return "adb -s " + device_serial_;
}

bool AdbTouchFallback::exec_adb_sync(const std::string& args) {
    if (!enabled_.load()) return false;

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
    execHiddenAsync(cmd, &last_latency_ms_);

    return true; // Optimistically return success
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
