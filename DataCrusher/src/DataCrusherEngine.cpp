// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Engine orchestrator: multi-tenant CDC via SYSxTenant (ADM database)
// =============================================================================
#include "DataCrusherEngine.h"
#include <csignal>
#include <cstdlib>
#include <algorithm>

namespace DC
{

// Global engine pointer for signal handling
static DataCrusherEngine* g_Engine = nullptr;

static void SignalHandler(int signal)
{
    if (g_Engine)
    {
        LOG_INFO("Signal {} received, requesting shutdown...", signal);
        g_Engine->RequestShutdown();
    }
}

DataCrusherEngine::DataCrusherEngine()
{
    g_Engine = this;
}

DataCrusherEngine::~DataCrusherEngine()
{
    g_Engine = nullptr;
}

void DataCrusherEngine::RequestShutdown()
{
    _Manager.RequestShutdown();
}

bool DataCrusherEngine::Initialize(const EngineConfig& config)
{
    _Config = config;
    // NOTE: ResolveEnvOverrides is called by Main BEFORE CLI parsing
    //       so that CLI arguments take priority over env vars.

    // Initialize logger
    Logger::Instance().Initialize(_Config.LogLevelValue, _Config.LogFilePath);

    LOG_INFO("=== {} v{} ===", AppInfo::FullName, AppInfo::Version);
    LOG_INFO("{}", AppInfo::Copyright);

    // Install signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
#if DC_PLATFORM_LINUX
    std::signal(SIGHUP, SignalHandler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // Initialize type handlers
    TypeHandlers::Instance().InitializeBuiltins();

    // Transfer common settings to manager config
    _Config.Manager.LogLevelValue       = _Config.LogLevelValue;
    _Config.Manager.LogFilePath         = _Config.LogFilePath;
    _Config.Manager.DefaultReconnection = _Config.Reconnection;

    LOG_INFO("Engine initialized, delegating to WorkerManager");
    return true;
}

int DataCrusherEngine::Run()
{
    if (!_Manager.Initialize(_Config.Manager))
    {
        LOG_FATAL("WorkerManager initialization failed");
        return ExitCode::FatalError;
    }

    return _Manager.Run();
}

// =============================================================================
// Environment Variable Resolution
// =============================================================================

std::string DataCrusherEngine::GetEnvOrDefault(std::string_view envName, std::string_view defaultVal)
{
    const char* val = std::getenv(std::string(envName).c_str());
    if (val && *val)
        return val;
    return std::string(defaultVal);
}

int DataCrusherEngine::GetEnvIntOrDefault(std::string_view envName, int defaultVal)
{
    const char* val = std::getenv(std::string(envName).c_str());
    if (val && *val)
    {
        try { return std::stoi(val); }
        catch (...) { return defaultVal; }
    }
    return defaultVal;
}

void DataCrusherEngine::ResolveEnvOverrides(EngineConfig& config)
{
    // Log level override
    auto logLevelStr = GetEnvOrDefault(EnvVar::LogLevel, "");
    if (!logLevelStr.empty())
        config.LogLevelValue = ParseLogLevel(logLevelStr);

    // Reconnection overrides
    config.Reconnection.BaseDelayMS = GetEnvIntOrDefault(
        EnvVar::ReconnectBaseMS, config.Reconnection.BaseDelayMS);
    config.Reconnection.MaxDelayMS = GetEnvIntOrDefault(
        EnvVar::ReconnectMaxMS, config.Reconnection.MaxDelayMS);
    config.Reconnection.MaxRetries = GetEnvIntOrDefault(
        EnvVar::ReconnectMaxRetries, config.Reconnection.MaxRetries);

    // ADM bootstrap overrides
    config.Manager.ADMHost     = GetEnvOrDefault(EnvVar::ADMHost, config.Manager.ADMHost);
    config.Manager.ADMPort     = static_cast<uint16_t>(GetEnvIntOrDefault(
                                      EnvVar::ADMPort, config.Manager.ADMPort));
    config.Manager.ADMDBName   = GetEnvOrDefault(EnvVar::ADMDBName, config.Manager.ADMDBName);
    config.Manager.ADMUser     = GetEnvOrDefault(EnvVar::ADMUser, config.Manager.ADMUser);
    config.Manager.ADMPassword = GetEnvOrDefault(EnvVar::ADMPassword, config.Manager.ADMPassword);
    config.Manager.MaxWorkers       = GetEnvIntOrDefault(EnvVar::MaxWorkers, config.Manager.MaxWorkers);
    config.Manager.TenantPollIntervalSec  = GetEnvIntOrDefault(EnvVar::TenantPollInterval, config.Manager.TenantPollIntervalSec);
    config.Manager.HealthCheckIntervalSec = GetEnvIntOrDefault(EnvVar::HealthCheckInterval, config.Manager.HealthCheckIntervalSec);
}

} // namespace DC
