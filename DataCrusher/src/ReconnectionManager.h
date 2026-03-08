// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Automatic reconnection with exponential backoff and jitter
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <functional>
#include <random>
#include <atomic>

namespace DC
{

// Reconnection policy configuration
struct ReconnectionPolicy
{
    int    BaseDelayMS        = Reconnection::BaseDelayMS;
    int    MaxDelayMS         = Reconnection::MaxDelayMS;
    double BackoffMultiplier  = Reconnection::BackoffMultiplier;
    double JitterFactor       = Reconnection::JitterFactor;
    int    MaxRetries         = Reconnection::MaxRetries;   // -1 = infinite
    int    StabilityPeriodSec = Reconnection::StabilityPeriodSec;
};

// Callback type for reconnection attempts
using ReconnectCallback = std::function<bool()>;

class ReconnectionManager final
{
public:
    ReconnectionManager();
    ~ReconnectionManager() = default;

    ReconnectionManager(const ReconnectionManager&) = delete;
    ReconnectionManager& operator=(const ReconnectionManager&) = delete;

    // Configure the reconnection policy
    void Configure(const ReconnectionPolicy& policy);

    // Attempt reconnection using the provided callback
    // Blocks until successful reconnection or max retries exceeded
    // The callback should return true on successful connection
    // Returns true if reconnected, false if gave up
    bool AttemptReconnection(ReconnectCallback connectFn);

    // Signal that the connection is stable (reset retry counters)
    void MarkStable();

    // Request stop (for graceful shutdown)
    void RequestStop();

    // Check if stop was requested
    bool StopRequested() const { return _StopRequested.load(std::memory_order_relaxed); }

    // Statistics
    int  ConsecutiveFailures() const { return _ConsecutiveFailures; }
    int  TotalReconnections()  const { return _TotalReconnections; }
    int  TotalFailures()       const { return _TotalFailedAttempts; }

private:
    ReconnectionPolicy _Policy;
    std::atomic<bool>  _StopRequested{false};

    int _ConsecutiveFailures  = 0;
    int _TotalReconnections   = 0;
    int _TotalFailedAttempts  = 0;

    TimePoint _LastStableTime;

    // Random engine for jitter
    mutable std::mt19937 _RNG;

    // Calculate delay with exponential backoff and jitter
    int CalculateDelayMS() const;
};

} // namespace DC
