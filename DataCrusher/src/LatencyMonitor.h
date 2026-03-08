// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Latency monitor: tracks end-to-end CDC pipeline latency
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <atomic>
#include <mutex>

namespace DC
{

// Latency statistics snapshot
struct LatencySnapshot
{
    int64_t SampleCount        = 0;
    int64_t MinCaptureUS       = 0;
    int64_t MaxCaptureUS       = 0;
    int64_t AvgCaptureUS       = 0;
    int64_t P50CaptureUS       = 0;
    int64_t P95CaptureUS       = 0;
    int64_t P99CaptureUS       = 0;
    int64_t MinWriteUS         = 0;
    int64_t MaxWriteUS         = 0;
    int64_t AvgWriteUS         = 0;
    int64_t MinEndToEndUS      = 0;
    int64_t MaxEndToEndUS      = 0;
    int64_t AvgEndToEndUS      = 0;
};

class LatencyMonitor final
{
public:
    LatencyMonitor() = default;

    // Record a single latency measurement
    void Record(Microseconds captureLatency, Microseconds writeLatency);

    // Get a snapshot of current statistics and reset counters
    LatencySnapshot TakeSnapshot();

    // Get current statistics without resetting
    LatencySnapshot PeekSnapshot() const;

    // Reset all counters
    void Reset();

    // Total samples recorded (lifetime)
    int64_t TotalSamples() const { return _LifetimeSamples.load(std::memory_order_relaxed); }

private:
    mutable std::mutex _Mutex;

    // Running statistics for current window
    int64_t _SampleCount   = 0;
    int64_t _SumCaptureUS  = 0;
    int64_t _SumWriteUS    = 0;
    int64_t _SumTotalUS    = 0;
    int64_t _MinCaptureUS  = INT64_MAX;
    int64_t _MaxCaptureUS  = 0;
    int64_t _MinWriteUS    = INT64_MAX;
    int64_t _MaxWriteUS    = 0;
    int64_t _MinTotalUS    = INT64_MAX;
    int64_t _MaxTotalUS    = 0;

    // Histogram buckets for percentile estimation (capture latency)
    // Buckets: 0-100us, 100-500us, 500us-1ms, 1-5ms, 5-10ms, 10-50ms, 50-100ms, 100ms+
    static constexpr int BUCKET_COUNT = 8;
    int64_t _CaptureHistogram[BUCKET_COUNT] = {};

    std::atomic<int64_t> _LifetimeSamples{0};

    int GetBucketIndex(int64_t microseconds) const;
    int64_t EstimatePercentile(double pct) const;
};

} // namespace DC
