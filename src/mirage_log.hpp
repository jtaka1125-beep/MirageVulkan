// =============================================================================
// MirageSystem - Structured Logging (Async)
// =============================================================================
// Thread-safe, async logging via lock-free queue + background writer thread.
// Main thread is NEVER blocked by fprintf(stderr) or file I/O.
// Usage: MLOG_INFO("tag", "message %s", arg);
// =============================================================================
#pragma once
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <mutex>
#include <thread>
#include <atomic>
#include <string>
#include <condition_variable>
#include <deque>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace mirage::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Fatal };

inline std::atomic<Level> g_min_level{Level::Info};

// Keep g_log_mutex for openLogFile/closeLogFile (rare ops)
inline std::mutex g_log_mutex;
inline FILE* g_log_file = nullptr;

inline const char* levelStr(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO ";
        case Level::Warn:  return "WARN ";
        case Level::Error: return "ERROR";
        case Level::Fatal: return "FATAL";
    }
    return "?????";
}

// ---------------------------------------------------------------------------
// Async writer: background thread drains a deque, no caller is blocked
// ---------------------------------------------------------------------------
struct LogEntry {
    std::string line;
    bool to_file;
};

struct AsyncLogger {
    std::deque<LogEntry>     queue_;
    std::mutex               q_mutex_;
    std::condition_variable  q_cv_;
    std::thread              worker_;
    std::atomic<bool>        running_{false};

    void start() {
        running_.store(true);
        worker_ = std::thread([this]() {
            while (true) {
                std::unique_lock<std::mutex> lk(q_mutex_);
                q_cv_.wait(lk, [this]{ return !queue_.empty() || !running_.load(); });
                std::deque<LogEntry> local;
                local.swap(queue_);
                lk.unlock();

                for (auto& e : local) {
                    fputs(e.line.c_str(), stderr);
                    if (e.to_file) {
                        std::lock_guard<std::mutex> fl(g_log_mutex);
                        if (g_log_file) fputs(e.line.c_str(), g_log_file);
                    }
                }
                // Flush once per batch (cheaper than per-line)
                fflush(stderr);
                {
                    std::lock_guard<std::mutex> fl(g_log_mutex);
                    if (g_log_file) fflush(g_log_file);
                }

                if (!running_.load()) break;
            }
        });
    }

    void stop() {
        running_.store(false);
        q_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    void push(std::string line, bool to_file) {
        {
            std::lock_guard<std::mutex> lk(q_mutex_);
            // Cap queue at 4096 entries to prevent unbounded growth
            if (queue_.size() < 4096) {
                queue_.push_back({std::move(line), to_file});
            }
        }
        q_cv_.notify_one();
    }

    ~AsyncLogger() { stop(); }
};

inline AsyncLogger& getLogger() {
    static AsyncLogger inst;
    return inst;
}

// Called once at startup (from gui_main or equivalent)
inline void startAsyncLogger() {
    getLogger().start();
}

inline void setLogLevel(Level l) { g_min_level = l; }

inline bool openLogFile(const char* path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) fclose(g_log_file);
    g_log_file = fopen(path, "w");
    return g_log_file != nullptr;
}

inline void closeLogFile() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; }
}

inline void write(Level level, const char* tag, const char* fmt, ...) {
    if (level < g_min_level.load(std::memory_order_relaxed)) return;

    // Format timestamp + message on caller thread (cheap, no I/O)
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_s(&tm_buf, &time_t_now);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d.%03d",
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec, (int)ms.count());
    DWORD tid = GetCurrentThreadId();
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    // Build line string (no I/O yet)
    char line[2200];
    snprintf(line, sizeof(line), "%s [%s] [%s] (T%lu) %s\n",
             time_str, levelStr(level), tag, (unsigned long)tid, msg);

    bool to_file;
    {
        std::lock_guard<std::mutex> fl(g_log_mutex);
        to_file = (g_log_file != nullptr);
    }

    // Push to async queue — caller returns immediately
    getLogger().push(line, to_file);
}

} // namespace mirage::log

#define MLOG_TRACE(tag, fmt, ...) mirage::log::write(mirage::log::Level::Trace, tag, fmt, ##__VA_ARGS__)
#define MLOG_DEBUG(tag, fmt, ...) mirage::log::write(mirage::log::Level::Debug, tag, fmt, ##__VA_ARGS__)
#define MLOG_INFO(tag, fmt, ...)  mirage::log::write(mirage::log::Level::Info,  tag, fmt, ##__VA_ARGS__)
#define MLOG_WARN(tag, fmt, ...)  mirage::log::write(mirage::log::Level::Warn,  tag, fmt, ##__VA_ARGS__)
#define MLOG_ERROR(tag, fmt, ...) mirage::log::write(mirage::log::Level::Error, tag, fmt, ##__VA_ARGS__)
#define MLOG_FATAL(tag, fmt, ...) mirage::log::write(mirage::log::Level::Fatal, tag, fmt, ##__VA_ARGS__)
