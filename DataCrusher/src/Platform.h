// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Platform abstraction layer
// =============================================================================
#pragma once

// Standard integer types MUST be included first - before any platform
// or third-party headers (PostgreSQL, Windows) to avoid redefinition conflicts
#include <cstdint>
#include <cstddef>

#ifdef _WIN32
    #define DC_PLATFORM_WINDOWS 1
    #define DC_PLATFORM_LINUX   0
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>

    #include <intrin.h>  // MSVC intrinsics (_byteswap_uint64, etc.)

    #define DC_EXPORT __declspec(dllexport)
    #define DC_IMPORT __declspec(dllimport)
    #define DC_LIKELY(x)   (x)
    #define DC_UNLIKELY(x) (x)
    #define DC_FORCEINLINE __forceinline

    using SocketHandle = SOCKET;
    constexpr SocketHandle DC_INVALID_SOCKET = INVALID_SOCKET;

    inline void DC_SleepMS(uint32_t ms) { ::Sleep(ms); }

    inline int64_t DC_HighResTimerFreq()
    {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        return freq.QuadPart;
    }

    inline int64_t DC_HighResTimerNow()
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart;
    }

    // Portable byte-swap (network ↔ host byte order)
    inline uint64_t DC_BSwap64(uint64_t val) { return _byteswap_uint64(val); }
    inline uint32_t DC_BSwap32(uint32_t val) { return _byteswap_ulong(val); }
    inline uint16_t DC_BSwap16(uint16_t val) { return _byteswap_ushort(val); }

#else
    #define DC_PLATFORM_WINDOWS 0
    #define DC_PLATFORM_LINUX   1
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <sys/time.h>
    #include <time.h>
    #include <signal.h>

    #define DC_EXPORT __attribute__((visibility("default")))
    #define DC_IMPORT
    #define DC_LIKELY(x)   __builtin_expect(!!(x), 1)
    #define DC_UNLIKELY(x) __builtin_expect(!!(x), 0)
    #define DC_FORCEINLINE __attribute__((always_inline)) inline

    using SocketHandle = int;
    constexpr SocketHandle DC_INVALID_SOCKET = -1;

    inline void DC_SleepMS(uint32_t ms) { usleep(ms * 1000u); }

    inline int64_t DC_HighResTimerFreq() { return 1'000'000'000LL; }

    inline int64_t DC_HighResTimerNow()
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    }

    // Portable byte-swap (network ↔ host byte order)
    inline uint64_t DC_BSwap64(uint64_t val) { return __builtin_bswap64(val); }
    inline uint32_t DC_BSwap32(uint32_t val) { return __builtin_bswap32(val); }
    inline uint16_t DC_BSwap16(uint16_t val) { return __builtin_bswap16(val); }

#endif
#include <cstring>
#include <string>
#include <string_view>
#include <memory>
#include <vector>
#include <array>
#include <span>
#include <optional>
#include <variant>
#include <chrono>
#include <atomic>
#include <mutex>
#include <thread>
#include <functional>
#include <format>
#include <unordered_map>
#include <cassert>

namespace DC
{
    // Byte buffer type aliases
    using Byte     = uint8_t;
    using ByteSpan = std::span<const Byte>;

    // LSN (Log Sequence Number) - 64-bit PostgreSQL WAL position
    using LSN = uint64_t;
    constexpr LSN LSN_INVALID = 0;

    // XID (Transaction ID) - 32-bit PostgreSQL transaction identifier
    using XID = uint32_t;
    constexpr XID XID_INVALID = 0;

    // OID (Object Identifier) - PostgreSQL relation/type OID
    using OID = uint32_t;

    // Timestamp type for high-resolution measurements
    using SteadyClock  = std::chrono::steady_clock;
    using SystemClock  = std::chrono::system_clock;
    using TimePoint    = SteadyClock::time_point;
    using Duration     = SteadyClock::duration;
    using Milliseconds = std::chrono::milliseconds;
    using Microseconds = std::chrono::microseconds;

    // Inline LSN formatting: "0/1A2B3C4D"
    inline std::string FormatLSN(LSN lsn)
    {
        uint32_t high = static_cast<uint32_t>(lsn >> 32);
        uint32_t low  = static_cast<uint32_t>(lsn & 0xFFFFFFFF);
        return std::format("{:X}/{:08X}", high, low);
    }

    // Parse LSN from "X/YYYYYYYY" format
    inline LSN ParseLSN(std::string_view str)
    {
        auto slash = str.find('/');
        if (slash == std::string_view::npos)
            return LSN_INVALID;

        uint32_t high = 0, low = 0;
        auto hiStr = str.substr(0, slash);
        auto loStr = str.substr(slash + 1);

        for (char c : hiStr)
        {
            high <<= 4;
            if (c >= '0' && c <= '9') high |= (c - '0');
            else if (c >= 'A' && c <= 'F') high |= (c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') high |= (c - 'a' + 10);
        }
        for (char c : loStr)
        {
            low <<= 4;
            if (c >= '0' && c <= '9') low |= (c - '0');
            else if (c >= 'A' && c <= 'F') low |= (c - 'A' + 10);
            else if (c >= 'a' && c <= 'f') low |= (c - 'a' + 10);
        }
        return (static_cast<LSN>(high) << 32) | low;
    }

} // namespace DC
