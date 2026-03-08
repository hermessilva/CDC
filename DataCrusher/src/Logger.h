// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Structured logging with latency tracking
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <fstream>
#include <mutex>
#include <iostream>
#include <format>
#include <chrono>
#include <source_location>

namespace DC
{

enum class LogLevel : uint8_t
{
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
    Off   = 6
};

inline std::string_view LogLevelToString(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        default:              return "UNKN ";
    }
}

inline LogLevel ParseLogLevel(std::string_view str)
{
    if (str == "trace" || str == "TRACE") return LogLevel::Trace;
    if (str == "debug" || str == "DEBUG") return LogLevel::Debug;
    if (str == "info"  || str == "INFO")  return LogLevel::Info;
    if (str == "warn"  || str == "WARN")  return LogLevel::Warn;
    if (str == "error" || str == "ERROR") return LogLevel::Error;
    if (str == "fatal" || str == "FATAL") return LogLevel::Fatal;
    if (str == "off"   || str == "OFF")   return LogLevel::Off;
    return LogLevel::Info;
}

class Logger final
{
public:
    static Logger& Instance();

    void Initialize(LogLevel level, const std::string& logFilePath = "");
    void SetLevel(LogLevel level);
    LogLevel GetLevel() const { return _Level.load(std::memory_order_relaxed); }

    template<typename... Args>
    void Log(LogLevel level, std::format_string<Args...> fmt, Args&&... args)
    {
        if (level < _Level.load(std::memory_order_relaxed))
            return;
        auto msg = std::format(fmt, std::forward<Args>(args)...);
        WriteEntry(level, msg);
    }

    template<typename... Args>
    void Trace(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Trace, fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    void Debug(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Debug, fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    void Info(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Info, fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    void Warn(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Warn, fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    void Error(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Error, fmt, std::forward<Args>(args)...); }

    template<typename... Args>
    void Fatal(std::format_string<Args...> fmt, Args&&... args) { Log(LogLevel::Fatal, fmt, std::forward<Args>(args)...); }

    void Flush();

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void WriteEntry(LogLevel level, std::string_view message);
    void RotateIfNeeded();

    std::atomic<LogLevel> _Level{LogLevel::Info};
    std::mutex            _Mutex;
    std::ofstream         _FileStream;
    std::string           _LogFilePath;
    size_t                _CurrentFileSize = 0;
};

// Convenience macros
#define LOG_TRACE(...) DC::Logger::Instance().Trace(__VA_ARGS__)
#define LOG_DEBUG(...) DC::Logger::Instance().Debug(__VA_ARGS__)
#define LOG_INFO(...)  DC::Logger::Instance().Info(__VA_ARGS__)
#define LOG_WARN(...)  DC::Logger::Instance().Warn(__VA_ARGS__)
#define LOG_ERROR(...) DC::Logger::Instance().Error(__VA_ARGS__)
#define LOG_FATAL(...) DC::Logger::Instance().Fatal(__VA_ARGS__)

} // namespace DC
