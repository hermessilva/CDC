// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Reconnection manager implementation
// =============================================================================
#include "ReconnectionManager.h"
#include "Logger.h"
#include <cmath>
#include <algorithm>

namespace DC
{

ReconnectionManager::ReconnectionManager()
    : _LastStableTime(SteadyClock::now())
{
    // Seed with a combination of time and address entropy
    auto seed = static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    _RNG.seed(seed);
}

void ReconnectionManager::Configure(const ReconnectionPolicy& policy)
{
    _Policy = policy;
}

bool ReconnectionManager::AttemptReconnection(ReconnectCallback connectFn)
{
    LOG_WARN("Connection lost. Starting reconnection sequence...");

    _ConsecutiveFailures = 0;

    while (!StopRequested())
    {
        // Check max retries
        if (_Policy.MaxRetries >= 0 && _ConsecutiveFailures >= _Policy.MaxRetries)
        {
            LOG_FATAL("Maximum reconnection attempts ({}) exceeded. Giving up.",
                      _Policy.MaxRetries);
            return false;
        }

        // Calculate delay
        int delayMS = CalculateDelayMS();
        LOG_INFO("Reconnection attempt {} in {}ms...",
                 _ConsecutiveFailures + 1, delayMS);

        // Wait with periodic stop checks
        int waited = 0;
        constexpr int checkInterval = 100; // Check stop every 100ms
        while (waited < delayMS && !StopRequested())
        {
            int sleepTime = std::min(checkInterval, delayMS - waited);
            DC_SleepMS(static_cast<uint32_t>(sleepTime));
            waited += sleepTime;
        }

        if (StopRequested())
        {
            LOG_INFO("Reconnection aborted: stop requested");
            return false;
        }

        // Attempt connection
        LOG_INFO("Attempting reconnection (attempt #{})...", _ConsecutiveFailures + 1);

        if (connectFn())
        {
            LOG_INFO("Reconnected successfully after {} failed attempts",
                     _ConsecutiveFailures);
            ++_TotalReconnections;
            _ConsecutiveFailures = 0;
            _LastStableTime = SteadyClock::now();
            return true;
        }

        ++_ConsecutiveFailures;
        ++_TotalFailedAttempts;
        LOG_WARN("Reconnection attempt #{} failed", _ConsecutiveFailures);
    }

    return false;
}

void ReconnectionManager::MarkStable()
{
    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _LastStableTime);

    if (elapsed.count() >= _Policy.StabilityPeriodSec)
    {
        if (_ConsecutiveFailures > 0)
        {
            LOG_DEBUG("Connection stable for {}s, resetting failure counter",
                      elapsed.count());
        }
        _ConsecutiveFailures = 0;
        _LastStableTime = now;
    }
}

void ReconnectionManager::RequestStop()
{
    _StopRequested.store(true, std::memory_order_relaxed);
}

int ReconnectionManager::CalculateDelayMS() const
{
    // Exponential backoff: base * multiplier^failures
    double baseDelay = static_cast<double>(_Policy.BaseDelayMS);
    double delay = baseDelay * std::pow(_Policy.BackoffMultiplier,
                                         static_cast<double>(_ConsecutiveFailures));

    // Cap at max delay
    delay = std::min(delay, static_cast<double>(_Policy.MaxDelayMS));

    // Apply jitter: delay * (1 ± jitter_factor)
    if (_Policy.JitterFactor > 0.0)
    {
        std::uniform_real_distribution<double> dist(
            1.0 - _Policy.JitterFactor,
            1.0 + _Policy.JitterFactor
        );
        delay *= dist(_RNG);
    }

    return std::max(1, static_cast<int>(delay));
}

} // namespace DC
