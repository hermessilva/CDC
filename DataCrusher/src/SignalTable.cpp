// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Signal table implementation
// =============================================================================
#include "SignalTable.h"
#include "Logger.h"

namespace DC
{

SignalTable::SignalTable() = default;

SignalTable::~SignalTable()
{
    Disconnect();
}

bool SignalTable::Connect(const std::string& host, uint16_t port,
                           const std::string& dbname, const std::string& user,
                           const std::string& password, const std::string& sslMode)
{
    Disconnect();

    auto connStr = std::format(
        "host='{}' port='{}' dbname='{}' user='{}' password='{}' "
        "connect_timeout='{}' application_name='{}' sslmode='{}'",
        host, port, dbname, user, password,
        Connection::ConnectTimeoutSec,
        "DataCrusher-Signal",
        sslMode
    );

    _Connection = PQconnectdb(connStr.c_str());
    if (PQstatus(_Connection) != CONNECTION_OK)
    {
        LOG_ERROR("Signal table connection failed: {}", PQerrorMessage(_Connection));
        Disconnect();
        return false;
    }

    LOG_INFO("Signal table connection established to {}:{}/{}", host, port, dbname);
    return true;
}

void SignalTable::Disconnect()
{
    if (_Connection)
    {
        PQfinish(_Connection);
        _Connection = nullptr;
    }
}

bool SignalTable::IsConnected() const
{
    return _Connection && PQstatus(_Connection) == CONNECTION_OK;
}

std::optional<SessionContext> SignalTable::QueryContext([[maybe_unused]] int backendPID)
{
    if (!IsConnected())
    {
        LOG_WARN("Signal table not connected, cannot query context");
        return std::nullopt;
    }

    // Best-effort fallback: most recent session row.
    // Precise correlation happens in-stream via _SessionCache in TenantWorker.
    auto signalTable = Tables::SignalTableFor(_TablePrefix);
    auto query = std::format(
        "SELECT {}, {} FROM {} ORDER BY {} DESC LIMIT 1",
        SignalColumns::UserID, SignalColumns::AppName,
        signalTable,
        SignalColumns::Moment
    );

    PGresult* res = PQexec(_Connection, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("Signal table query failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0)
    {
        PQclear(res);
        return std::nullopt;
    }

    SessionContext ctx;
    auto userVal = PQgetvalue(res, 0, 0);
    auto originVal = PQgetvalue(res, 0, 1);

    ctx.UserID  = userVal   ? userVal   : "";
    ctx.AppName = originVal ? originVal : "";

    PQclear(res);

    LOG_TRACE("Signal context: user_id='{}' app_name='{}'", ctx.UserID, ctx.AppName);
    return ctx;
}

std::optional<SessionContext> SignalTable::QueryContextByXID([[maybe_unused]] XID xid)
{
    if (!IsConnected())
    {
        LOG_WARN("Signal table not connected for XID context query");
        return std::nullopt;
    }

    // Alternatively query by matching the latest update to the signal table
    // This is a best-effort approach since XIDs are internal to PostgreSQL
    auto signalTable = Tables::SignalTableFor(_TablePrefix);
    auto query = std::format(
        "SELECT {}, {} FROM {} ORDER BY {} DESC LIMIT 1",
        SignalColumns::UserID, SignalColumns::AppName,
        signalTable, SignalColumns::Moment
    );

    PGresult* res = PQexec(_Connection, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("Signal table XID query failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return std::nullopt;
    }

    if (PQntuples(res) == 0)
    {
        PQclear(res);
        return std::nullopt;
    }

    SessionContext ctx;
    ctx.UserID  = PQgetvalue(res, 0, 0);
    ctx.AppName = PQgetvalue(res, 0, 1);

    PQclear(res);
    return ctx;
}

bool SignalTable::Bootstrap(std::string_view tablePrefix)
{
    if (!IsConnected())
    {
        LOG_WARN("Signal connection not available, cannot bootstrap signal table");
        return false;
    }

    _TablePrefix = std::string(tablePrefix);

    PGresult* res = PQexec(_Connection, DC::SQL::BootstrapSignalTable(tablePrefix).c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        // "already exists" is expected, treat as success
        LOG_WARN("Signal table bootstrap: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return true; // non-fatal
    }

    PQclear(res);
    LOG_INFO("Signal table 'public.\"{}Session\"' ready", _TablePrefix);
    return true;
}

bool SignalTable::EnsurePublication(std::string_view publicationName)
{
    if (!IsConnected())
    {
        LOG_WARN("Signal connection not available, cannot verify publication '{}'",
                 publicationName);
        return false;
    }

    // Check if publication already exists
    auto checkQuery = std::format(
        "SELECT pubname FROM pg_publication WHERE pubname = '{}'",
        publicationName
    );

    PGresult* res = PQexec(_Connection, checkQuery.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("Failed to query pg_publication: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    if (PQntuples(res) > 0)
    {
        LOG_INFO("Publication '{}' exists", publicationName);
        PQclear(res);
        return true;
    }
    PQclear(res);

    // Publication missing — try FOR ALL TABLES first (requires superuser)
    LOG_WARN("Publication '{}' not found, creating FOR ALL TABLES...", publicationName);

    auto createSQL = std::format("CREATE PUBLICATION {} FOR ALL TABLES", publicationName);
    res = PQexec(_Connection, createSQL.c_str());
    if (PQresultStatus(res) == PGRES_COMMAND_OK)
    {
        PQclear(res);
        LOG_INFO("Publication '{}' created successfully (FOR ALL TABLES)", publicationName);
        return true;
    }

    // Superuser required but not available — fall back to per-table publication
    std::string forAllErr = PQerrorMessage(_Connection);
    PQclear(res);
    LOG_WARN("FOR ALL TABLES not allowed ({}), falling back to per-table publication...",
             forAllErr);

    // Enumerate all user tables the current role can see
    const char* listSQL =
        "SELECT quote_ident(table_schema) || '.' || quote_ident(table_name) "
        "FROM information_schema.tables "
        "WHERE table_type = 'BASE TABLE' "
        "  AND table_schema NOT IN ('pg_catalog', 'information_schema') "
        "ORDER BY table_schema, table_name";

    res = PQexec(_Connection, listSQL);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_ERROR("Failed to enumerate tables for publication: {}",
                  PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    int tableCount = PQntuples(res);
    if (tableCount == 0)
    {
        PQclear(res);
        LOG_ERROR("No user tables found — cannot create publication '{}'. "
                  "Run manually as superuser: CREATE PUBLICATION {} FOR ALL TABLES",
                  publicationName, publicationName);
        return false;
    }

    std::string tableList;
    tableList.reserve(static_cast<size_t>(tableCount) * 32);
    for (int i = 0; i < tableCount; ++i)
    {
        if (i > 0) tableList += ", ";
        tableList += PQgetvalue(res, i, 0);
    }
    PQclear(res);

    createSQL = std::format("CREATE PUBLICATION {} FOR TABLE {}", publicationName, tableList);
    res = PQexec(_Connection, createSQL.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("Failed to create per-table publication '{}': {}",
                  publicationName, PQerrorMessage(_Connection));
        LOG_ERROR("Run manually as superuser: CREATE PUBLICATION {} FOR ALL TABLES",
                  publicationName);
        PQclear(res);
        return false;
    }

    PQclear(res);
    LOG_INFO("Publication '{}' created for {} tables (per-table fallback)",
             publicationName, tableCount);
    return true;
}

std::vector<std::string> SignalTable::GetPublicationTables(std::string_view publicationName)
{
    std::vector<std::string> tables;
    if (!IsConnected())
    {
        LOG_WARN("Signal connection unavailable, cannot enumerate publication tables");
        return tables;
    }

    // pg_publication_tables gives the exact set of tables in the publication
    auto query = std::format(
        "SELECT tablename FROM pg_publication_tables "
        "WHERE pubname = '{}' ORDER BY tablename",
        publicationName);

    PGresult* res = PQexec(_Connection, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        // Fallback: FOR ALL TABLES publications don't appear in pg_publication_tables
        // in older PG versions — enumerate information_schema instead
        PQclear(res);
        LOG_DEBUG("pg_publication_tables query failed, falling back to information_schema");

        const char* fallback =
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_type = 'BASE TABLE' "
            "  AND table_schema NOT IN ('pg_catalog', 'information_schema') "
            "ORDER BY table_name";
        res = PQexec(_Connection, fallback);
        if (PQresultStatus(res) != PGRES_TUPLES_OK)
        {
            LOG_ERROR("Cannot enumerate tables for journal creation: {}",
                      PQerrorMessage(_Connection));
            PQclear(res);
            return tables;
        }
    }

    int count = PQntuples(res);
    tables.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
        tables.emplace_back(PQgetvalue(res, i, 0));

    PQclear(res);
    LOG_INFO("Publication '{}' covers {} table(s)", publicationName, tables.size());
    return tables;
}

void SignalTable::EnsureReplicaIdentityFull()
{
    if (!IsConnected())
    {
        LOG_WARN("Signal connection not available, cannot set REPLICA IDENTITY FULL");
        return;
    }

    // Enumerate user tables that do NOT yet have REPLICA IDENTITY FULL (relreplident != 'f')
    const char* listSQL =
        "SELECT quote_ident(n.nspname) || '.' || quote_ident(c.relname) "
        "FROM pg_class c "
        "JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE c.relkind = 'r' "
        "  AND c.relreplident <> 'f' "
        "  AND n.nspname NOT IN ('pg_catalog', 'information_schema', 'pg_toast') "
        "ORDER BY n.nspname, c.relname";

    PGresult* res = PQexec(_Connection, listSQL);
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("EnsureReplicaIdentityFull: table enumeration failed: {}",
                 PQerrorMessage(_Connection));
        PQclear(res);
        return;
    }

    int count = PQntuples(res);
    if (count == 0)
    {
        PQclear(res);
        LOG_INFO("All user tables already have REPLICA IDENTITY FULL");
        return;
    }

    int ok = 0, fail = 0;
    for (int i = 0; i < count; ++i)
    {
        std::string tbl = PQgetvalue(res, i, 0);
        auto alterSQL   = std::format("ALTER TABLE {} REPLICA IDENTITY FULL", tbl);
        PGresult* ar    = PQexec(_Connection, alterSQL.c_str());
        if (PQresultStatus(ar) == PGRES_COMMAND_OK)
        {
            LOG_DEBUG("REPLICA IDENTITY FULL set on {}", tbl);
            ++ok;
        }
        else
        {
            LOG_WARN("Cannot set REPLICA IDENTITY FULL on {}: {} "
                     "(old values will not appear in UPDATE/DELETE logs)",
                     tbl, PQerrorMessage(_Connection));
            ++fail;
        }
        PQclear(ar);
    }
    PQclear(res);

    if (ok > 0)
        LOG_INFO("EnsureReplicaIdentityFull: {} table(s) updated, {} failed", ok, fail);
}

std::vector<SourceColumnInfo> SignalTable::GetTableColumns(const std::string& tableName)
{
    std::vector<SourceColumnInfo> result;
    if (!IsConnected()) return result;

    const char* params[1] = { tableName.c_str() };
    PGresult* res = PQexecParams(_Connection,
        "SELECT column_name, udt_name "
        "FROM information_schema.columns "
        "WHERE table_schema = 'public' AND table_name = $1 "
        "ORDER BY ordinal_position",
        1, nullptr, params, nullptr, nullptr, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("GetTableColumns: query failed for '{}': {}", tableName, PQerrorMessage(_Connection));
        PQclear(res);
        return result;
    }

    int rows = PQntuples(res);
    result.reserve(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i)
        result.push_back({ PQgetvalue(res, i, 0), PQgetvalue(res, i, 1) });

    PQclear(res);
    LOG_DEBUG("GetTableColumns: '{}' has {} columns", tableName, result.size());
    return result;
}

std::string SignalTable::LastError() const
{
    if (_Connection)
        return PQerrorMessage(_Connection);
    return "Not connected";
}

} // namespace DC
