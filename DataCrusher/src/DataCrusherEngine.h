// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Main engine orchestrator: multi-tenant CDC via SYSxTenant (ADM database)
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include "Logger.h"
#include "TypeHandlers.h"
#include "TenantWorkerConfig.h"
#include "WorkerManager.h"
#include <atomic>

namespace DC
{

// Full engine configuration (assembled from CLI args + env vars + defaults)
struct EngineConfig
{
    // Multi-Tenant Manager (reads tenants from ADM database)
    ManagerConfig Manager;

    // Reconnection policy (default for all tenants)
    ReconnectionPolicy Reconnection;

    // Logging
    LogLevel    LogLevelValue  = LogLevel::Info;
    std::string LogFilePath;
};

class DataCrusherEngine final
{
public:
    DataCrusherEngine();
    ~DataCrusherEngine();

    DataCrusherEngine(const DataCrusherEngine&) = delete;
    DataCrusherEngine& operator=(const DataCrusherEngine&) = delete;

    // Initialize the engine with configuration
    bool Initialize(const EngineConfig& config);

    // Run the manager loop (blocking). Returns exit code.
    int Run();

    // Request graceful shutdown
    void RequestShutdown();

    // Resolve configuration: merge env vars over hardcoded defaults.
    // Call BEFORE CLI parsing so that CLI arguments take priority.
    static void ResolveEnvOverrides(EngineConfig& config);

private:
    EngineConfig  _Config;
    WorkerManager _Manager;

    // Environment helpers
    static std::string GetEnvOrDefault(std::string_view envName, std::string_view defaultVal);
    static int          GetEnvIntOrDefault(std::string_view envName, int defaultVal);
};

} // namespace DC
