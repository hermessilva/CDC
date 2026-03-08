// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Replication stream connection and WAL consumption
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <libpq-fe.h>
#include <string>
#include <functional>

namespace DC
{

// Configuration for the replication stream
struct ReplicationConfig
{
    std::string Host;
    uint16_t    Port          = 5432;
    std::string DBName;
    std::string User;
    std::string Password;
    std::string SSLMode       = "prefer";
    std::string SlotName;
    std::string Publication;
    std::string OutputPlugin  = "pgoutput";
    int         ProtoVersion  = 4;
    int         ConnTimeoutSec = 10;
};

// Result of reading from the replication stream
enum class StreamReadResult
{
    Data,        // Got WAL data
    KeepAlive,   // Server keep-alive (no data)
    Timeout,     // No data within poll interval
    EndOfStream, // Stream ended (slot dropped, etc.)
    Error        // Connection error
};

class ReplicationStream final
{
public:
    ReplicationStream();
    ~ReplicationStream();

    ReplicationStream(const ReplicationStream&) = delete;
    ReplicationStream& operator=(const ReplicationStream&) = delete;

    // Connect to PostgreSQL in replication mode
    bool Connect(const ReplicationConfig& config);

    // Disconnect and clean up
    void Disconnect();

    // Check if connected
    bool IsConnected() const;

    // Pre-flight: verify wal_level = logical
    bool CheckWalLevel();

    // Create the logical replication slot if it doesn't exist
    bool CreateSlotIfNotExists();

    // Identify system: retrieve timeline, xlogpos, systemid
    bool IdentifySystem(std::string& systemID, std::string& timeline,
                        std::string& xlogpos, std::string& dbname);

    // Start streaming from a given LSN
    bool StartStreaming(LSN startLSN);

    // Read next WAL message from the stream
    // On Data: outData points to the message, outLength its size
    // Caller does NOT own outData (it's freed on next call or Disconnect)
    StreamReadResult ReadMessage(Byte*& outData, int& outLength);

    // Send standby status update (confirming LSN processed)
    bool SendStandbyStatus(LSN writtenLSN, LSN flushedLSN, LSN appliedLSN,
                           bool replyRequested = false);

    // Get the last error message from libpq
    std::string LastError() const;

    // Get the server-reported WAL position at connection start
    LSN GetServerWalEnd() const { return _ServerWalEnd; }

    // Get connection info string (for logging)
    std::string ConnectionInfo() const;

private:
    PGconn*         _Connection = nullptr;
    ReplicationConfig _Config;
    LSN             _ServerWalEnd = LSN_INVALID;
    bool            _Streaming = false;

    // Free the last CopyData buffer
    char*           _LastCopyBuffer = nullptr;

    // Build connection string for replication mode
    std::string BuildConnectionString() const;

    // Internal: execute a simple query and return result
    PGresult* ExecSimple(const std::string& query);
};

} // namespace DC
