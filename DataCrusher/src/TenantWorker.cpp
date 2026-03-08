// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// TenantWorker implementation: independent CDC thread per tenant
// =============================================================================
#include "TenantWorker.h"
#include "Logger.h"
#include "TypeHandlers.h"
#include <fstream>
#include <chrono>

namespace DC
{

// =============================================================================
// Construction / Destruction
// =============================================================================

TenantWorker::TenantWorker(TenantWorkerConfig config)
    : _Config(std::move(config))
{
}

TenantWorker::~TenantWorker()
{
    RequestStop();
    Join();
}

// =============================================================================
// Thread Lifecycle (called from manager thread)
// =============================================================================

void TenantWorker::Start()
{
    if (_Thread.joinable())
        return; // Already running

    SetState(WorkerState::Idle);
    _StopRequested.store(false, std::memory_order_relaxed);

    _Thread = std::jthread([this](std::stop_token st) { Execute(st); });

    LOG_INFO("{} Worker thread started", LogPrefix());
}

void TenantWorker::RequestStop()
{
    _StopRequested.store(true, std::memory_order_release);
    _ReconnManager.RequestStop();

    if (_Thread.joinable())
        _Thread.request_stop();
}

void TenantWorker::Join()
{
    if (_Thread.joinable())
        _Thread.join();
}

bool TenantWorker::IsFinished() const
{
    auto state = _State.load(std::memory_order_acquire);
    return state == WorkerState::Stopped || state == WorkerState::Failed;
}

WorkerHealthSnapshot TenantWorker::TakeHealthSnapshot() const
{
    WorkerHealthSnapshot snap;
    snap.TenantIdentifier  = _Config.TenantIdentifier;
    snap.State             = _State.load(std::memory_order_acquire);
    snap.ConfirmedLSN      = _ConfirmedLSN.load(std::memory_order_relaxed);
    snap.TotalTransactions = _TotalTransactions.load(std::memory_order_relaxed);
    snap.TotalChanges      = _TotalChanges.load(std::memory_order_relaxed);
    snap.TotalErrors       = _TotalErrors.load(std::memory_order_relaxed);
    snap.ReconnectionCount = _ReconnectionCount.load(std::memory_order_relaxed);

    auto ticks = _LastActivityTicks.load(std::memory_order_relaxed);
    snap.LastActivity = TimePoint(Duration(ticks));

    return snap;
}

// =============================================================================
// Thread Entry Point
// =============================================================================

void TenantWorker::Execute(std::stop_token stopToken)
{
    LOG_INFO("{} CDC pipeline starting...", LogPrefix());
    LOG_INFO("{} Source: {}:{}/{}", LogPrefix(),
             _Config.Source.Host, _Config.Source.Port, _Config.Source.DBName);
    LOG_INFO("{} Journal: {}:{}/{}", LogPrefix(),
             _Config.Journal.Host, _Config.Journal.Port, _Config.Journal.DBName);
    LOG_INFO("{} Slot: {} (plugin: {})", LogPrefix(),
             _Config.Source.SlotName, _Config.Source.OutputPlugin);

    // Configure reconnection (per-tenant policy)
    _ReconnManager.Configure(_Config.Reconnection);

    // Initialize timers
    auto now = SteadyClock::now();
    _LastStandbyStatus = now;
    _LastLatencyReport = now;
    _LastStatsReport   = now;

    // Phase 1: Connect
    SetState(WorkerState::Connecting);
    if (!ConnectAll())
    {
        LOG_FATAL("{} Initial connection failed", LogPrefix());
        _TotalErrors.fetch_add(1, std::memory_order_relaxed);
        SetState(WorkerState::Failed);
        return;
    }

    // Phase 2: Bootstrap
    if (_Config.BootstrapJournal)
    {
        if (!_Writer.Bootstrap())
            LOG_WARN("{} Journal bootstrap failed (tables may already exist)", LogPrefix());
    }

    if (_Config.BootstrapSignal && _Signal.IsConnected())
    {
        if (!_Signal.Bootstrap(_Config.TablePrefix))
            LOG_WARN("{} Signal table bootstrap failed (table may already exist)", LogPrefix());
    }

    // After schema is guaranteed to exist, set REPLICA IDENTITY FULL and
    // pre-create per-table journal tables for all publication tables.
    if (_Signal.IsConnected())
    {
        _Signal.EnsureReplicaIdentityFull();

        auto pubTables = _Signal.GetPublicationTables(_Config.Source.Publication);
        if (pubTables.empty())
        {
            LOG_WARN("{} No tables found in publication '{}' — journal tables not pre-created",
                     LogPrefix(), _Config.Source.Publication);
        }
        else
        {
            // Remove tables that are explicitly excluded from monitoring
            if (!_Config.UntrackedTables.empty())
            {
                auto it = std::remove_if(pubTables.begin(), pubTables.end(),
                    [this](const std::string& tbl)
                    {
                        if (_Config.UntrackedTables.count(tbl))
                        {
                            LOG_INFO("{} Table '{}' is untracked — skipping journal setup",
                                     LogPrefix(), tbl);
                            return true;
                        }
                        return false;
                    });
                pubTables.erase(it, pubTables.end());
            }

            _Writer.EnsureJournalTables(pubTables);

            // Fetch column metadata from source DB and create/refresh views on target.
            // Done every startup so any source table schema changes are applied.
            std::vector<std::pair<std::string, std::vector<SourceColumnInfo>>> tableColumns;
            tableColumns.reserve(pubTables.size());
            for (const auto& tbl : pubTables)
                tableColumns.emplace_back(tbl, _Signal.GetTableColumns(tbl));
            _Writer.EnsureTableViews(tableColumns);
        }
    }

    // Phase 3: Start replication
    if (!StartReplication())
    {
        LOG_FATAL("{} Failed to start replication stream", LogPrefix());
        _TotalErrors.fetch_add(1, std::memory_order_relaxed);
        Cleanup();
        SetState(WorkerState::Failed);
        return;
    }

    // Phase 4: Main CDC loop
    SetState(WorkerState::Streaming);
    LOG_INFO("{} Entering main CDC loop", LogPrefix());
    MainLoop(stopToken);

    // Phase 5: Cleanup
    Cleanup();

    auto finalState = _StopRequested.load(std::memory_order_relaxed)
        ? WorkerState::Stopped
        : WorkerState::Failed;

    SetState(finalState);
    ReportStatistics(); // Final stats dump

    LOG_INFO("{} Worker thread finished (state={})", LogPrefix(),
             WorkerStateToString(finalState));
}

// =============================================================================
// Connection Phase
// =============================================================================

bool TenantWorker::ConnectAll()
{
    // 1. Replication connection (Source)
    if (!_Stream.Connect(_Config.Source))
    {
        LOG_ERROR("{} Failed to connect to source database", LogPrefix());
        return false;
    }

    // 2. Journal writer (Target/Destination)
    if (!_Writer.Connect(_Config.Journal))
    {
        LOG_ERROR("{} Failed to connect to journal database", LogPrefix());
        return false;
    }

    // 3. Signal table (non-replication connection to Source)
    if (!_Signal.Connect(_Config.Source.Host, _Config.Source.Port,
                          _Config.Source.DBName, _Config.Source.User,
                          _Config.Source.Password, _Config.Source.SSLMode))
    {
        LOG_WARN("{} Signal table connection failed (context capture unavailable)",
                 LogPrefix());
        // Non-fatal: CDC works without session context
    }

    // 4. Ensure publication exists on source database (uses regular connection)
    if (_Signal.IsConnected())
    {
        if (!_Signal.EnsurePublication(_Config.Source.Publication))
            LOG_WARN("{} Publication '{}' could not be verified/created — "
                     "CDC events may not arrive. Create it manually: "
                     "CREATE PUBLICATION {} FOR ALL TABLES",
                     LogPrefix(), _Config.Source.Publication, _Config.Source.Publication);
    }
    else
    {
        LOG_WARN("{} Cannot verify publication '{}' (no signal connection)",
                 LogPrefix(), _Config.Source.Publication);
    }

    // 5. Identify system
    std::string sysID, timeline, xlogpos, dbname;
    if (!_Stream.IdentifySystem(sysID, timeline, xlogpos, dbname))
    {
        LOG_ERROR("{} IDENTIFY_SYSTEM failed", LogPrefix());
        return false;
    }

    // 5. Create replication slot
    if (!_Stream.CreateSlotIfNotExists())
    {
        LOG_ERROR("{} Failed to create/verify replication slot", LogPrefix());
        return false;
    }

    return true;
}

bool TenantWorker::StartReplication()
{
    // Read last confirmed LSN from checkpoint table
    LSN startLSN = _Writer.ReadCheckpointLSN(_Config.Source.SlotName);

    // Fallback: LSN file
    if (startLSN == LSN_INVALID)
    {
        startLSN = ReadLSNFromFile();
        if (startLSN != LSN_INVALID)
            LOG_INFO("{} Resumed LSN from file: {}", LogPrefix(), FormatLSN(startLSN));
    }

    if (startLSN == LSN_INVALID)
    {
        LOG_INFO("{} No previous LSN, starting from current WAL position", LogPrefix());
        startLSN = 0;
    }

    _ConfirmedLSN.store(startLSN, std::memory_order_relaxed);
    return _Stream.StartStreaming(startLSN);
}

// =============================================================================
// Main CDC Loop (runs inside the worker thread)
// =============================================================================

void TenantWorker::MainLoop(std::stop_token& stopToken)
{
    while (!stopToken.stop_requested() &&
           !_StopRequested.load(std::memory_order_relaxed))
    {
        Byte* data   = nullptr;
        int   length = 0;

        auto result = _Stream.ReadMessage(data, length);

        switch (result)
        {
            case StreamReadResult::Data:
                ProcessWalMessage(data, length);
                _ReconnManager.MarkStable();
                TouchActivity();
                break;

            case StreamReadResult::KeepAlive:
                HandleKeepAlive();
                _ReconnManager.MarkStable();
                TouchActivity();
                break;

            case StreamReadResult::Timeout:
                SendPeriodicStandbyStatus();
                ReportLatency();
                ReportStatistics();
                DC_SleepMS(1);
                break;

            case StreamReadResult::EndOfStream:
                LOG_WARN("{} Replication stream ended unexpectedly", LogPrefix());
                SetState(WorkerState::Reconnecting);
                if (!ReconnectAll())
                {
                    LOG_FATAL("{} Failed to recover replication stream", LogPrefix());
                    return;
                }
                SetState(WorkerState::Streaming);
                break;

            case StreamReadResult::Error:
                LOG_ERROR("{} Stream read error: {}", LogPrefix(), _Stream.LastError());
                _TotalErrors.fetch_add(1, std::memory_order_relaxed);
                SetState(WorkerState::Reconnecting);
                if (!ReconnectAll())
                {
                    LOG_FATAL("{} Failed to recover from stream error", LogPrefix());
                    return;
                }
                SetState(WorkerState::Streaming);
                break;
        }
    }
}

// =============================================================================
// WAL Message Processing
// =============================================================================

void TenantWorker::ProcessWalMessage(Byte* data, int length)
{
    if (!data || length <= 0)
        return;

    char msgType = _Parser.Parse(data, static_cast<size_t>(length));

    switch (msgType)
    {
        case Operation::Begin:    HandleBegin();  break;
        case Operation::Commit:   HandleCommit(); break;

        case Operation::Insert:
        case Operation::Update:
        case Operation::Delete:
        case Operation::Truncate:
            HandleChange();
            break;

        case Operation::Relation:
        case Operation::Type:
        case Operation::Origin:
        case Operation::Message:
            break; // Informational, handled by parser

        default:
            break;
    }
}

void TenantWorker::HandleBegin()
{
    _TxManager.BeginTransaction(_Parser.LastBegin());
}

void TenantWorker::HandleCommit()
{
    XID xid = _TxManager.ActiveXID();
    int backendPID = 0;

    auto tx = _TxManager.CommitTransaction(_Parser.LastCommit(), _Signal, backendPID);
    if (!tx.has_value())
        return;

    // Prefer in-stream context (captured from SYSxCDCSession WAL events)
    // over the best-effort fallback DB query done by CommitTransaction.
    auto it = _SessionCache.find(xid);
    if (it != _SessionCache.end())
    {
        tx->Context = std::move(it->second);
        _SessionCache.erase(it);
        LOG_DEBUG("{} In-stream context applied for xid={}: user='{}' app='{}'",
                  LogPrefix(), xid, tx->Context.UserID, tx->Context.AppName);
    }
    else if (tx->Context.UserID.empty())
    {
        LOG_DEBUG("{} No session context found for xid={} — signal session INSERT may be in a separate transaction",
                  LogPrefix(), xid);
    }

    auto writeStart = SteadyClock::now();

    // Write transaction to journal
    if (!_Writer.WriteTransaction(tx.value()))
    {
        LOG_ERROR("{} Failed to write transaction xid={}", LogPrefix(), tx->TransactionXID);
        _TotalErrors.fetch_add(1, std::memory_order_relaxed);

        // Attempt journal reconnection
        if (_ReconnManager.AttemptReconnection([this]() { return ReconnectJournal(); }))
        {
            if (!_Writer.WriteTransaction(tx.value()))
            {
                LOG_FATAL("{} Transaction xid={} write failed even after reconnection",
                          LogPrefix(), tx->TransactionXID);
                return;
            }
        }
        else
        {
            LOG_FATAL("{} Journal reconnection failed for xid={}",
                      LogPrefix(), tx->TransactionXID);
            _StopRequested.store(true, std::memory_order_relaxed);
            return;
        }
    }

    auto writeEnd = SteadyClock::now();

    // Confirm LSN AFTER successful write (idempotency guarantee)
    LSN commitLSN = _Parser.LastCommit().EndLSN;
    LSN currentLSN = _ConfirmedLSN.load(std::memory_order_relaxed);

    if (commitLSN > currentLSN)
    {
        if (_Writer.SaveCheckpointLSN(_Config.Source.SlotName, commitLSN))
        {
            _ConfirmedLSN.store(commitLSN, std::memory_order_relaxed);
            WriteLSNToFile(commitLSN);
            _Stream.SendStandbyStatus(commitLSN, commitLSN, commitLSN);
        }
        else
        {
            LOG_ERROR("{} Failed to save checkpoint LSN {}", LogPrefix(),
                      FormatLSN(commitLSN));
        }
    }

    // Log processed transaction summary at INFO level
    {
        std::unordered_map<std::string, std::array<int32_t, 4>> tableCounts;
        for (const auto& change : tx->Changes)
        {
            auto key = change.SchemaName + '.' + change.TableName;
            auto& c  = tableCounts[key];
            if      (change.Operation == Operation::Insert)   ++c[0];
            else if (change.Operation == Operation::Update)   ++c[1];
            else if (change.Operation == Operation::Delete)   ++c[2];
            else if (change.Operation == Operation::Truncate) ++c[3];
        }
        std::string summary;
        for (const auto& [tbl, c] : tableCounts)
        {
            if (!summary.empty()) summary += ' ';
            summary += tbl + '(';
            bool sep = false;
            if (c[0]) { summary += std::to_string(c[0]); summary += 'I'; sep = true; }
            if (c[1]) { if (sep) summary += '/'; summary += std::to_string(c[1]); summary += 'U'; sep = true; }
            if (c[2]) { if (sep) summary += '/'; summary += std::to_string(c[2]); summary += 'D'; sep = true; }
            if (c[3]) { if (sep) summary += '/'; summary += 'T'; }
            summary += ')';
        }
        const auto& ctx = tx->Context;
        if (!ctx.UserID.empty() || !ctx.AppName.empty())
            LOG_INFO("{} xid={} changes={} lsn={} user='{}' app='{}' {}",
                     LogPrefix(), tx->TransactionXID, tx->Changes.size(),
                     FormatLSN(tx->CommitLSN), ctx.UserID, ctx.AppName, summary);
        else
            LOG_INFO("{} xid={} changes={} lsn={} user=<unknown> {}",
                     LogPrefix(), tx->TransactionXID, tx->Changes.size(),
                     FormatLSN(tx->CommitLSN), summary);
    }

    // Update statistics (atomic, safe for manager reads)
    _TotalTransactions.fetch_add(1, std::memory_order_relaxed);
    _TotalChanges.fetch_add(
        static_cast<int64_t>(tx->Changes.size()), std::memory_order_relaxed);

    // Record latency
    auto captureLatency = std::chrono::duration_cast<Microseconds>(tx->CommitTime - tx->BeginTime);
    auto writeLatency   = std::chrono::duration_cast<Microseconds>(writeEnd - writeStart);
    _LatencyMonitor.Record(captureLatency, writeLatency);
}

void TenantWorker::HandleChange()
{
    ChangeEvent event = _Parser.LastChange();

    // ------------------------------------------------------------------
    // Intercept signal table rows for in-stream session correlation.
    // The application calls pg_backend_pid() + stores user identity in
    // the SAME transaction as its business changes.  DataCrusher reads
    // the context directly from the WAL stream and caches it by XID,
    // avoiding a round-trip query at commit time.
    // ------------------------------------------------------------------
    if (event.SchemaName == "public" && event.TableName == _Config.TablePrefix + "Session")
    {
        const auto& cols = (event.Operation == Operation::Delete)
            ? event.OldValues : event.NewValues;
        SessionContext ctx;
        for (const auto& col : cols)
        {
            if (col.IsNull) continue;
            if (col.Name == "UserID")  ctx.UserID  = col.TextValue;
            if (col.Name == "AppName") ctx.AppName = col.TextValue;
        }
        LOG_DEBUG("{} Signal context captured in stream: user='{}' app='{}'",
                  LogPrefix(), ctx.UserID, ctx.AppName);
        _SessionCache[_TxManager.ActiveXID()] = std::move(ctx);
        return; // Do not journal signal table rows
    }

    // Skip tables excluded from monitoring
    if (!_Config.UntrackedTables.empty() && _Config.UntrackedTables.count(event.TableName))
        return;

    // -----------------------------------------------------------------------
    // Per-field change detail
    // INFO  : changed/added/removed fields only  (op prefix + type + value)
    // DEBUG : additionally logs unchanged fields (= prefix)
    // ~ means PostgreSQL sent no OLD tuple (table lacks REPLICA IDENTITY FULL)
    // -----------------------------------------------------------------------
    {
        const std::string tbl = std::format("{}.{}", event.SchemaName, event.TableName);
        const char        op  = event.Operation;
        const bool showUnchanged = Logger::Instance().GetLevel() <= LogLevel::Debug;

        if (op == Operation::Insert)
        {
            for (const auto& col : event.NewValues)
            {
                auto type = PgTypeOID::TypeName(col.TypeOID);
                auto val  = PgTypeOID::FormatValue(col.TypeOID, col.TextValue, col.IsNull);
                LOG_INFO("HandleChange  {}  +  {} [{}]: {}", tbl, col.Name, type, val);
            }
        }
        else if (op == Operation::Delete)
        {
            for (const auto& col : event.OldValues)
            {
                auto type = PgTypeOID::TypeName(col.TypeOID);
                auto val  = PgTypeOID::FormatValue(col.TypeOID, col.TextValue, col.IsNull);
                LOG_INFO("HandleChange  {}  -  {} [{}]: {}", tbl, col.Name, type, val);
            }
        }
        else if (op == Operation::Update)
        {
            // Index OldValues by name for O(1) lookup
            std::unordered_map<std::string, const ColumnValue*> oldMap;
            oldMap.reserve(event.OldValues.size());
            for (const auto& col : event.OldValues)
                oldMap.emplace(col.Name, &col);

            for (const auto& newCol : event.NewValues)
            {
                auto type = PgTypeOID::TypeName(newCol.TypeOID);
                auto it   = oldMap.find(newCol.Name);

                if (it == oldMap.end())
                {
                    // No OLD tuple — table needs REPLICA IDENTITY FULL for before-values
                    auto val = PgTypeOID::FormatValue(newCol.TypeOID, newCol.TextValue, newCol.IsNull);
                    LOG_INFO("HandleChange  {}  ~  {} [{}]: ? -> {}", tbl, newCol.Name, type, val);
                }
                else
                {
                    const ColumnValue& oldCol = *it->second;
                    bool changed = (oldCol.IsNull != newCol.IsNull) ||
                                   (!oldCol.IsNull && oldCol.TextValue != newCol.TextValue);
                    if (changed)
                    {
                        auto oldVal = PgTypeOID::FormatValue(oldCol.TypeOID, oldCol.TextValue, oldCol.IsNull);
                        auto newVal = PgTypeOID::FormatValue(newCol.TypeOID, newCol.TextValue, newCol.IsNull);
                        LOG_INFO("HandleChange  {}  *  {} [{}]: {} -> {}", tbl, newCol.Name, type, oldVal, newVal);
                    }
                    else if (showUnchanged)
                    {
                        auto val = PgTypeOID::FormatValue(newCol.TypeOID, newCol.TextValue, newCol.IsNull);
                        LOG_DEBUG("HandleChange  {}  =  {} [{}]: {}", tbl, newCol.Name, type, val);
                    }
                }
            }
        }
    }

    _TxManager.AddChange(std::move(event));
}

void TenantWorker::HandleKeepAlive()
{
    LSN lsn = _ConfirmedLSN.load(std::memory_order_relaxed);
    if (lsn != LSN_INVALID)
        _Stream.SendStandbyStatus(lsn, lsn, lsn);
}

// =============================================================================
// Periodic Housekeeping
// =============================================================================

void TenantWorker::SendPeriodicStandbyStatus()
{
    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - _LastStandbyStatus);

    if (elapsed.count() >= Performance::StandbyStatusIntervalSec)
    {
        LSN lsn = _ConfirmedLSN.load(std::memory_order_relaxed);
        if (lsn != LSN_INVALID && _Stream.IsConnected())
            _Stream.SendStandbyStatus(lsn, lsn, lsn);
        _LastStandbyStatus = now;
    }
}

void TenantWorker::ReportLatency()
{
    if (_Config.LatencyReportIntervalSec <= 0)
        return;

    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - _LastLatencyReport);

    if (elapsed.count() >= _Config.LatencyReportIntervalSec)
    {
        auto snap = _LatencyMonitor.TakeSnapshot();
        if (snap.SampleCount > 0)
            LOG_INFO("{} [LATENCY] samples={} capture: avg={}us p50={}us p95={}us p99={}us "
                     "write: avg={}us end2end: avg={}us",
                     LogPrefix(), snap.SampleCount,
                     snap.AvgCaptureUS, snap.P50CaptureUS, snap.P95CaptureUS, snap.P99CaptureUS,
                     snap.AvgWriteUS, snap.AvgEndToEndUS);
        _LastLatencyReport = now;
    }
}

void TenantWorker::ReportStatistics()
{
    if (_Config.StatsReportIntervalSec <= 0)
        return;

    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - _LastStatsReport);

    if (elapsed.count() >= _Config.StatsReportIntervalSec)
    {
        auto& wStats = _Writer.GetStats();
        LSN lsn = _ConfirmedLSN.load(std::memory_order_relaxed);

        LOG_INFO("{} [STATS] tx={} changes={} aborted={} rows={} "
                 "batches={} errors={} lsn={}",
                 LogPrefix(),
                 _TotalTransactions.load(std::memory_order_relaxed),
                 _TotalChanges.load(std::memory_order_relaxed),
                 _TxManager.AbortedTransactions(),
                 wStats.TotalRowsWritten,
                 wStats.TotalBatchesFlushed,
                 wStats.TotalCopyErrors,
                 FormatLSN(lsn));
        LOG_INFO("{} [STATS] parser={} parse_errors={} reconnections={}",
                 LogPrefix(),
                 _Parser.TotalMessagesParsed(),
                 _Parser.TotalParseErrors(),
                 _ReconnectionCount.load(std::memory_order_relaxed));

        _LastStatsReport = now;
    }
}

// =============================================================================
// Reconnection (per-tenant, does NOT affect other workers)
// =============================================================================

bool TenantWorker::ReconnectSource()
{
    _Stream.Disconnect();
    if (!_Stream.Connect(_Config.Source))
        return false;

    std::string sysID, timeline, xlogpos, dbname;
    if (!_Stream.IdentifySystem(sysID, timeline, xlogpos, dbname))
        return false;

    LSN lsn = _ConfirmedLSN.load(std::memory_order_relaxed);
    return _Stream.StartStreaming(lsn);
}

bool TenantWorker::ReconnectJournal()
{
    _Writer.Disconnect();
    return _Writer.Connect(_Config.Journal);
}

bool TenantWorker::ReconnectSignal()
{
    _Signal.Disconnect();
    return _Signal.Connect(_Config.Source.Host, _Config.Source.Port,
                            _Config.Source.DBName, _Config.Source.User,
                            _Config.Source.Password, _Config.Source.SSLMode);
}

bool TenantWorker::ReconnectAll()
{
    LOG_WARN("{} Full reconnection starting...", LogPrefix());

    _TxManager.AbortTransaction();

    bool ok = _ReconnManager.AttemptReconnection([this]()
    {
        if (!ReconnectSource())  return false;
        if (!ReconnectJournal()) return false;
        ReconnectSignal(); // Non-fatal
        return true;
    });

    if (ok)
        _ReconnectionCount.fetch_add(1, std::memory_order_relaxed);

    return ok;
}

// =============================================================================
// Cleanup
// =============================================================================

void TenantWorker::Cleanup()
{
    LOG_INFO("{} Disconnecting all connections...", LogPrefix());
    _Stream.Disconnect();
    _Writer.Disconnect();
    _Signal.Disconnect();
}

// =============================================================================
// LSN File Persistence
// =============================================================================

LSN TenantWorker::ReadLSNFromFile() const
{
    if (_Config.LSNFilePath.empty())
        return LSN_INVALID;

    std::ifstream file(_Config.LSNFilePath);
    if (!file.is_open())
        return LSN_INVALID;

    std::string lsnStr;
    if (std::getline(file, lsnStr) && !lsnStr.empty())
        return ParseLSN(lsnStr);

    return LSN_INVALID;
}

bool TenantWorker::WriteLSNToFile(LSN lsn) const
{
    if (_Config.LSNFilePath.empty())
        return false;

    std::ofstream file(_Config.LSNFilePath, std::ios::trunc);
    if (!file.is_open())
    {
        LOG_WARN("{} Cannot write LSN file: {}", LogPrefix(), _Config.LSNFilePath);
        return false;
    }
    file << FormatLSN(lsn) << '\n';
    return true;
}

// =============================================================================
// Helpers
// =============================================================================

void TenantWorker::SetState(WorkerState state)
{
    _State.store(state, std::memory_order_release);
}

void TenantWorker::TouchActivity()
{
    auto now = SteadyClock::now().time_since_epoch().count();
    _LastActivityTicks.store(now, std::memory_order_relaxed);
}

std::string TenantWorker::LogPrefix() const
{
    return std::format("[T:{}:{}]", _Config.TenantIdentifier, _Config.TenantName);
}

} // namespace DC
