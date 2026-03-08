// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Signal table queries (session context capture)
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <libpq-fe.h>
#include <string>
#include <vector>
#include <optional>

namespace DC
{

// Session context captured from the signal table at COMMIT
struct SessionContext
{
    std::string UserID;
    std::string AppName;
};

class SignalTable final
{
public:
    SignalTable();
    ~SignalTable();

    SignalTable(const SignalTable&) = delete;
    SignalTable& operator=(const SignalTable&) = delete;

    // Connect to the source database (non-replication connection for queries)
    bool Connect(const std::string& host, uint16_t port,
                 const std::string& dbname, const std::string& user,
                 const std::string& password, const std::string& sslMode = "prefer");

    // Disconnect
    void Disconnect();

    // Check if connected
    bool IsConnected() const;

    // Query the signal table for the session context at COMMIT time
    // Uses the backend PID of the replication connection to find the session context
    std::optional<SessionContext> QueryContext(int backendPID);

    // Query by a specific XID (if the signal table tracks XIDs)
    std::optional<SessionContext> QueryContextByXID(XID xid);

    // Bootstrap: create public."{tablePrefix}Session" on the source database if it does not exist.
    // tablePrefix: the value from CDCxConfig.TablePrefix, e.g. "CDCx".
    // Called once at startup. Non-fatal if the table already exists.
    bool Bootstrap(std::string_view tablePrefix);

    // Verify that the publication exists on the source database.
    // Creates it (FOR ALL TABLES) automatically if missing.
    // Requires the user to have CREATE PUBLICATION privileges.
    // Returns true if the publication already existed or was created.
    bool EnsurePublication(std::string_view publicationName);

    // Returns the list of table names (unqualified) included in the given publication.
    // Used at startup to create the per-table journal tables on the target.
    std::vector<std::string> GetPublicationTables(std::string_view publicationName);

    // Returns column metadata (name + udt_name) for a source table, ordered by
    // ordinal_position. Used to generate type-aware views on the target database.
    std::vector<SourceColumnInfo> GetTableColumns(const std::string& tableName);

    // Set REPLICA IDENTITY FULL on every user table that does not already have it.
    // Without REPLICA IDENTITY FULL, PostgreSQL does not include old-row values
    // in the WAL stream for UPDATE/DELETE, so before-values cannot be captured.
    // Logs a warning for each table that cannot be altered (insufficient privilege).
    void EnsureReplicaIdentityFull();

    // Get last error
    std::string LastError() const;

private:
    PGconn*     _Connection  = nullptr;
    std::string _TablePrefix = "CDCx";   // Set by Bootstrap(); used in SQL queries
};

} // namespace DC
