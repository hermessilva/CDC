// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// WorkerManager implementation: multi-tenant orchestrator
// Reads tenant credentials from centralized ADM database (SYSxTenant table)
// =============================================================================
#include "WorkerManager.h"
#include "Logger.h"
#include "TypeHandlers.h"
#include <libpq-fe.h>
#include <csignal>
#include <cstdlib>
#include <format>
#include <algorithm>

namespace DC
{

// =============================================================================
// Global manager pointer for signal handling
// =============================================================================
static WorkerManager* g_Manager = nullptr;

static void ManagerSignalHandler(int signal)
{
    if (g_Manager)
    {
        LOG_INFO("Signal {} received, requesting manager shutdown...", signal);
        g_Manager->RequestShutdown();
    }
}

// =============================================================================
// SQL for CDCxConfig table in the ADM (centralized admin) database
// =============================================================================
namespace ADMSQL
{
    // Query active CDC configurations from the CDCxConfig table.
    // State = 1 means Active.
    // Columns:
    //   0: "ID"              (UUID)
    //   1: "Name"            (varchar)
    //   2: "DBHost"          (varchar) - Source database host
    //   3: "DBHostPort"      (int)     - Source database port
    //   4: "DBName"          (varchar) - Source database name
    //   5: "DBUser"          (varchar) - Source database user
    //   6: "DBPassword"      (varchar) - Source database password
    //   7: "DBCDCHost"       (varchar) - Journal database host
    //   8: "DBCDCHostPort"   (int)     - Journal database port
    //   9: "DBCDCName"       (varchar) - Journal database name
    //  10: "DBCDCUser"       (varchar) - Journal database user
    //  11: "DBCDCPassword"   (varchar) - Journal database password
    //  12: "TablePrefix"     (varchar) - Schema prefix for journal tables + session table
    //  13: "UntrackedTables" (varchar) - ";"-delimited list of table names to exclude from journalling
    constexpr std::string_view SelectActiveTenants = R"SQL(
        SELECT
            "ID",
            "Name",
            "DBHost",
            "DBHostPort",
            "DBName",
            "DBUser",
            "DBPassword",
            "DBCDCHost",
            "DBCDCHostPort",
            "DBCDCName",
            "DBCDCUser",
            "DBCDCPassword",
            "TablePrefix",
            "UntrackedTables"
        FROM "CDCxConfig"
        WHERE "State" = 1
        ORDER BY "Name"
    )SQL";

    // Upsert checkpoint LSN per CDCxConfig entry into the ADM database
    // Used during graceful shutdown to persist the last confirmed position
    constexpr std::string_view BootstrapCheckpointTable = R"SQL(
        CREATE TABLE IF NOT EXISTS "CDCxCheckpoint" (
            "CDCxConfigID"  UUID        PRIMARY KEY,
            "SlotName"      TEXT        NOT NULL,
            "ConfirmedLSN"  TEXT        NOT NULL,
            "UpdatedAt"     TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
    )SQL";

    inline std::string UpsertTenantCheckpoint(const std::string& configID,
                                               const std::string& slotName,
                                               const std::string& lsnStr)
    {
        return std::format(
            "INSERT INTO \"CDCxCheckpoint\" (\"CDCxConfigID\", \"SlotName\", \"ConfirmedLSN\", \"UpdatedAt\") "
            "VALUES ('{}', '{}', '{}', NOW()) "
            "ON CONFLICT (\"CDCxConfigID\") DO UPDATE SET "
            "\"ConfirmedLSN\" = '{}', \"SlotName\" = '{}', \"UpdatedAt\" = NOW()",
            configID, slotName, lsnStr, lsnStr, slotName
        );
    }
}

// =============================================================================
// Construction / Destruction
// =============================================================================

WorkerManager::WorkerManager()
{
    g_Manager = this;
}

WorkerManager::~WorkerManager()
{
    ConfirmAllLSNToADM();
    StopAllWorkers();
    DisconnectADM();
    g_Manager = nullptr;
}

// =============================================================================
// Initialization
// =============================================================================

bool WorkerManager::Initialize(const ManagerConfig& config)
{
    _Config = config;

    // Initialize logger
    Logger::Instance().Initialize(_Config.LogLevelValue, _Config.LogFilePath);

    PrintManagerBanner();

    // Install signal handlers
    std::signal(SIGINT, ManagerSignalHandler);
    std::signal(SIGTERM, ManagerSignalHandler);
#if DC_PLATFORM_LINUX
    std::signal(SIGHUP, ManagerSignalHandler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Initialize type handlers (shared singleton, thread-safe after init)
    TypeHandlers::Instance().InitializeBuiltins();

    // Connect to ADM database
    if (!ConnectADM())
    {
        LOG_FATAL("Failed to connect to ADM database (SYSxTenant source)");
        return false;
    }

    // Bootstrap checkpoint table in ADM for LSN persistence
    PGresult* res = PQexec(_ADMConn,
                           std::string(ADMSQL::BootstrapCheckpointTable).c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_WARN("ADM checkpoint bootstrap: {} (table may already exist)", PQerrorMessage(_ADMConn));
    }
    PQclear(res);

    // Initialize timers
    auto now = SteadyClock::now();
    _LastTenantPoll = now;
    _LastHealthCheck = now;

    LOG_INFO("WorkerManager initialized (max_workers={}, poll_interval={}s, health_interval={}s)",
             _Config.MaxWorkers, _Config.TenantPollIntervalSec, _Config.HealthCheckIntervalSec);

    return true;
}

// =============================================================================
// Main Manager Loop
// =============================================================================

int WorkerManager::Run()
{
    LOG_INFO("Starting WorkerManager main loop...");

    // Initial tenant discovery
    if (!RefreshTenantList())
    {
        LOG_ERROR("Initial tenant discovery failed, will retry on next poll");
    }

    while (!ShutdownRequested())
    {
        RunPeriodicTenantPoll();
        RunPeriodicHealthCheck();
        ReapFinishedWorkers();

        // Sleep to avoid busy-wait (short enough for responsive shutdown)
        DC_SleepMS(500);
    }

    // Graceful shutdown
    LOG_INFO("Manager shutdown initiated, stopping all workers...");
    ConfirmAllLSNToADM();
    StopAllWorkers();

    PrintFinalSummary();
    DisconnectADM();

    LOG_INFO("WorkerManager shutdown complete");
    return ExitCode::Success;
}

void WorkerManager::RequestShutdown()
{
    _ShutdownRequested.store(true, std::memory_order_relaxed);
}

// =============================================================================
// ADM Connection
// =============================================================================

bool WorkerManager::ConnectADM()
{
    auto connStr = BuildADMConnString(_Config);
    _ADMConn = PQconnectdb(connStr.c_str());

    // Scrub connection string from memory (contains password)
    Security::SecureErase(connStr);

    if (PQstatus(_ADMConn) != CONNECTION_OK)
    {
        LOG_ERROR("ADM connection failed: {}", PQerrorMessage(_ADMConn));
        PQfinish(_ADMConn);
        _ADMConn = nullptr;
        return false;
    }

    LOG_INFO("Connected to ADM database: {}:{}/{}",
             _Config.ADMHost, _Config.ADMPort, _Config.ADMDBName);
    return true;
}

void WorkerManager::DisconnectADM()
{
    if (_ADMConn)
    {
        PQfinish(_ADMConn);
        _ADMConn = nullptr;
    }
}

bool WorkerManager::IsADMConnected() const
{
    return _ADMConn && PQstatus(_ADMConn) == CONNECTION_OK;
}

bool WorkerManager::ReconnectADM()
{
    LOG_WARN("Attempting ADM reconnection...");
    DisconnectADM();
    return ConnectADM();
}

std::string WorkerManager::BuildADMConnString(const ManagerConfig& config)
{
    return std::format(
        "host={} port={} dbname={} user={} password={} "
        "sslmode={} application_name=DataCrusher-Manager "
        "connect_timeout={}",
        config.ADMHost, config.ADMPort,
        config.ADMDBName, config.ADMUser,
        config.ADMPassword, config.ADMSSLMode,
        Connection::ConnectTimeoutSec
    );
}

// =============================================================================
// Tenant Discovery (SYSxTenant table in ADM database)
// =============================================================================

bool WorkerManager::RefreshTenantList()
{
    if (!IsADMConnected())
    {
        if (!ReconnectADM())
        {
            LOG_ERROR("ADM unavailable, skipping tenant refresh");
            return false;
        }
    }

    PGresult* res = PQexec(_ADMConn,
                           std::string(ADMSQL::SelectActiveTenants).c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_ERROR("CDCxConfig query failed: {}", PQerrorMessage(_ADMConn));
        PQclear(res);

        // Connection might be stale, try reconnecting
        if (!ReconnectADM())
            return false;

        res = PQexec(_ADMConn,
                     std::string(ADMSQL::SelectActiveTenants).c_str());
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            LOG_ERROR("CDCxConfig query failed after reconnection: {}", PQerrorMessage(_ADMConn));
            PQclear(res);
            return false;
        }
    }

    int rowCount = PQntuples(res);

    // Collect active CDCxConfig IDs
    std::unordered_map<TenantID, bool> activeTenants;

    for (int row = 0; row < rowCount; ++row)
    {
        // Column 0: ID (UUID)
        TenantID tenantID = PQgetvalue(res, row, 0);
        activeTenants[tenantID] = true;

        // If worker already running for this tenant, skip
        if (_Workers.contains(tenantID))
            continue;

        // Check max workers limit
        if (static_cast<int>(_Workers.size()) >= _Config.MaxWorkers)
        {
            LOG_WARN("Max workers ({}) reached, cannot start tenant {}",
                     _Config.MaxWorkers, tenantID);
            continue;
        }

        // Extract fields from SYSxTenant row
        auto field = [&](int col) -> std::string
        {
            const char* val = PQgetvalue(res, row, col);
            return val ? std::string(val) : "";
        };

        auto fieldInt = [&](int col, int defaultVal) -> int
        {
            const char* val = PQgetvalue(res, row, col);
            if (!val || !*val) return defaultVal;
            try { return std::stoi(val); }
            catch (...) { return defaultVal; }
        };

        //  Map CDCxConfig columns to TenantWorkerConfig
        //  Source (Origin): DBHost, DBHostPort, DBName, DBUser, DBPassword
        //  Journal (Dest):  DBCDCHost, DBCDCHostPort, DBCDCName, DBCDCUser, DBCDCPassword
        //  Prefix:          TablePrefix (schema + session table)
        auto config = BuildWorkerConfig(
            tenantID,
            field(1),                                          // Name
            field(2),                                          // DBHost
            static_cast<uint16_t>(fieldInt(3, 5432)),          // DBHostPort
            field(4),                                          // DBName
            field(5),                                          // DBUser
            field(6),                                          // DBPassword
            field(7),                                          // DBCDCHost
            static_cast<uint16_t>(fieldInt(8, 5432)),          // DBCDCHostPort
            field(9),                                          // DBCDCName
            field(10),                                         // DBCDCUser
            field(11),                                         // DBCDCPassword
            field(12),                                         // TablePrefix
            field(13)                                          // UntrackedTables
        );

        // Scrub extracted password fields from local copies
        // (BuildWorkerConfig already moved them into the config struct)

        StartWorker(std::move(config));
    }

    PQclear(res);

    // Stop workers for tenants that are no longer active
    std::vector<TenantID> toRemove;
    for (auto& [tenantID, worker] : _Workers)
    {
        if (!activeTenants.contains(tenantID))
        {
            LOG_INFO("CDCxConfig entry {} no longer active, stopping worker", tenantID);
            toRemove.push_back(tenantID);
        }
    }

    for (auto& id : toRemove)
        StopWorker(id);

    LOG_DEBUG("Tenant refresh complete: {} active, {} workers running",
              rowCount, _Workers.size());

    return true;
}

TenantWorkerConfig WorkerManager::BuildWorkerConfig(
    const TenantID& tenantID,
    const std::string& tenantName,
    const std::string& dbHost, uint16_t dbPort,
    const std::string& dbName, const std::string& dbUser,
    const std::string& dbPassword,
    const std::string& dbCDCHost, uint16_t dbCDCPort,
    const std::string& dbCDCName, const std::string& dbCDCUser,
    const std::string& dbCDCPassword,
    const std::string& tablePrefix,
    const std::string& untrackedTables
) const
{
    TenantWorkerConfig cfg;

    cfg.TenantIdentifier = tenantID;
    cfg.TenantName       = tenantName;
    cfg.TablePrefix      = tablePrefix.empty() ? "CDCx" : tablePrefix;

    // PostgreSQL replication slot names: only [a-z0-9_] are valid.
    // Sanitize tenant name: lowercase, replace non-alnum with '_'.
    std::string sanitizedName;
    sanitizedName.reserve(tenantName.size());
    for (char c : tenantName)
    {
        if (std::isalnum(static_cast<unsigned char>(c)))
            sanitizedName += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else
            sanitizedName += '_';
    }

    // Source (replication against the tenant's origin database)
    cfg.Source.Host          = dbHost;
    cfg.Source.Port          = dbPort;
    cfg.Source.DBName        = dbName;
    cfg.Source.User          = dbUser;
    cfg.Source.Password      = dbPassword;
    cfg.Source.SSLMode       = _Config.DefaultSSLMode;
    cfg.Source.SlotName      = std::format("datacrusher_slot_{}", sanitizedName);
    cfg.Source.Publication   = std::string(Replication::PublicationName);
    cfg.Source.OutputPlugin  = std::string(Replication::OutputPlugin);
    cfg.Source.ProtoVersion  = Replication::ProtocolVersion;
    cfg.Source.ConnTimeoutSec = Connection::ConnectTimeoutSec;

    // Journal (destination - tenant's journal database)
    cfg.Journal.Host          = dbCDCHost;
    cfg.Journal.Port          = dbCDCPort;
    cfg.Journal.DBName        = dbCDCName;
    cfg.Journal.User          = dbCDCUser;
    cfg.Journal.Password      = dbCDCPassword;
    cfg.Journal.SSLMode       = _Config.DefaultSSLMode;
    cfg.Journal.Schema        = cfg.TablePrefix;
    cfg.Journal.BatchSize     = _Config.DefaultBatchSize;
    cfg.Journal.FlushIntervalMS = _Config.DefaultFlushIntervalMS;

    // Reconnection (use manager defaults)
    cfg.Reconnection = _Config.DefaultReconnection;

    // Bootstrap flags
    cfg.BootstrapJournal = true;
    cfg.BootstrapSignal  = true;

    // LSN file per tenant (fallback)
    cfg.LSNFilePath = std::format("datacrusher_{}.lsn", tenantID);

    // Parse ";"-delimited UntrackedTables into a lookup set
    if (!untrackedTables.empty())
    {
        std::string_view remaining = untrackedTables;
        while (!remaining.empty())
        {
            auto pos = remaining.find(';');
            auto token = (pos == std::string_view::npos) ? remaining : remaining.substr(0, pos);
            // Trim whitespace
            while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
            while (!token.empty() && token.back()  == ' ') token.remove_suffix(1);
            if (!token.empty())
                cfg.UntrackedTables.emplace(token);
            if (pos == std::string_view::npos) break;
            remaining.remove_prefix(pos + 1);
        }
        if (!cfg.UntrackedTables.empty())
            LOG_INFO("Tenant {} — {} untracked table(s) configured",
                     tenantName, cfg.UntrackedTables.size());
    }

    return cfg;
}

// =============================================================================
// Worker Lifecycle
// =============================================================================

bool WorkerManager::StartWorker(TenantWorkerConfig config)
{
    TenantID id = config.TenantIdentifier;

    if (_Workers.contains(id))
    {
        LOG_WARN("Worker for tenant {} already running, skipping", id);
        return false;
    }

    LOG_INFO("Starting worker for tenant {} [{}]", id, config.TenantName);

    auto worker = std::make_unique<TenantWorker>(std::move(config));
    worker->Start();

    _Workers[id] = std::move(worker);
    return true;
}

void WorkerManager::StopWorker(const TenantID& tenantID)
{
    auto it = _Workers.find(tenantID);
    if (it == _Workers.end())
        return;

    LOG_INFO("Stopping worker for tenant {}", tenantID);
    it->second->RequestStop();
    it->second->Join();

    LOG_INFO("Worker for tenant {} stopped", tenantID);
    _Workers.erase(it);
}

void WorkerManager::StopAllWorkers()
{
    if (_Workers.empty())
        return;

    LOG_INFO("Stopping all {} workers...", _Workers.size());

    // Request stop on all workers first (non-blocking)
    for (auto& [id, worker] : _Workers)
        worker->RequestStop();

    // Now join all
    for (auto& [id, worker] : _Workers)
    {
        worker->Join();
        LOG_INFO("Worker for tenant {} joined", id);
    }

    _Workers.clear();
    LOG_INFO("All workers stopped");
}

// =============================================================================
// Health Monitoring
// =============================================================================

void WorkerManager::CheckWorkerHealth()
{
    for (auto& [tenantID, worker] : _Workers)
    {
        auto snap = worker->TakeHealthSnapshot();

        // Detect stuck workers (no activity for 5 minutes)
        auto now = SteadyClock::now();
        auto sinceActivity = std::chrono::duration_cast<std::chrono::seconds>(
            now - snap.LastActivity);

        if (snap.State == WorkerState::Streaming && sinceActivity.count() > 300)
        {
            LOG_WARN("[HEALTH] Tenant {} [{}] no activity for {}s (state={})",
                     tenantID, worker->GetTenantName(),
                     sinceActivity.count(), WorkerStateToString(snap.State));
        }

        // Log workers in reconnecting state
        if (snap.State == WorkerState::Reconnecting)
        {
            LOG_WARN("[HEALTH] Tenant {} [{}] reconnecting (attempts={})",
                     tenantID, worker->GetTenantName(), snap.ReconnectionCount);
        }

        // Log high error counts
        if (snap.TotalErrors > 0)
        {
            LOG_DEBUG("[HEALTH] Tenant {} errors={} tx={} changes={} lsn={}",
                      tenantID, snap.TotalErrors, snap.TotalTransactions,
                      snap.TotalChanges, FormatLSN(snap.ConfirmedLSN));
        }
    }
}

void WorkerManager::ReapFinishedWorkers()
{
    std::vector<TenantID> finished;

    for (auto& [tenantID, worker] : _Workers)
    {
        if (worker->IsFinished())
        {
            auto snap = worker->TakeHealthSnapshot();
            auto stateStr = WorkerStateToString(snap.State);

            if (snap.State == WorkerState::Failed)
            {
                LOG_ERROR("Worker for tenant {} [{}] FAILED (tx={} errors={})",
                          tenantID, worker->GetTenantName(),
                          snap.TotalTransactions, snap.TotalErrors);
            }
            else
            {
                LOG_INFO("Worker for tenant {} [{}] finished (state={} tx={})",
                         tenantID, worker->GetTenantName(),
                         stateStr, snap.TotalTransactions);
            }

            finished.push_back(tenantID);
        }
    }

    for (auto& id : finished)
    {
        _Workers[id]->Join();
        _Workers.erase(id);
    }
}

std::vector<WorkerHealthSnapshot> WorkerManager::GetAllHealthSnapshots() const
{
    std::vector<WorkerHealthSnapshot> snapshots;
    snapshots.reserve(_Workers.size());

    for (auto& [id, worker] : _Workers)
        snapshots.push_back(worker->TakeHealthSnapshot());

    return snapshots;
}

int WorkerManager::ActiveWorkerCount() const
{
    return static_cast<int>(_Workers.size());
}

// =============================================================================
// Periodic Tasks
// =============================================================================

void WorkerManager::RunPeriodicTenantPoll()
{
    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _LastTenantPoll);

    if (elapsed.count() >= _Config.TenantPollIntervalSec)
    {
        RefreshTenantList();
        _LastTenantPoll = now;
    }
}

void WorkerManager::RunPeriodicHealthCheck()
{
    auto now = SteadyClock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _LastHealthCheck);

    if (elapsed.count() >= _Config.HealthCheckIntervalSec)
    {
        CheckWorkerHealth();
        _LastHealthCheck = now;
    }
}

// =============================================================================
// Logging / Reporting
// =============================================================================

void WorkerManager::PrintManagerBanner() const
{
    LOG_INFO("=== {} v{} - Multi-Tenant Mode ===", AppInfo::FullName, AppInfo::Version);
    LOG_INFO("{}", AppInfo::Copyright);
    LOG_INFO("ADM Database: {}:{}/{}", _Config.ADMHost,
             _Config.ADMPort, _Config.ADMDBName);
    LOG_INFO("Tenant source: SYSxTenant (SYSxStateID=1 Active)");
    LOG_INFO("Max workers: {}", _Config.MaxWorkers);
    LOG_INFO("Tenant poll interval: {}s", _Config.TenantPollIntervalSec);
    LOG_INFO("Health check interval: {}s", _Config.HealthCheckIntervalSec);
}

void WorkerManager::PrintFinalSummary() const
{
    LOG_INFO("=== WorkerManager Final Summary ===");

    auto snapshots = GetAllHealthSnapshots();
    int64_t totalTx = 0, totalChanges = 0, totalErrors = 0;

    for (auto& snap : snapshots)
    {
        totalTx      += snap.TotalTransactions;
        totalChanges += snap.TotalChanges;
        totalErrors  += snap.TotalErrors;

        LOG_INFO("  Tenant {} : state={} tx={} changes={} errors={} lsn={}",
                 snap.TenantIdentifier,
                 WorkerStateToString(snap.State),
                 snap.TotalTransactions,
                 snap.TotalChanges,
                 snap.TotalErrors,
                 FormatLSN(snap.ConfirmedLSN));
    }

    LOG_INFO("Total: {} tenants, {} transactions, {} changes, {} errors",
             snapshots.size(), totalTx, totalChanges, totalErrors);
    LOG_INFO("===================================");
}

// =============================================================================
// Graceful Shutdown: Confirm last LSN per tenant to ADM
// =============================================================================

void WorkerManager::ConfirmAllLSNToADM()
{
    if (!IsADMConnected())
    {
        LOG_WARN("ADM not connected during shutdown, cannot persist LSN checkpoints");
        return;
    }

    LOG_INFO("Persisting last confirmed LSN for all workers to ADM...");

    for (auto& [tenantID, worker] : _Workers)
    {
        auto snap = worker->TakeHealthSnapshot();
        if (snap.ConfirmedLSN == LSN_INVALID)
            continue;

        auto lsnStr   = FormatLSN(snap.ConfirmedLSN);
        auto slotName = std::string(worker->GetSlotName());

        auto sql = ADMSQL::UpsertTenantCheckpoint(tenantID, slotName, lsnStr);
        PGresult* res = PQexec(_ADMConn, sql.c_str());

        if (PQresultStatus(res) != PGRES_COMMAND_OK)
        {
            LOG_ERROR("Failed to persist LSN for tenant {}: {}",
                      tenantID, PQerrorMessage(_ADMConn));
        }
        else
        {
            LOG_INFO("Persisted LSN {} for tenant {} [{}]",
                     lsnStr, tenantID, worker->GetTenantName());
        }

        PQclear(res);
    }
}

} // namespace DC
