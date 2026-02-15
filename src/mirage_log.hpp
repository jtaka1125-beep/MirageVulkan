// =============================================================================
// MirageSystem - Structured Logging
// =============================================================================
// Thread-safe, level-filtered logging with optional file output.
// Usage: MLOG_INFO("tag", "message %s", arg);
// =============================================================================
#pragma once
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <mutex>
#include <string>
#include <atomic>

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

inline void setLogLevel(Level l) { g_min_level = l; }

inline bool openLogFile(const char* path) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) fclose(g_log_file);
    g_log_file = fopen(path, "w");  // overwrite mode: prevents old process log contamination
    return g_log_file != nullptr;
}

inline void closeLogFile() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; }
}

inline void write(Level level, const char* tag, const char* fmt, ...) {
    if (level < g_min_level.load(std::memory_order_relaxed)) return;
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
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stderr, "%s [%s] [%s] (T%lu) %s\n", time_str, levelStr(level), tag, tid, msg);
    if (g_log_file) {
        fprintf(g_log_file, "%s [%s] [%s] (T%lu) %s\n",
                time_str, levelStr(level), tag, tid, msg);
        fflush(g_log_file);
    }
}

} // namespace mirage::log

#define MLOG_TRACE(tag, fmt, ...) mirage::log::write(mirage::log::Level::Trace, tag, fmt, ##__VA_ARGS__)
#define MLOG_DEBUG(tag, fmt, ...) mirage::log::write(mirage::log::Level::Debug, tag, fmt, ##__VA_ARGS__)
#define MLOG_INFO(tag, fmt, ...)  mirage::log::write(mirage::log::Level::Info,  tag, fmt, ##__VA_ARGS__)
#define MLOG_WARN(tag, fmt, ...)  mirage::log::write(mirage::log::Level::Warn,  tag, fmt, ##__VA_ARGS__)
#define MLOG_ERROR(tag, fmt, ...) mirage::log::write(mirage::log::Level::Error, tag, fmt, ##__VA_ARGS__)
#define MLOG_FATAL(tag, fmt, ...) mirage::log::write(mirage::log::Level::Fatal, tag, fmt, ##__VA_ARGS__)
