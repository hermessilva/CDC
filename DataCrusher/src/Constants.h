// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Constants, configuration, and naming conventions
// =============================================================================
#pragma once

#include "Platform.h"
#include <string>
#include <string_view>
#include <array>
#include <cstdint>
#include <vector>

namespace DC
{

// Column metadata queried from the source database's information_schema
struct SourceColumnInfo
{
    std::string Name;     // e.g. "ID", "Name"
    std::string UdtName;  // PostgreSQL udt_name, e.g. "uuid", "int4", "varchar"
};

// =============================================================================
// Application Identity
// =============================================================================
namespace AppInfo
{
    constexpr std::string_view Name        = "DataCrusher";
    constexpr std::string_view FullName    = "DataCrusher CDC Engine";
    constexpr std::string_view Version     = "1.0.0";
    constexpr std::string_view Copyright   = "Tootega Pesquisa e Inovacao (C) 1999-2026";
    constexpr std::string_view Description = "High-Performance PostgreSQL Change Data Capture via Logical Decoding";
}

// =============================================================================
// Replication Slot Configuration
// =============================================================================
namespace Replication
{
    // Slot name used for logical replication in PostgreSQL
    constexpr std::string_view SlotName        = "datacrusher_cdc_slot";

    // Output plugin: pgoutput (binary, native) is preferred for performance
    constexpr std::string_view OutputPlugin     = "pgoutput";

    // Alternative plugin (fallback)
    constexpr std::string_view OutputPluginAlt  = "wal2json";

    // Publication name for pgoutput filtering
    constexpr std::string_view PublicationName  = "datacrusher_publication";

    // Replication connection parameter
    constexpr std::string_view ReplicationMode  = "database";

    // Protocol version for pgoutput
    constexpr int ProtocolVersion = 4;

    // Streaming mode: enable transaction streaming for large TXs
    constexpr bool StreamingEnabled = true;
}

// =============================================================================
// Connection Defaults
// =============================================================================
namespace Connection
{
    // Connection timeouts (seconds)
    constexpr int ConnectTimeoutSec  = 10;
    constexpr int StatementTimeoutMS = 30'000;

    // Application name reported to PostgreSQL
    constexpr std::string_view ApplicationName = "DataCrusher";

    // SSL mode: disable (encryption removed — no SSL/TLS for database connections)
    constexpr std::string_view SSLMode = "disable";

    // Default host/port fallback when not provided by tenant registry
    constexpr std::string_view DefaultHost = "127.0.0.1";
    constexpr uint16_t         DefaultPort = 5432;
}

// =============================================================================
// Credential Scrubbing Utility
// =============================================================================
namespace Security
{
    // Zero-fill a string to scrub sensitive data from RAM
    inline void SecureErase(std::string& secret)
    {
        if (secret.empty()) return;
        volatile char* p = secret.data();
        for (size_t i = 0; i < secret.size(); ++i)
            p[i] = '\0';
        secret.clear();
    }
}

// =============================================================================
// Environment Variable Names
// =============================================================================
namespace EnvVar
{
    // ADM (centralized admin database) bootstrap credentials
    constexpr std::string_view ADMHost     = "DC_ADM_HOST";
    constexpr std::string_view ADMPort     = "DC_ADM_PORT";
    constexpr std::string_view ADMUser     = "DC_ADM_USER";
    constexpr std::string_view ADMPassword = "DC_ADM_PASSWORD";
    constexpr std::string_view ADMDBName   = "DC_ADM_DBNAME";

    // General engine tuning
    constexpr std::string_view SlotName       = "DC_SLOT_NAME";
    constexpr std::string_view PublicationName = "DC_PUBLICATION_NAME";
    constexpr std::string_view LogLevel       = "DC_LOG_LEVEL";
    constexpr std::string_view BatchSize      = "DC_BATCH_SIZE";
    constexpr std::string_view FlushIntervalMS = "DC_FLUSH_INTERVAL_MS";

    constexpr std::string_view SSLMode        = "DC_SSL_MODE";
    constexpr std::string_view LSNFile        = "DC_LSN_FILE";

    constexpr std::string_view ReconnectBaseMS  = "DC_RECONNECT_BASE_MS";
    constexpr std::string_view ReconnectMaxMS   = "DC_RECONNECT_MAX_MS";
    constexpr std::string_view ReconnectMaxRetries = "DC_RECONNECT_MAX_RETRIES";

    // Manager-mode environment variables (multi-tenant)
    constexpr std::string_view MaxWorkers       = "DC_MAX_WORKERS";
    constexpr std::string_view TenantPollInterval = "DC_TENANT_POLL_INTERVAL";
    constexpr std::string_view HealthCheckInterval = "DC_HEALTH_CHECK_INTERVAL";
}

// =============================================================================
// Table Names (Target Database - Journal/Audit Target)
// =============================================================================
namespace Tables
{
    // Journal table name = "{schema}"."{schema}{sourceTable}"
    // e.g. ("CDCx", "DOCxContato") → "CDCx"."CDCxDOCxContato"

    // Returns the per-table journal table name for a given source table.
    inline std::string JournalTableFor(std::string_view schema, std::string_view sourceTable)
    {
        return std::format("\"{0}\".\"{0}{1}\"", schema, sourceTable);
    }

    // Returns the view name for a source table (same name, in journal schema).
    inline std::string ViewTableFor(std::string_view schema, std::string_view sourceTable)
    {
        return std::format("\"{}\".\"{}\"", schema, sourceTable);
    }

    // LSN Checkpoint table in the journal schema.
    inline std::string CheckpointTableFor(std::string_view schema)
    {
        return std::format("\"{}\".\"Checkpoint\"", schema);
    }

    // Signal table on source database (session context capture).
    inline std::string SignalTableFor(std::string_view tablePrefix)
    {
        return std::format("public.\"{}Session\"", tablePrefix);
    }

    // Columns in the journal table (COPY target order)
    constexpr int JournalColumnCount = 9;

    // Checkpoint table column names (CamelCase -- must be double-quoted in SQL)
    constexpr std::string_view CheckpointSlotColumn = "SlotName";
    constexpr std::string_view CheckpointLSNColumn  = "ConfirmedLSN";
    constexpr std::string_view CheckpointUpdatedAt  = "UpdatedAt";
}

// =============================================================================
// Column Names for Journal Table (COPY column order)
// All names are CamelCase — must be double-quoted in PostgreSQL SQL.
// =============================================================================
namespace JournalColumns
{
    constexpr std::string_view JournalID   = "JournalID";
    constexpr std::string_view XID         = "XID";
    constexpr std::string_view LSN         = "LSN";
    constexpr std::string_view Operation   = "Operation";
    constexpr std::string_view Data        = "Data";
    constexpr std::string_view UserID      = "UserID";
    constexpr std::string_view Origin      = "Origin";
    constexpr std::string_view CommittedAt = "CommittedAt";
    constexpr std::string_view CapturedAt  = "CapturedAt";

    // COPY header (excludes JournalID which is SERIAL).
    // CamelCase names require double-quoting in PostgreSQL.
    constexpr std::string_view CopyColumns =
        "\"XID\", \"LSN\", \"Operation\", \"Data\", \"UserID\", \"Origin\", \"CommittedAt\", \"CapturedAt\"";

    constexpr int CopyColumnCount = 8;
}

// =============================================================================
// Signal Table Columns (Source DB - Session Context)
// =============================================================================
namespace SignalColumns
{
    constexpr std::string_view SessionID = "\"SessionID\"";
    constexpr std::string_view BackendPID = "\"BackendPID\"";
    constexpr std::string_view UserID = "\"UserID\"";
    constexpr std::string_view AppName = "\"AppName\"";
    constexpr std::string_view Moment = "\"Moment\"";

}

// =============================================================================
// CDC Operation Types
// =============================================================================
namespace Operation
{
    constexpr char Insert   = 'I';
    constexpr char Update   = 'U';
    constexpr char Delete   = 'D';
    constexpr char Truncate = 'T';
    constexpr char Begin    = 'B';
    constexpr char Commit   = 'C';
    constexpr char Relation = 'R';
    constexpr char Type     = 'Y';
    constexpr char Origin   = 'O';
    constexpr char Message  = 'M';
    constexpr char Stream   = 'S';

    inline std::string_view ToString(char op)
    {
        switch (op)
        {
            case Insert:   return "INSERT";
            case Update:   return "UPDATE";
            case Delete:   return "DELETE";
            case Truncate: return "TRUNCATE";
            default:       return "UNKNOWN";
        }
    }
}

// =============================================================================
// Performance Tuning
// =============================================================================
namespace Performance
{
    // Memory buffer initial capacity for transaction accumulation (bytes)
    constexpr size_t TxBufferInitialCapacity = 64 * 1024;  // 64 KB

    // Maximum transaction buffer before forced flush (bytes)
    constexpr size_t TxBufferMaxCapacity = 16 * 1024 * 1024;  // 16 MB

    // COPY batch size: number of rows accumulated before COPY flush
    constexpr int DefaultBatchSize = 1000;

    // Maximum batch size allowed
    constexpr int MaxBatchSize = 50'000;

    // Flush interval: force COPY flush even if batch is not full (milliseconds)
    constexpr int DefaultFlushIntervalMS = 500;

    // Minimum flush interval
    constexpr int MinFlushIntervalMS = 50;

    // Maximum flush interval
    constexpr int MaxFlushIntervalMS = 30'000;

    // Standby status update interval to keep replication alive (seconds)
    constexpr int StandbyStatusIntervalSec = 10;

    // PQgetCopyData poll interval when no data available (microseconds)
    constexpr int PollIntervalUS = 1000;  // 1 ms

    // Maximum WAL message size we expect (safety limit)
    constexpr size_t MaxWalMessageSize = 256 * 1024 * 1024;  // 256 MB

    // COPY buffer pre-allocation per row (average estimate)
    constexpr size_t CopyRowAvgSize = 512;

    // String builder initial capacity for COPY row formatting
    constexpr size_t CopyRowBuilderCapacity = 4096;
}

// =============================================================================
// Reconnection Policy
// =============================================================================
namespace Reconnection
{
    // Initial delay before first reconnection attempt (milliseconds)
    constexpr int BaseDelayMS = 500;

    // Maximum delay between reconnection attempts (milliseconds)
    constexpr int MaxDelayMS = 60'000;  // 1 minute

    // Backoff multiplier per failed attempt
    constexpr double BackoffMultiplier = 2.0;

    // Jitter factor: randomize delay by ±JitterFactor
    constexpr double JitterFactor = 0.25;

    // Maximum consecutive reconnection attempts before abort (-1 = infinite)
    constexpr int MaxRetries = -1;

    // Grace period after successful connection before resetting retry count (seconds)
    constexpr int StabilityPeriodSec = 60;
}

// =============================================================================
// Logging Configuration
// =============================================================================
namespace Logging
{
    constexpr std::string_view DefaultLogFile = "datacrusher.log";
    constexpr size_t MaxLogFileSizeMB = 100;
    constexpr int MaxRotatedFiles = 10;

    // Log format timestamp pattern
    constexpr std::string_view TimestampFormat = "{:%Y-%m-%d %H:%M:%S}";
}

// =============================================================================
// PostgreSQL Type OIDs (Commonly used)
// =============================================================================
namespace PgTypeOID
{
    constexpr OID Bool        = 16;
    constexpr OID Int2        = 21;
    constexpr OID Int4        = 23;
    constexpr OID Int8        = 20;
    constexpr OID Float4      = 700;
    constexpr OID Float8      = 701;
    constexpr OID Numeric     = 1700;
    constexpr OID Varchar     = 1043;
    constexpr OID Text        = 25;
    constexpr OID Char        = 18;
    constexpr OID BPChar      = 1042;
    constexpr OID Bytea       = 17;
    constexpr OID UUID        = 2950;
    constexpr OID Timestamp   = 1114;
    constexpr OID TimestampTZ = 1184;
    constexpr OID Date        = 1082;
    constexpr OID Time        = 1083;
    constexpr OID TimeTZ      = 1266;
    constexpr OID Interval    = 1186;
    constexpr OID JSON        = 114;
    constexpr OID JSONB       = 3802;
    constexpr OID XML         = 142;
    constexpr OID Inet        = 869;
    constexpr OID Cidr        = 650;
    constexpr OID MacAddr     = 829;
    constexpr OID Bit         = 1560;
    constexpr OID VarBit      = 1562;
    constexpr OID OIDType     = 26;
    constexpr OID Name        = 19;
    constexpr OID Money       = 790;

    // Array types (add 1-dimensional array offset)
    constexpr OID Int4Array   = 1007;
    constexpr OID TextArray   = 1009;
    constexpr OID UUIDArray   = 2951;

    // Returns a human-readable type name for a PostgreSQL OID
    inline std::string_view TypeName(OID oid)
    {
        switch (oid)
        {
        case Bool:        return "bool";
        case Int2:        return "int2";
        case Int4:        return "int4";
        case Int8:        return "int8";
        case Float4:      return "float4";
        case Float8:      return "float8";
        case Numeric:     return "numeric";
        case Varchar:     return "varchar";
        case Text:        return "text";
        case Char:        return "char";
        case BPChar:      return "bpchar";
        case Bytea:       return "bytea";
        case UUID:        return "uuid";
        case Timestamp:   return "timestamp";
        case TimestampTZ: return "timestamptz";
        case Date:        return "date";
        case Time:        return "time";
        case TimeTZ:      return "timetz";
        case Interval:    return "interval";
        case JSON:        return "json";
        case JSONB:       return "jsonb";
        case XML:         return "xml";
        case Inet:        return "inet";
        case Cidr:        return "cidr";
        case MacAddr:     return "macaddr";
        case Bit:         return "bit";
        case VarBit:      return "varbit";
        case OIDType:     return "oid";
        case Name:        return "name";
        case Money:       return "money";
        case Int4Array:   return "int4[]";
        case TextArray:   return "text[]";
        case UUIDArray:   return "uuid[]";
        default:          return "?";
        }
    }

    // Returns a display-ready value string according to its PostgreSQL type:
    //   - Numeric types   → bare number, no quotes
    //   - Boolean         → "true" / "false" (PG sends 't'/'f')
    //   - UUID            → bare UUID, no quotes
    //   - Binary (bytea)  → literal <binary>
    //   - JSON / JSONB    → raw JSON, no extra quotes
    //   - Everything else → value wrapped in single quotes
    //   - NULL            → NULL (always, regardless of type)
    inline std::string FormatValue(OID oid, std::string_view value, bool isNull)
    {
        if (isNull) return "NULL";
        switch (oid)
        {
        case Int2: case Int4: case Int8:
        case Float4: case Float8: case Numeric:
        case Money: case OIDType:
            return std::string(value);
        case Bool:
            return (value == "t" || value == "true" || value == "1") ? "true" : "false";
        case UUID:
            return std::string(value);
        case Bytea:
            return "<binary>";
        case JSON: case JSONB:
            return std::string(value);
        default:
            return std::format("'{}'", value);
        }
    }
}

// =============================================================================
// SQL Statements
// =============================================================================
namespace SQL
{
    // Pre-flight: verify wal_level = logical
    constexpr std::string_view CheckWalLevel = "SHOW wal_level";

    // Create replication slot
    inline std::string CreateReplicationSlot(std::string_view slotName, std::string_view plugin)
    {
        return std::format(
            "CREATE_REPLICATION_SLOT {} LOGICAL {}",
            slotName, plugin
        );
    }

    // Start logical replication stream
    inline std::string StartReplication(std::string_view slotName, LSN startLSN,
                                        std::string_view publication, int protoVersion)
    {
        return std::format(
            "START_REPLICATION SLOT {} LOGICAL {} "
            "(proto_version '{}', publication_names '{}')",
            slotName, FormatLSN(startLSN), protoVersion, publication
        );
    }

    // Identify system (get timeline, xlogpos, dbname, systemid)
    constexpr std::string_view IdentifySystem = "IDENTIFY_SYSTEM";

    // Drop replication slot
    inline std::string DropReplicationSlot(std::string_view slotName)
    {
        return std::format("DROP_REPLICATION_SLOT {}", slotName);
    }

    // Read checkpoint LSN from the journal schema's Checkpoint table
    inline std::string ReadCheckpointLSN(std::string_view schema, std::string_view slotName)
    {
        return std::format(
            "SELECT \"ConfirmedLSN\" FROM {} WHERE \"SlotName\" = '{}'",
            Tables::CheckpointTableFor(schema), slotName
        );
    }

    // Upsert checkpoint LSN (idempotent) in the journal schema's Checkpoint table
    inline std::string UpsertCheckpointLSN(std::string_view schema, std::string_view slotName, LSN lsn)
    {
        return std::format(
            "INSERT INTO {} (\"SlotName\", \"ConfirmedLSN\", \"UpdatedAt\") "
            "VALUES ('{}', '{}', NOW()) "
            "ON CONFLICT (\"SlotName\") DO UPDATE SET \"ConfirmedLSN\" = '{}', \"UpdatedAt\" = NOW()",
            Tables::CheckpointTableFor(schema), slotName, FormatLSN(lsn), FormatLSN(lsn)
        );
    }

    // COPY command for a specific per-table journal
    inline std::string CopyJournalTableIn(std::string_view journalTable)
    {
        return std::format(
            "COPY {} ({}) FROM STDIN WITH (FORMAT text, DELIMITER E'\\t', NULL '\\N')",
            journalTable, JournalColumns::CopyColumns
        );
    }

    // Maps a PostgreSQL udt_name to a CAST target type for use in a view SELECT.
    // Returns empty string for text-compatible types that need no cast.
    inline std::string_view PgCastTypeFor(std::string_view udtName)
    {
        if (udtName == "uuid")        return "uuid";
        if (udtName == "bool")        return "boolean";
        if (udtName == "int2")        return "smallint";
        if (udtName == "int4")        return "integer";
        if (udtName == "int8")        return "bigint";
        if (udtName == "float4")      return "real";
        if (udtName == "float8")      return "double precision";
        if (udtName == "numeric")     return "numeric";
        if (udtName == "money")       return "money";
        if (udtName == "timestamptz") return "timestamptz";
        if (udtName == "timestamp")   return "timestamp";
        if (udtName == "date")        return "date";
        if (udtName == "time")        return "time";
        if (udtName == "timetz")      return "timetz";
        if (udtName == "json")        return "json";
        if (udtName == "jsonb")       return "jsonb";
        if (udtName == "inet")        return "inet";
        if (udtName == "cidr")        return "cidr";
        return ""; // varchar, text, char, etc. — no cast needed
    }

    // DDL to drop and recreate the view {schema}."{sourceTable}" over {schema}."{schema}{sourceTable}".
    // Source columns are expanded from the JSONB delta field with type-aware casts.
    // Journal audit columns appear at the end with the schema prefix (CamelCase).
    // Called at startup so any source table schema changes are always applied.
    inline std::string CreateOrReplaceTableView(
        std::string_view schema,
        std::string_view sourceTable,
        const std::vector<SourceColumnInfo>& columns)
    {
        auto viewName = Tables::ViewTableFor(schema, sourceTable);
        auto jnlTable = Tables::JournalTableFor(schema, sourceTable);

        std::string sql;
        sql.reserve(512 + columns.size() * 80);

        // DROP first so column order/type changes are always applied
        sql += std::format("DROP VIEW IF EXISTS {};\n", viewName);
        sql += std::format("CREATE VIEW {} AS\nSELECT\n", viewName);

        // Source data columns extracted from the JSONB delta
        for (const auto& col : columns)
        {
            auto castType = PgCastTypeFor(col.UdtName);
            if (castType.empty())
                sql += std::format("    \"Data\"->>'{}' AS \"{}\",\n", col.Name, col.Name);
            else
                sql += std::format("    (\"Data\"->>'{}')::{} AS \"{}\",\n",
                                   col.Name, castType, col.Name);
        }

        // Journal audit columns with schema prefix (CamelCase)
        sql += std::format("    \"JournalID\"   AS \"{}JournalID\",\n", schema);
        sql += std::format("    \"XID\"         AS \"{}XID\",\n", schema);
        sql += std::format("    \"LSN\"         AS \"{}LSN\",\n", schema);
        sql += std::format("    \"Operation\"   AS \"{}Operation\",\n", schema);
        sql += std::format("    \"UserID\"      AS \"{}UserID\",\n", schema);
        sql += std::format("    \"Origin\"      AS \"{}Origin\",\n", schema);
        sql += std::format("    \"CommittedAt\" AS \"{}CommittedAt\",\n", schema);
        sql += std::format("    \"CapturedAt\"  AS \"{}CapturedAt\"\n", schema);
        sql += std::format("FROM {};\n", jnlTable);

        return sql;
    }

    // DDL to create one per-table journal table in the given journal schema.
    // schema: e.g. "CDCx"  sourceTable: unquoted name, e.g. "SYSxApplication"
    // Journal table = "{schema}"."{schema}{sourceTable}" e.g. "CDCx"."CDCxSYSxApplication"
    inline std::string CreateJournalTable(std::string_view schema, std::string_view sourceTable)
    {
        return std::format(R"SQL(
            CREATE TABLE IF NOT EXISTS "{0}"."{0}{1}" (
                "JournalID"   BIGSERIAL    PRIMARY KEY,
                "XID"         BIGINT       NOT NULL,
                "LSN"         TEXT         NOT NULL,
                "Operation"   CHAR(1)      NOT NULL,
                "Data"        JSONB,
                "UserID"      TEXT,
                "Origin"      TEXT,
                "CommittedAt" TIMESTAMPTZ  NOT NULL,
                "CapturedAt"  TIMESTAMPTZ  NOT NULL DEFAULT NOW()
            );
            CREATE INDEX IF NOT EXISTS "idx_{0}{1}_XID"         ON "{0}"."{0}{1}" ("XID");
            CREATE INDEX IF NOT EXISTS "idx_{0}{1}_CommittedAt" ON "{0}"."{0}{1}" ("CommittedAt");
            CREATE INDEX IF NOT EXISTS "idx_{0}{1}_UserID"      ON "{0}"."{0}{1}" ("UserID") WHERE "UserID" IS NOT NULL;
        )SQL",
        schema, sourceTable);
    }

    // Bootstrap journal schema + Checkpoint table (journal tables are created per-source-table).
    // schema: the TablePrefix value, e.g. "CDCx" — quoted in DDL to preserve case.
    inline std::string BootstrapTargetSchema(std::string_view schema)
    {
        return std::format(R"SQL(
        CREATE SCHEMA IF NOT EXISTS "{0}";

        CREATE TABLE IF NOT EXISTS "{0}"."Checkpoint" (
            "SlotName"     TEXT PRIMARY KEY,
            "ConfirmedLSN" TEXT NOT NULL,
            "UpdatedAt"    TIMESTAMPTZ NOT NULL DEFAULT NOW()
        );
        )SQL", schema);
    }

    // Create signal table on source database.
    // tablePrefix: e.g. "CDCx" → creates public."CDCxSession"
    inline std::string BootstrapSignalTable(std::string_view tablePrefix)
    {
        return std::format(R"SQL(
        CREATE TABLE IF NOT EXISTS public."{}Session" (
            "SessionID"  UUID         NOT NULL DEFAULT gen_random_uuid() PRIMARY KEY,
            "UserID"     UUID,
            "AppName"    VARCHAR(50),
            "BackendPID" INTEGER,
            "Moment"     TIMESTAMPTZ  NOT NULL DEFAULT NOW()
        );
        )SQL", tablePrefix);
    }
}

// =============================================================================
// Exit Codes
// =============================================================================
namespace ExitCode
{
    constexpr int Success              = 0;
    constexpr int InvalidArguments     = 1;
    constexpr int ConnectionFailed     = 2;
    constexpr int ReplicationFailed    = 3;
    constexpr int WriteFailed          = 4;
    constexpr int CheckpointFailed     = 5;
    constexpr int FatalError           = 99;
}

} // namespace DC
