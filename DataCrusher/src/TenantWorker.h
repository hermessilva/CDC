// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// TenantWorker: independent CDC thread per tenant
// =============================================================================
#pragma once

#include "Platform.h"
#include "TenantWorkerConfig.h"
#include "ReplicationStream.h"
#include "WalMessageParser.h"
#include "TransactionManager.h"
#include "CopyWriter.h"
#include "SignalTable.h"
#include "ReconnectionManager.h"
#include "LatencyMonitor.h"
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>

namespace DC
{

// =============================================================================
// TenantWorker: self-contained CDC pipeline running on its own thread.
//
// Each worker owns its own:
//   - PGconn for replication (Source)
//   - PGconn for journal writes (Target/Journal)
//   - PGconn for signal table queries (Source, non-replication)
//   - WAL parser, transaction manager, COPY writer, reconnection manager
//   - Per-tenant latency monitor (NOT the global singleton)
//
// Workers share NOTHING except the Logger singleton (internally mutex-guarded).
// =============================================================================
class TenantWorker final
{
public:
    explicit TenantWorker(TenantWorkerConfig config);
    ~TenantWorker();

    TenantWorker(const TenantWorker&) = delete;
    TenantWorker& operator=(const TenantWorker&) = delete;
    TenantWorker(TenantWorker&&) = delete;
    TenantWorker& operator=(TenantWorker&&) = delete;

    // Start the worker thread. Non-blocking; spawns a std::jthread.
    void Start();

    // Request graceful shutdown. The thread will finish its current
    // iteration and exit cleanly.
    void RequestStop();

    // Block the calling thread until this worker finishes.
    void Join();

    // Check if the worker thread has completed execution.
    bool IsFinished() const;

    // --- Lock-free health queries (safe to call from manager thread) ---

    WorkerState      State()         const { return _State.load(std::memory_order_acquire); }
    TenantID         GetTenantID()    const { return _Config.TenantIdentifier; }
    std::string_view GetTenantName()  const { return _Config.TenantName; }
    std::string_view GetSlotName()    const { return _Config.Source.SlotName; }

    WorkerHealthSnapshot TakeHealthSnapshot() const;

private:
    // --------------- Configuration (immutable after construction) --------
    TenantWorkerConfig _Config;

    // --------------- Thread -----------------------------------------------
    std::jthread       _Thread;

    // --------------- Atomic state (read by manager, written by worker) ----
    std::atomic<WorkerState> _State{WorkerState::Idle};
    std::atomic<bool>        _StopRequested{false};
    std::atomic<LSN>         _ConfirmedLSN{LSN_INVALID};
    std::atomic<int64_t>     _TotalTransactions{0};
    std::atomic<int64_t>     _TotalChanges{0};
    std::atomic<int64_t>     _TotalErrors{0};
    std::atomic<int>         _ReconnectionCount{0};
    std::atomic<int64_t>     _LastActivityTicks{0};

    // --------------- Per-worker components (thread-local, no sharing) -----
    ReplicationStream   _Stream;
    WalMessageParser    _Parser;
    TransactionManager  _TxManager;
    CopyWriter          _Writer;
    SignalTable         _Signal;
    ReconnectionManager _ReconnManager;

    // Per-worker latency tracker (P50/P95/P99-capable histogram)
    LatencyMonitor              _LatencyMonitor;

    // In-stream session cache: XID → context captured from SYSxCDCSession WAL events
    std::unordered_map<XID, SessionContext> _SessionCache;

    // Timing for periodic tasks
    TimePoint _LastStandbyStatus;
    TimePoint _LastLatencyReport;
    TimePoint _LastStatsReport;

    // --------------- Thread entry point ------------------------------------
    void Execute(std::stop_token stopToken);

    // --------------- Lifecycle phases -------------------------------------
    bool ConnectAll();
    bool StartReplication();
    void MainLoop(std::stop_token& stopToken);
    void Cleanup();

    // --------------- WAL processing --------------------------------------
    void ProcessWalMessage(Byte* data, int length);
    void HandleBegin();
    void HandleCommit();
    void HandleChange();
    void HandleKeepAlive();

    // --------------- Periodic housekeeping --------------------------------
    void SendPeriodicStandbyStatus();
    void ReportLatency();
    void ReportStatistics();

    // --------------- Reconnection ----------------------------------------
    bool ReconnectSource();
    bool ReconnectJournal();
    bool ReconnectSignal();
    bool ReconnectAll();

    // --------------- LSN persistence -------------------------------------
    LSN  ReadLSNFromFile() const;
    bool WriteLSNToFile(LSN lsn) const;

    // --------------- Helpers ---------------------------------------------
    void SetState(WorkerState state);
    void TouchActivity();
    std::string LogPrefix() const;
};

} // namespace DC
