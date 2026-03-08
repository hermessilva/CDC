// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Per-tenant worker configuration (multi-tenancy abstraction)
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include "Logger.h"
#include "ReplicationStream.h"
#include "CopyWriter.h"
#include "ReconnectionManager.h"
#include <string>
#include <unordered_set>

namespace DC
{

// Unique identifier for a tenant (UUID string from SYSxTenant.ID)
using TenantID = std::string;
const TenantID TENANT_ID_INVALID = "";

// =============================================================================
// Health status of a running worker thread
// =============================================================================
enum class WorkerState : uint8_t
{
    Idle,           // Created but not yet started
    Connecting,     // Establishing connections
    Streaming,      // Active WAL streaming (healthy)
    Reconnecting,   // Lost connection, attempting recovery
    Paused,         // Intentionally paused by manager
    Stopping,       // Graceful shutdown requested
    Stopped,        // Thread exited normally
    Failed          // Unrecoverable error, thread exited
};

inline std::string_view WorkerStateToString(WorkerState state)
{
    switch (state)
    {
        case WorkerState::Idle:          return "IDLE";
        case WorkerState::Connecting:    return "CONNECTING";
        case WorkerState::Streaming:     return "STREAMING";
        case WorkerState::Reconnecting:  return "RECONNECTING";
        case WorkerState::Paused:        return "PAUSED";
        case WorkerState::Stopping:      return "STOPPING";
        case WorkerState::Stopped:       return "STOPPED";
        case WorkerState::Failed:        return "FAILED";
        default:                         return "UNKNOWN";
    }
}

// =============================================================================
// Per-tenant worker configuration
// Each tenant has its own source DB, journal DB, slot, and credentials.
// =============================================================================
struct TenantWorkerConfig
{
    // Tenant identity
    TenantID    TenantIdentifier = TENANT_ID_INVALID;
    std::string TenantName;       // Human-readable label (for logs)
    std::string TablePrefix = "CDCx";  // Schema name in journal DB + session table prefix

    // Table names (unqualified) that must NOT be journalled.
    // Populated by parsing CDCxConfig.UntrackedTables (";"-delimited).
    std::unordered_set<std::string> UntrackedTables;

    // Source database (Tenant's PostgreSQL instance)
    ReplicationConfig Source;

    // Destination database (Journal/Target for this tenant)
    WriterConfig Journal;

    // Reconnection policy (independent per tenant)
    ReconnectionPolicy Reconnection;

    // Bootstrap Target schema on first connection?
    bool BootstrapJournal = true;

    // Bootstrap signal table on source DB?
    bool BootstrapSignal = false;

    // LSN persistence file (fallback per tenant)
    std::string LSNFilePath;

    // Latency/stats reporting interval (0 = disabled)
    int LatencyReportIntervalSec = 60;
    int StatsReportIntervalSec   = 30;
};

// =============================================================================
// Health snapshot exposed to the manager (lock-free read via atomics)
// =============================================================================
struct WorkerHealthSnapshot
{
    TenantID    TenantIdentifier = TENANT_ID_INVALID;
    WorkerState State            = WorkerState::Idle;
    LSN         ConfirmedLSN     = LSN_INVALID;
    int64_t     TotalTransactions = 0;
    int64_t     TotalChanges      = 0;
    int64_t     TotalErrors       = 0;
    int         ReconnectionCount = 0;
    TimePoint   LastActivity;
};

// =============================================================================
// Global manager configuration (read from CLI / env / Target database)
// =============================================================================
struct ManagerConfig
{
    // ADM (centralized admin) database where SYSxTenant lives
    std::string ADMHost     = "127.0.0.1";
    uint16_t    ADMPort     = 5432;
    std::string ADMDBName   = "TokenGuard";
    std::string ADMUser;
    std::string ADMPassword;
    std::string ADMSSLMode  = "prefer";

    // How often the manager polls ADM for tenant changes (seconds)
    int TenantPollIntervalSec = 30;

    // How often the manager checks worker health (seconds)
    int HealthCheckIntervalSec = 10;

    // Maximum number of concurrent tenant workers
    int MaxWorkers = 64;

    // Logging
    LogLevel    LogLevelValue = LogLevel::Info;
    std::string LogFilePath;

    // Default reconnection for all tenants (can be overridden per-tenant)
    ReconnectionPolicy DefaultReconnection;

    // Default journal writer settings
    int DefaultBatchSize      = Performance::DefaultBatchSize;
    int DefaultFlushIntervalMS = Performance::DefaultFlushIntervalMS;

    // SSL mode applied to all tenant connections by default
    std::string DefaultSSLMode = "prefer";
};

} // namespace DC
