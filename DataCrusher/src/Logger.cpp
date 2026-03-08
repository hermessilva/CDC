// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Logger implementation
// =============================================================================
#include "Logger.h"
#include <filesystem>

namespace DC
{

Logger& Logger::Instance()
{
    static Logger instance;
    return instance;
}

Logger::~Logger()
{
    Flush();
    if (_FileStream.is_open())
        _FileStream.close();
}

void Logger::Initialize(LogLevel level, const std::string& logFilePath)
{
    std::lock_guard lock(_Mutex);
    _Level.store(level, std::memory_order_relaxed);

    if (!logFilePath.empty())
    {
        _LogFilePath = logFilePath;
        _FileStream.open(_LogFilePath, std::ios::app | std::ios::out);
        if (_FileStream.is_open())
        {
            auto fileSize = std::filesystem::file_size(_LogFilePath);
            _CurrentFileSize = static_cast<size_t>(fileSize);
        }
    }
}

void Logger::SetLevel(LogLevel level)
{
    _Level.store(level, std::memory_order_relaxed);
}

void Logger::WriteEntry(LogLevel level, std::string_view message)
{
    auto now = SystemClock::now();
    auto timeT = SystemClock::to_time_t(now);
    auto ms = std::chrono::duration_cast<Milliseconds>(
        now.time_since_epoch()) % 1000;

    struct tm tmBuf;
#if DC_PLATFORM_WINDOWS
    localtime_s(&tmBuf, &timeT);
#else
    localtime_r(&timeT, &tmBuf);
#endif

    char timeBuf[32];
    std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &tmBuf);

    auto entry = std::format("[{}.{:03d}] [{}] {}\n",
        timeBuf,
        static_cast<int>(ms.count()),
        LogLevelToString(level),
        message
    );

    std::lock_guard lock(_Mutex);

    // Always write to stderr for visibility
    std::cerr << entry;

    // Write to file if configured
    if (_FileStream.is_open())
    {
        _FileStream << entry;
        _CurrentFileSize += entry.size();
        RotateIfNeeded();
    }
}

void Logger::Flush()
{
    std::lock_guard lock(_Mutex);
    if (_FileStream.is_open())
        _FileStream.flush();
    std::cerr.flush();
}

void Logger::RotateIfNeeded()
{
    constexpr size_t maxSize = Logging::MaxLogFileSizeMB * 1024 * 1024;
    if (_CurrentFileSize < maxSize)
        return;

    _FileStream.close();

    // Rotate: datacrusher.log -> datacrusher.log.1 -> datacrusher.log.2 ...
    for (int i = Logging::MaxRotatedFiles - 1; i >= 1; --i)
    {
        auto oldPath = std::format("{}.{}", _LogFilePath, i);
        auto newPath = std::format("{}.{}", _LogFilePath, i + 1);
        std::error_code ec;
        std::filesystem::rename(oldPath, newPath, ec);
    }

    {
        auto rotatedPath = std::format("{}.1", _LogFilePath);
        std::error_code ec;
        std::filesystem::rename(_LogFilePath, rotatedPath, ec);
    }

    _FileStream.open(_LogFilePath, std::ios::out | std::ios::trunc);
    _CurrentFileSize = 0;
}

} // namespace DC
