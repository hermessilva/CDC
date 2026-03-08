// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// COPY writer implementation
// =============================================================================
#include "CopyWriter.h"
#include "Logger.h"
#include <cstring>
#include <unordered_map>

namespace DC
{

CopyWriter::CopyWriter()
{
    _CopyBuffer.reserve(Performance::CopyRowAvgSize * Performance::DefaultBatchSize);
}

CopyWriter::~CopyWriter()
{
    Disconnect();
}

bool CopyWriter::Connect(const WriterConfig& config)
{
    _Config = config;
    Disconnect();

    auto connStr = std::format(
        "host='{}' port='{}' dbname='{}' user='{}' password='{}' "
        "connect_timeout='{}' application_name='{}' sslmode='{}'",
        config.Host, config.Port, config.DBName,
        config.User, config.Password,
        Connection::ConnectTimeoutSec,
        "DataCrusher-Writer",
        config.SSLMode
    );

    _Connection = PQconnectdb(connStr.c_str());
    if (PQstatus(_Connection) != CONNECTION_OK)
    {
        LOG_ERROR("Target writer connection failed: {}", PQerrorMessage(_Connection));
        Disconnect();
        return false;
    }

    LOG_INFO("Target writer connected to {}:{}/{}", config.Host, config.Port, config.DBName);
    return true;
}

void CopyWriter::Disconnect()
{
    if (_InCopyMode)
        EndCopy();

    if (_Connection)
    {
        PQfinish(_Connection);
        _Connection = nullptr;
    }
}

bool CopyWriter::IsConnected() const
{
    return _Connection && PQstatus(_Connection) == CONNECTION_OK;
}

bool CopyWriter::Bootstrap()
{
    if (!IsConnected()) return false;

    LOG_INFO("Bootstrapping Target CDC schema '{}'...", _Config.Schema);

    PGresult* res = PQexec(_Connection, SQL::BootstrapTargetSchema(_Config.Schema).c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("Bootstrap failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }
    PQclear(res);

    LOG_INFO("Target CDC schema '{}' bootstrapped successfully", _Config.Schema);
    return true;
}

bool CopyWriter::EnsureJournalTable(std::string_view sourceTable)
{
    if (!IsConnected()) return false;

    // Already known — skip CREATE check
    if (_KnownTables.count(std::string(sourceTable)))
        return true;

    auto ddl = SQL::CreateJournalTable(_Config.Schema, sourceTable);
    PGresult* res = PQexec(_Connection, ddl.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("EnsureJournalTable: failed to create {}: {}",
                  Tables::JournalTableFor(_Config.Schema, sourceTable), PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }
    PQclear(res);

    _KnownTables.emplace(sourceTable);
    LOG_DEBUG("Journal table {} ready", Tables::JournalTableFor(_Config.Schema, sourceTable));
    return true;
}

void CopyWriter::EnsureJournalTables(const std::vector<std::string>& sourceTables)
{
    int created = 0;
    for (const auto& tbl : sourceTables)
    {
        if (EnsureJournalTable(tbl))
            ++created;
    }
    LOG_INFO("Journal tables ready: {}/{} tables initialised in '{}' schema",
             created, sourceTables.size(), _Config.Schema);
}

bool CopyWriter::EnsureTableView(const std::string& sourceTable,
                                  const std::vector<SourceColumnInfo>& columns)
{
    if (!IsConnected()) return false;

    if (columns.empty())
    {
        LOG_WARN("EnsureTableView: no columns for '{}', skipping view", sourceTable);
        return false;
    }

    auto ddl = SQL::CreateOrReplaceTableView(_Config.Schema, sourceTable, columns);
    PGresult* res = PQexec(_Connection, ddl.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("EnsureTableView: failed for {}.\"{}\":  {}",
                  _Config.Schema, sourceTable, PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }
    PQclear(res);

    LOG_DEBUG("View {}.\"{}\" created/updated ({} columns)", _Config.Schema, sourceTable, columns.size());
    return true;
}

void CopyWriter::EnsureTableViews(
    const std::vector<std::pair<std::string, std::vector<SourceColumnInfo>>>& tableColumns)
{
    int created = 0;
    for (const auto& [tbl, cols] : tableColumns)
    {
        if (EnsureTableView(tbl, cols))
            ++created;
    }
    LOG_INFO("Table views ready: {}/{} views created/updated in '{}' schema",
             created, tableColumns.size(), _Config.Schema);
}

bool CopyWriter::WriteTransaction(const Transaction& tx)
{
    if (!IsConnected())
    {
        LOG_ERROR("Cannot write transaction: not connected to Target");
        return false;
    }

    if (tx.Changes.empty())
    {
        LOG_TRACE("Transaction xid={} has no changes, skipping write", tx.TransactionXID);
        return true;
    }

    auto startTime = SteadyClock::now();

    // Group changes by table name — each group gets its own COPY stream
    // Use a vector of (tableName, indices) to preserve insertion order
    std::vector<std::pair<std::string, std::vector<size_t>>> groups;
    {
        std::unordered_map<std::string, size_t> groupIndex;
        for (size_t i = 0; i < tx.Changes.size(); ++i)
        {
            const auto& tbl = tx.Changes[i].TableName;
            auto it = groupIndex.find(tbl);
            if (it == groupIndex.end())
            {
                groupIndex.emplace(tbl, groups.size());
                groups.push_back({tbl, {i}});
            }
            else
            {
                groups[it->second].second.push_back(i);
            }
        }
    }

    if (!ExecCommand("BEGIN"))
        return false;

    for (const auto& [tableName, indices] : groups)
    {
        // Lazy-create journal table if this table was not seen at startup
        if (!EnsureJournalTable(tableName))
        {
            LOG_ERROR("Cannot journal table '{}' — journal table creation failed", tableName);
            ExecCommand("ROLLBACK");
            return false;
        }

        auto journalTable = Tables::JournalTableFor(_Config.Schema, tableName);
        if (!BeginCopy(journalTable))
        {
            ExecCommand("ROLLBACK");
            return false;
        }

        for (size_t idx : indices)
            AppendCopyRow(tx.Changes[idx], tx);

        if (!EndCopy())
        {
            ExecCommand("ROLLBACK");
            return false;
        }
    }

    if (!ExecCommand("COMMIT"))
        return false;

    auto elapsed = std::chrono::duration_cast<Microseconds>(SteadyClock::now() - startTime);
    _Stats.LastFlushDurationUS = elapsed.count();
    _Stats.TotalRowsWritten += static_cast<int64_t>(tx.Changes.size());
    _Stats.TotalBatchesFlushed++;

    LOG_TRACE("Transaction xid={} written: {} rows across {} table(s) in {}us",
              tx.TransactionXID, tx.Changes.size(), groups.size(), elapsed.count());

    return true;
}

bool CopyWriter::BeginCopy(const std::string& journalTable)
{
    if (_InCopyMode) return true;

    auto copyCmd = SQL::CopyJournalTableIn(journalTable);
    PGresult* res = PQexec(_Connection, copyCmd.c_str());
    if (PQresultStatus(res) != PGRES_COPY_IN)
    {
        LOG_ERROR("COPY command failed for {}: {}", journalTable, PQerrorMessage(_Connection));
        PQclear(res);
        ++_Stats.TotalCopyErrors;
        return false;
    }
    PQclear(res);

    _InCopyMode = true;
    _CopyBuffer.clear();
    _PendingRows = 0;
    return true;
}

bool CopyWriter::EndCopy()
{
    if (!_InCopyMode) return true;

    // Send any remaining buffered data
    if (!_CopyBuffer.empty())
    {
        int rc = PQputCopyData(_Connection, _CopyBuffer.data(),
                                static_cast<int>(_CopyBuffer.size()));
        if (rc != 1)
        {
            LOG_ERROR("PQputCopyData failed: {}", PQerrorMessage(_Connection));
            PQputCopyEnd(_Connection, "write error");
            _InCopyMode = false;
            ++_Stats.TotalCopyErrors;
            return false;
        }
        _Stats.TotalBytesWritten += static_cast<int64_t>(_CopyBuffer.size());
        _CopyBuffer.clear();
    }

    // End COPY
    int rc = PQputCopyEnd(_Connection, nullptr);
    if (rc != 1)
    {
        LOG_ERROR("PQputCopyEnd failed: {}", PQerrorMessage(_Connection));
        _InCopyMode = false;
        ++_Stats.TotalCopyErrors;
        return false;
    }

    // Check result
    PGresult* res = PQgetResult(_Connection);
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("COPY completed with error: {}", PQerrorMessage(_Connection));
        PQclear(res);
        _InCopyMode = false;
        ++_Stats.TotalCopyErrors;
        return false;
    }
    PQclear(res);

    _InCopyMode = false;
    _PendingRows = 0;
    return true;
}

void CopyWriter::AppendCopyRow(const ChangeEvent& event, const Transaction& tx)
{
    // COPY columns (tab-separated, no schema_name/table_name — implicit in journal table name):
    // xid, lsn, operation, data, user_id, origin, committed_at, captured_at

    auto& buf = _CopyBuffer;

    // xid
    buf += std::to_string(tx.TransactionXID);
    buf += '\t';

    // lsn
    buf += FormatLSN(tx.CommitLSN);
    buf += '\t';

    // operation
    buf += event.Operation;
    buf += '\t';

    // data: delta JSON (INSERT=all new, UPDATE=changed fields only, DELETE=all old)
    buf += event.DeltaJSON();
    buf += '\t';

    // user_id
    if (tx.Context.UserID.empty())
        buf += "\\N";
    else
        buf += tx.Context.UserID;
    buf += '\t';

    // origin / app_name
    if (tx.Context.AppName.empty())
        buf += "\\N";
    else
        buf += tx.Context.AppName;
    buf += '\t';

    // committed_at
    buf += FormatPgTimestamp(tx.CommitTimestamp);
    buf += '\t';

    // captured_at
    buf += NowUTC();
    buf += '\n';

    ++_PendingRows;
}

LSN CopyWriter::ReadCheckpointLSN(std::string_view slotName)
{
    if (!IsConnected()) return LSN_INVALID;

    auto query = SQL::ReadCheckpointLSN(_Config.Schema, slotName);
    PGresult* res = PQexec(_Connection, query.c_str());

    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_WARN("Checkpoint read failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return LSN_INVALID;
    }

    LSN lsn = LSN_INVALID;
    if (PQntuples(res) > 0)
    {
        auto lsnStr = PQgetvalue(res, 0, 0);
        if (lsnStr)
        {
            lsn = ParseLSN(lsnStr);
            LOG_INFO("Read checkpoint LSN: {} ({})", lsnStr, FormatLSN(lsn));
        }
    }
    else
    {
        LOG_INFO("No checkpoint found for slot '{}', starting from scratch", slotName);
    }

    PQclear(res);
    return lsn;
}

bool CopyWriter::SaveCheckpointLSN(std::string_view slotName, LSN lsn)
{
    if (!IsConnected()) return false;

    auto sql = SQL::UpsertCheckpointLSN(_Config.Schema, slotName, lsn);
    PGresult* res = PQexec(_Connection, sql.c_str());

    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("Checkpoint save failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    PQclear(res);
    LOG_DEBUG("Checkpoint saved: slot='{}' lsn={}", slotName, FormatLSN(lsn));
    return true;
}

bool CopyWriter::FlushPending()
{
    if (_InCopyMode && _PendingRows > 0)
        return EndCopy();
    return true;
}

std::string CopyWriter::LastError() const
{
    if (_Connection)
        return PQerrorMessage(_Connection);
    return "Not connected";
}

std::string CopyWriter::FormatPgTimestamp(int64_t pgTimestampUS)
{
    // PostgreSQL timestamp: microseconds since 2000-01-01
    constexpr int64_t PG_EPOCH_OFFSET_US = 946'684'800'000'000LL;
    int64_t unixUS = pgTimestampUS + PG_EPOCH_OFFSET_US;
    int64_t unixSec = unixUS / 1'000'000;
    int64_t fracUS  = unixUS % 1'000'000;
    if (fracUS < 0) { fracUS += 1'000'000; unixSec--; }

    time_t tt = static_cast<time_t>(unixSec);
    struct tm tmBuf;
#if DC_PLATFORM_WINDOWS
    gmtime_s(&tmBuf, &tt);
#else
    gmtime_r(&tt, &tmBuf);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);

    if (fracUS > 0)
        return std::format("{}.{:06d}+00", buf, fracUS);
    return std::format("{}+00", buf);
}

std::string CopyWriter::NowUTC()
{
    auto now = SystemClock::now();
    auto timeT = SystemClock::to_time_t(now);
    auto us = std::chrono::duration_cast<Microseconds>(
        now.time_since_epoch()) % 1'000'000;

    struct tm tmBuf;
#if DC_PLATFORM_WINDOWS
    gmtime_s(&tmBuf, &timeT);
#else
    gmtime_r(&timeT, &tmBuf);
#endif

    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
    return std::format("{}.{:06d}+00", buf, us.count());
}

bool CopyWriter::ExecCommand(const std::string& sql)
{
    PGresult* res = PQexec(_Connection, sql.c_str());
    if (PQresultStatus(res) != PGRES_COMMAND_OK)
    {
        LOG_ERROR("SQL failed [{}]: {}", sql, PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

} // namespace DC
