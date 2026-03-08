// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Main entry point: CLI argument parsing and engine startup (multi-tenant)
// =============================================================================
#include "DataCrusherEngine.h"
#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>

#if DC_PLATFORM_WINDOWS
#include <Windows.h>
#endif

static void InitConsole()
{
#if DC_PLATFORM_WINDOWS
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

static void PrintBanner()
{
    std::cerr << "\n"
              << "  +====================================================+\n"
              << "  |           DataCrusher CDC Engine v"
              << DC::AppInfo::Version << "            |\n"
              << "  |   High-Performance PostgreSQL Change Data Capture   |\n"
              << "  |   " << DC::AppInfo::Copyright << "  |\n"
              << "  +====================================================+\n"
              << "\n";
}

static void PrintUsage(const char* progName)
{
    std::cerr << "Usage: " << progName << " [OPTIONS]\n\n"
              << "ADM Database (required):\n"
              << "  --adm-host <host>          ADM database host (env: DC_ADM_HOST, default: 127.0.0.1)\n"
              << "  --adm-port <port>          ADM database port (env: DC_ADM_PORT, default: 5432)\n"
              << "  --adm-db <name>            ADM database name (env: DC_ADM_DBNAME, default: TokenGuard)\n"
              << "  --adm-user <user>          ADM user (env: DC_ADM_USER) [REQUIRED]\n"
              << "  --adm-password <pass>      ADM password (prefer DC_ADM_PASSWORD env) [REQUIRED]\n\n"
              << "Worker Options:\n"
              << "  --max-workers <n>          Max concurrent tenant workers (default: 64)\n"
              << "  --tenant-poll <sec>        Tenant list poll interval (default: 30)\n"
              << "  --health-check <sec>       Health check interval (default: 10)\n\n"
              << "Reconnection:\n"
              << "  --reconnect-base <ms>      Base reconnection delay (default: 1000)\n"
              << "  --reconnect-max <ms>       Max reconnection delay (default: 30000)\n"
              << "  --reconnect-retries <n>    Max reconnection attempts (default: 10)\n\n"
              << "Logging:\n"
              << "  --log-level <level>        Log level: trace|debug|info|warn|error|fatal (default: info)\n"
              << "  --log-file <path>          Log file path (default: stderr only)\n\n"
              << "General:\n"
              << "  --version                  Print version and exit\n"
              << "  --help                     Print this help and exit\n\n"
              << "Environment variables:\n"
              << "  DC_ADM_HOST, DC_ADM_PORT, DC_ADM_DBNAME, DC_ADM_USER, DC_ADM_PASSWORD\n"
              << "  DC_MAX_WORKERS, DC_TENANT_POLL_INTERVAL, DC_HEALTH_CHECK_INTERVAL\n"
              << "  DC_LOG_LEVEL, DC_RECONNECT_BASE_MS, DC_RECONNECT_MAX_MS, DC_RECONNECT_MAX_RETRIES\n\n";
}

static bool ParseArgs(int argc, char* argv[], DC::EngineConfig& config)
{
    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        auto nextArg = [&]() -> std::string
        {
            if (i + 1 < argc) return argv[++i];
            std::cerr << "Error: " << arg << " requires an argument\n";
            return "";
        };

        if (arg == "--help" || arg == "-h")
        {
            PrintUsage(argv[0]);
            std::exit(DC::ExitCode::Success);
        }
        else if (arg == "--version" || arg == "-v")
        {
            std::cerr << DC::AppInfo::FullName << " v" << DC::AppInfo::Version << "\n";
            std::exit(DC::ExitCode::Success);
        }
        // ADM database
        else if (arg == "--adm-host")         { config.Manager.ADMHost = nextArg(); }
        else if (arg == "--adm-port")         { config.Manager.ADMPort = static_cast<uint16_t>(std::stoi(nextArg())); }
        else if (arg == "--adm-db")           { config.Manager.ADMDBName = nextArg(); }
        else if (arg == "--adm-user")         { config.Manager.ADMUser = nextArg(); }
        else if (arg == "--adm-password")     { config.Manager.ADMPassword = nextArg(); }
        // Workers
        else if (arg == "--max-workers")      { config.Manager.MaxWorkers = std::stoi(nextArg()); }
        else if (arg == "--tenant-poll")      { config.Manager.TenantPollIntervalSec = std::stoi(nextArg()); }
        else if (arg == "--health-check")     { config.Manager.HealthCheckIntervalSec = std::stoi(nextArg()); }
        // Reconnection
        else if (arg == "--reconnect-base")   { config.Reconnection.BaseDelayMS = std::stoi(nextArg()); }
        else if (arg == "--reconnect-max")    { config.Reconnection.MaxDelayMS = std::stoi(nextArg()); }
        else if (arg == "--reconnect-retries") { config.Reconnection.MaxRetries = std::stoi(nextArg()); }
        // Logging
        else if (arg == "--log-level")        { config.LogLevelValue = DC::ParseLogLevel(nextArg()); }
        else if (arg == "--log-file")         { config.LogFilePath = nextArg(); }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return false;
        }
    }

    return true;
}

int main(int argc, char* argv[])
{
    InitConsole();
    PrintBanner();

    DC::EngineConfig config;

    // ADM defaults
    config.Manager.ADMHost    = std::string(DC::Connection::DefaultHost);
    config.Manager.ADMPort    = DC::Connection::DefaultPort;
    config.Manager.ADMSSLMode = std::string(DC::Connection::SSLMode);

    // Layer 1: environment variables override hardcoded defaults
    DC::DataCrusherEngine::ResolveEnvOverrides(config);

    // Layer 2: CLI arguments override env vars (highest priority)
    if (!ParseArgs(argc, argv, config))
        return DC::ExitCode::InvalidArguments;

    // Validate mandatory ADM credentials
    if (config.Manager.ADMUser.empty() || config.Manager.ADMPassword.empty())
    {
        std::cerr << "Error: ADM credentials are required\n"
                  << "       Set via --adm-user/--adm-password or DC_ADM_USER/DC_ADM_PASSWORD env vars\n";
        PrintUsage(argv[0]);
        return DC::ExitCode::InvalidArguments;
    }

    // Create and run engine
    DC::DataCrusherEngine engine;

    if (!engine.Initialize(config))
    {
        std::cerr << "Fatal: Engine initialization failed\n";
        return DC::ExitCode::FatalError;
    }

    return engine.Run();
}
