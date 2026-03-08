// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// WorkerManager: multi-tenant orchestrator that manages TenantWorker instances
// Reads tenant credentials from the centralized ADM database (SYSxTenant)
// =============================================================================
#pragma once

#include "Platform.h"
#include "TenantWorkerConfig.h"
#include "TenantWorker.h"
#include <unordered_map>
#include <memory>
#include <atomic>
#include <vector>

// Forward declaration for libpq
struct pg_conn;
typedef struct pg_conn PGconn;

namespace DC
{

// =============================================================================
// WorkerManager: the multi-tenant orchestrator.
//
// Connects to the centralized ADM database, queries CDCxConfig for active
// CDC configurations, and spawns one TenantWorker (std::jthread) per entry.
// Periodically polls for configuration changes (new, removed, reconfigured)
// and monitors worker health via lock-free snapshots.
//
// Thread model:
//   - WorkerManager runs on the main thread (blocking Run() loop)
//   - Each TenantWorker runs on its own std::jthread
//   - ADM PGconn is owned exclusively by the manager (no sharing)
//   - Workers share NOTHING except the Logger singleton
// =============================================================================
class WorkerManager final
{
public:
    WorkerManager();
    ~WorkerManager();

    WorkerManager(const WorkerManager&) = delete;
    WorkerManager& operator=(const WorkerManager&) = delete;

    // Initialize manager: connect to ADM database, set up signal handlers.
    // Returns false on fatal initialization error.
    bool Initialize(const ManagerConfig& config);

    // Run the manager loop (blocking). Polls tenants, monitors health.
    // Returns process exit code.
    int Run();

    // Request graceful shutdown of all workers and the manager loop.
    void RequestShutdown();

    // Check if shutdown was requested.
    bool ShutdownRequested() const { return _ShutdownRequested.load(std::memory_order_relaxed); }

    // Get health snapshots for all currently running workers.
    std::vector<WorkerHealthSnapshot> GetAllHealthSnapshots() const;

    // Get current number of active workers.
    int ActiveWorkerCount() const;

private:
    ManagerConfig _Config;

    // ADM database connection (owned by manager, never shared)
    PGconn* _ADMConn = nullptr;

    // Active workers: TenantID (UUID) -> worker instance
    std::unordered_map<TenantID, std::unique_ptr<TenantWorker>> _Workers;

    // Shutdown flag
    std::atomic<bool> _ShutdownRequested{false};

    // Timing for periodic tasks
    TimePoint _LastTenantPoll;
    TimePoint _LastHealthCheck;

    // --------------- ADM Connection --------------------------------------
    bool ConnectADM();
    void DisconnectADM();
    bool IsADMConnected() const;
    bool ReconnectADM();

    // --------------- Tenant Discovery (CDCxConfig) -----------------------
    // Query ADM for active CDCxConfig entries and reconcile with running workers.
    bool RefreshTenantList();

    // Build a TenantWorkerConfig from a CDCxConfig row.
    TenantWorkerConfig BuildWorkerConfig(
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
    ) const;

    // Start a new worker for the given tenant.
    bool StartWorker(TenantWorkerConfig config);

    // Stop and remove a worker for the given tenant.
    void StopWorker(const TenantID& tenantID);

    // Stop all running workers gracefully.
    void StopAllWorkers();

    // --------------- Graceful Shutdown ------------------------------------
    // Confirm the last processed LSN per tenant to the ADM database
    // before closing. This ensures restart consistency.
    void ConfirmAllLSNToADM();

    // --------------- Health Monitoring -----------------------------------
    void CheckWorkerHealth();

    // Remove workers that have finished (stopped/failed) and log the reason.
    void ReapFinishedWorkers();

    // --------------- Periodic Tasks --------------------------------------
    void RunPeriodicTenantPoll();
    void RunPeriodicHealthCheck();

    // --------------- Logging / Reporting ---------------------------------
    void PrintManagerBanner() const;
    void PrintFinalSummary() const;

    // --------------- Helpers ---------------------------------------------
    static std::string BuildADMConnString(const ManagerConfig& config);
};

} // namespace DC
