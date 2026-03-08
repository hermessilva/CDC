// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// COPY writer: batched ingestion to Target journal via COPY FROM STDIN
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include "TransactionManager.h"
#include <libpq-fe.h>
#include <string>
#include <vector>
#include <utility>
#include <unordered_set>

namespace DC
{

// Configuration for the COPY writer
struct WriterConfig
{
    std::string Host;
    uint16_t    Port       = 5432;
    std::string DBName;
    std::string User;
    std::string Password;
    std::string SSLMode    = "prefer";
    std::string Schema     = "CDCx";   // Journal schema name (= TablePrefix from CDCxConfig)
    int         BatchSize  = Performance::DefaultBatchSize;
    int         FlushIntervalMS = Performance::DefaultFlushIntervalMS;
};

// Statistics for the writer
struct WriterStats
{
    int64_t TotalRowsWritten   = 0;
    int64_t TotalBytesWritten  = 0;
    int64_t TotalBatchesFlushed = 0;
    int64_t TotalCopyErrors    = 0;
    int64_t LastFlushDurationUS = 0;
};

class CopyWriter final
{
public:
    CopyWriter();
    ~CopyWriter();

    CopyWriter(const CopyWriter&) = delete;
    CopyWriter& operator=(const CopyWriter&) = delete;

    // Connect to Target database
    bool Connect(const WriterConfig& config);

    // Disconnect
    void Disconnect();

    // Check connection
    bool IsConnected() const;

    // Bootstrap: create CDC schema and checkpoint table if they don't exist
    bool Bootstrap();

    // Create the per-table journal table cdc."JNLx{sourceTable}" if it does not exist.
    // Called at startup for all known tables and lazily for new tables seen at runtime.
    bool EnsureJournalTable(std::string_view sourceTable);

    // Create journal tables for a list of source table names (startup bulk creation).
    void EnsureJournalTables(const std::vector<std::string>& sourceTables);

    // Drop and recreate the view cdc."{sourceTable}" over cdc."JNLx{sourceTable}".
    // Expands JSONB delta into typed columns + journal audit columns with JNLx prefix.
    // Called at startup so any schema changes to the source table are reflected.
    bool EnsureTableView(const std::string& sourceTable, const std::vector<SourceColumnInfo>& columns);

    // Recreate views for all tracked tables (startup bulk operation).
    void EnsureTableViews(const std::vector<std::pair<std::string, std::vector<SourceColumnInfo>>>& tableColumns);

    // Write a complete transaction to the journal via COPY
    // Returns true if all rows were written and committed
    bool WriteTransaction(const Transaction& tx);

    // Read the last confirmed LSN from the checkpoint table
    LSN ReadCheckpointLSN(std::string_view slotName);

    // Save the confirmed LSN to the checkpoint table (idempotent upsert)
    bool SaveCheckpointLSN(std::string_view slotName, LSN lsn);

    // Flush any pending data and end current COPY session
    bool FlushPending();

    // Get writer statistics
    const WriterStats& GetStats() const { return _Stats; }

    // Get last error
    std::string LastError() const;

private:
    PGconn*     _Connection = nullptr;
    WriterConfig _Config;
    WriterStats  _Stats;

    // Buffer for accumulating COPY rows
    std::string _CopyBuffer;
    int         _PendingRows = 0;

    // State tracking
    bool _InCopyMode = false;

    // Set of source table names that already have a journal table (avoids repeated CREATE checks)
    std::unordered_set<std::string> _KnownTables;

    // Start a COPY session targeting a specific journal table
    bool BeginCopy(const std::string& journalTable);

    // End the current COPY session (flush and commit)
    bool EndCopy();

    // Append a single change event as a COPY row to the buffer
    void AppendCopyRow(const ChangeEvent& event, const Transaction& tx);

    // Format PostgreSQL timestamp from internal microseconds
    static std::string FormatPgTimestamp(int64_t pgTimestampUS);

    // Get current UTC timestamp in ISO format
    static std::string NowUTC();

    // Execute a simple query
    bool ExecCommand(const std::string& sql);
};

} // namespace DC
