// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Latency monitor implementation
// =============================================================================
#include "LatencyMonitor.h"
#include "Logger.h"
#include <cstring>
#include <algorithm>

namespace DC
{

void LatencyMonitor::Record(Microseconds captureLatency, Microseconds writeLatency)
{
    int64_t capUS   = captureLatency.count();
    int64_t writeUS = writeLatency.count();
    int64_t totalUS = capUS + writeUS;

    std::lock_guard lock(_Mutex);

    ++_SampleCount;
    _SumCaptureUS += capUS;
    _SumWriteUS   += writeUS;
    _SumTotalUS   += totalUS;

    _MinCaptureUS = std::min(_MinCaptureUS, capUS);
    _MaxCaptureUS = std::max(_MaxCaptureUS, capUS);
    _MinWriteUS   = std::min(_MinWriteUS, writeUS);
    _MaxWriteUS   = std::max(_MaxWriteUS, writeUS);
    _MinTotalUS   = std::min(_MinTotalUS, totalUS);
    _MaxTotalUS   = std::max(_MaxTotalUS, totalUS);

    // Update histogram
    int bucket = GetBucketIndex(capUS);
    ++_CaptureHistogram[bucket];

    _LifetimeSamples.fetch_add(1, std::memory_order_relaxed);
}

LatencySnapshot LatencyMonitor::TakeSnapshot()
{
    std::lock_guard lock(_Mutex);

    LatencySnapshot snap;
    if (_SampleCount == 0)
        return snap;

    snap.SampleCount   = _SampleCount;
    snap.MinCaptureUS  = _MinCaptureUS;
    snap.MaxCaptureUS  = _MaxCaptureUS;
    snap.AvgCaptureUS  = _SumCaptureUS / _SampleCount;
    snap.MinWriteUS    = _MinWriteUS;
    snap.MaxWriteUS    = _MaxWriteUS;
    snap.AvgWriteUS    = _SumWriteUS / _SampleCount;
    snap.MinEndToEndUS = _MinTotalUS;
    snap.MaxEndToEndUS = _MaxTotalUS;
    snap.AvgEndToEndUS = _SumTotalUS / _SampleCount;
    snap.P50CaptureUS  = EstimatePercentile(0.50);
    snap.P95CaptureUS  = EstimatePercentile(0.95);
    snap.P99CaptureUS  = EstimatePercentile(0.99);

    // Reset for next window
    _SampleCount  = 0;
    _SumCaptureUS = 0;
    _SumWriteUS   = 0;
    _SumTotalUS   = 0;
    _MinCaptureUS = INT64_MAX;
    _MaxCaptureUS = 0;
    _MinWriteUS   = INT64_MAX;
    _MaxWriteUS   = 0;
    _MinTotalUS   = INT64_MAX;
    _MaxTotalUS   = 0;
    std::memset(_CaptureHistogram, 0, sizeof(_CaptureHistogram));

    return snap;
}

LatencySnapshot LatencyMonitor::PeekSnapshot() const
{
    std::lock_guard lock(_Mutex);

    LatencySnapshot snap;
    if (_SampleCount == 0)
        return snap;

    snap.SampleCount   = _SampleCount;
    snap.MinCaptureUS  = _MinCaptureUS;
    snap.MaxCaptureUS  = _MaxCaptureUS;
    snap.AvgCaptureUS  = _SumCaptureUS / _SampleCount;
    snap.MinWriteUS    = _MinWriteUS;
    snap.MaxWriteUS    = _MaxWriteUS;
    snap.AvgWriteUS    = _SumWriteUS / _SampleCount;
    snap.MinEndToEndUS = _MinTotalUS;
    snap.MaxEndToEndUS = _MaxTotalUS;
    snap.AvgEndToEndUS = _SumTotalUS / _SampleCount;
    snap.P50CaptureUS  = EstimatePercentile(0.50);
    snap.P95CaptureUS  = EstimatePercentile(0.95);
    snap.P99CaptureUS  = EstimatePercentile(0.99);

    return snap;
}

void LatencyMonitor::Reset()
{
    std::lock_guard lock(_Mutex);
    _SampleCount  = 0;
    _SumCaptureUS = 0;
    _SumWriteUS   = 0;
    _SumTotalUS   = 0;
    _MinCaptureUS = INT64_MAX;
    _MaxCaptureUS = 0;
    _MinWriteUS   = INT64_MAX;
    _MaxWriteUS   = 0;
    _MinTotalUS   = INT64_MAX;
    _MaxTotalUS   = 0;
    std::memset(_CaptureHistogram, 0, sizeof(_CaptureHistogram));
}

int LatencyMonitor::GetBucketIndex(int64_t microseconds) const
{
    // Bucket boundaries: 100, 500, 1000, 5000, 10000, 50000, 100000
    if (microseconds < 100)    return 0;
    if (microseconds < 500)    return 1;
    if (microseconds < 1000)   return 2;
    if (microseconds < 5000)   return 3;
    if (microseconds < 10000)  return 4;
    if (microseconds < 50000)  return 5;
    if (microseconds < 100000) return 6;
    return 7;
}

int64_t LatencyMonitor::EstimatePercentile(double pct) const
{
    if (_SampleCount == 0) return 0;

    int64_t targetCount = static_cast<int64_t>(static_cast<double>(_SampleCount) * pct);
    int64_t accumulated = 0;

    // Bucket midpoints (microseconds)
    constexpr int64_t midpoints[BUCKET_COUNT] = {
        50, 300, 750, 3000, 7500, 30000, 75000, 200000
    };

    for (int i = 0; i < BUCKET_COUNT; ++i)
    {
        accumulated += _CaptureHistogram[i];
        if (accumulated >= targetCount)
            return midpoints[i];
    }

    return midpoints[BUCKET_COUNT - 1];
}

} // namespace DC
