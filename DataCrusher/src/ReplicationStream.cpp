// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Replication stream implementation
// =============================================================================
#include "ReplicationStream.h"
#include "Logger.h"
#include <cstring>

namespace DC
{

ReplicationStream::ReplicationStream() = default;

ReplicationStream::~ReplicationStream()
{
    Disconnect();
}

bool ReplicationStream::Connect(const ReplicationConfig& config)
{
    _Config = config;
    Disconnect();

    auto connStr = BuildConnectionString();
    LOG_INFO("Connecting to source database: {}:{}/{}", config.Host, config.Port, config.DBName);
    LOG_DEBUG("Connection string: {}", connStr);

    _Connection = PQconnectdb(connStr.c_str());
    if (PQstatus(_Connection) != CONNECTION_OK)
    {
        LOG_ERROR("Replication connection failed: {}", PQerrorMessage(_Connection));
        Disconnect();
        return false;
    }

    LOG_INFO("Connected to PostgreSQL (PID={}, server_version={})",
             PQbackendPID(_Connection), PQserverVersion(_Connection));

    // Pre-flight: verify wal_level = logical
    if (!CheckWalLevel())
        return false;

    return true;
}

void ReplicationStream::Disconnect()
{
    _Streaming = false;

    if (_LastCopyBuffer)
    {
        PQfreemem(_LastCopyBuffer);
        _LastCopyBuffer = nullptr;
    }

    if (_Connection)
    {
        PQfinish(_Connection);
        _Connection = nullptr;
    }
}

bool ReplicationStream::IsConnected() const
{
    return _Connection && PQstatus(_Connection) == CONNECTION_OK;
}

std::string ReplicationStream::BuildConnectionString() const
{
    return std::format(
        "host='{}' port='{}' dbname='{}' user='{}' password='{}' "
        "replication='{}' connect_timeout='{}' application_name='{}' sslmode='{}'",
        _Config.Host, _Config.Port, _Config.DBName,
        _Config.User, _Config.Password,
        Replication::ReplicationMode,
        _Config.ConnTimeoutSec,
        Connection::ApplicationName,
        _Config.SSLMode
    );
}

bool ReplicationStream::CheckWalLevel()
{
    if (!IsConnected()) return false;

    PGresult* res = PQexec(_Connection, std::string(SQL::CheckWalLevel).c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1)
    {
        LOG_ERROR("Failed to query wal_level: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    std::string walLevel = PQgetvalue(res, 0, 0);
    PQclear(res);

    if (walLevel != "logical")
    {
        LOG_FATAL("PostgreSQL wal_level is '{}' but 'logical' is required for CDC", walLevel);
        LOG_FATAL("Fix: set wal_level = 'logical' in postgresql.conf and restart PostgreSQL");
        return false;
    }

    LOG_DEBUG("wal_level = '{}' (OK)", walLevel);
    return true;
}

bool ReplicationStream::CreateSlotIfNotExists()
{
    if (!IsConnected()) return false;

    // Check if slot exists
    auto query = std::format(
        "SELECT slot_name FROM pg_replication_slots WHERE slot_name = '{}'",
        _Config.SlotName
    );

    PGresult* res = PQexec(_Connection, query.c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK)
    {
        LOG_ERROR("Failed to check replication slot: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    if (PQntuples(res) > 0)
    {
        LOG_INFO("Replication slot '{}' already exists", _Config.SlotName);
        PQclear(res);
        return true;
    }
    PQclear(res);

    // Create the slot
    auto createSQL = SQL::CreateReplicationSlot(_Config.SlotName, _Config.OutputPlugin);
    LOG_INFO("Creating replication slot '{}' with plugin '{}'",
             _Config.SlotName, _Config.OutputPlugin);

    res = PQexec(_Connection, createSQL.c_str());
    auto status = PQresultStatus(res);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)
    {
        LOG_ERROR("Failed to create replication slot: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    // Extract consistent_point (LSN)
    if (PQntuples(res) > 0 && PQnfields(res) >= 2)
    {
        auto lsnField = PQgetvalue(res, 0, 1);
        if (lsnField)
        {
            _ServerWalEnd = ParseLSN(lsnField);
            LOG_INFO("Replication slot created at consistent point: {}", lsnField);
        }
    }

    PQclear(res);
    return true;
}

bool ReplicationStream::IdentifySystem(std::string& systemID, std::string& timeline,
                                        std::string& xlogpos, std::string& dbname)
{
    if (!IsConnected()) return false;

    PGresult* res = PQexec(_Connection, std::string(SQL::IdentifySystem).c_str());
    if (PQresultStatus(res) != PGRES_TUPLES_OK || PQntuples(res) < 1)
    {
        LOG_ERROR("IDENTIFY_SYSTEM failed: {}", PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    systemID = PQgetvalue(res, 0, 0);
    timeline = PQgetvalue(res, 0, 1);
    xlogpos  = PQgetvalue(res, 0, 2);
    dbname   = PQgetvalue(res, 0, 3);

    LOG_INFO("System: id={} timeline={} xlogpos={} db={}",
             systemID, timeline, xlogpos, dbname);

    PQclear(res);
    return true;
}

bool ReplicationStream::StartStreaming(LSN startLSN)
{
    if (!IsConnected()) return false;

    auto startCmd = SQL::StartReplication(
        _Config.SlotName, startLSN, _Config.Publication, _Config.ProtoVersion
    );

    LOG_INFO("Starting replication from LSN {} with slot '{}'",
             FormatLSN(startLSN), _Config.SlotName);
    LOG_DEBUG("START_REPLICATION command: {}", startCmd);

    PGresult* res = PQexec(_Connection, startCmd.c_str());
    if (PQresultStatus(res) != PGRES_COPY_BOTH)
    {
        LOG_ERROR("START_REPLICATION failed (status={}): {}",
                  PQresStatus(PQresultStatus(res)), PQerrorMessage(_Connection));
        PQclear(res);
        return false;
    }

    PQclear(res);
    _Streaming = true;
    LOG_INFO("Replication stream started successfully");
    return true;
}

StreamReadResult ReplicationStream::ReadMessage(Byte*& outData, int& outLength)
{
    outData = nullptr;
    outLength = 0;

    if (!IsConnected() || !_Streaming)
        return StreamReadResult::Error;

    // Free previous buffer
    if (_LastCopyBuffer)
    {
        PQfreemem(_LastCopyBuffer);
        _LastCopyBuffer = nullptr;
    }

    // Non-blocking consume
    if (PQconsumeInput(_Connection) == 0)
    {
        LOG_ERROR("PQconsumeInput failed: {}", PQerrorMessage(_Connection));
        return StreamReadResult::Error;
    }

    int rc = PQgetCopyData(_Connection, &_LastCopyBuffer, 1 /* async */);

    if (rc > 0)
    {
        // Got data
        outData = reinterpret_cast<Byte*>(_LastCopyBuffer);
        outLength = rc;

        // First byte indicates message type:
        // 'w' (0x77) = WAL data
        // 'k' (0x6B) = primary keepalive
        if (outLength > 0 && outData[0] == 'k')
        {
            // Keepalive message: parse server WAL end
            if (outLength >= 17)
            {
                uint64_t serverWalEnd = 0;
                std::memcpy(&serverWalEnd, outData + 1, 8);
                // Convert from network byte order
                _ServerWalEnd = DC_BSwap64(serverWalEnd);

                // Check if reply is requested (byte 17)
                bool replyRequested = (outLength >= 18 && outData[17] != 0);
                if (replyRequested)
                    return StreamReadResult::KeepAlive;
            }
            return StreamReadResult::KeepAlive;
        }

        if (outLength > 0 && outData[0] == 'w')
        {
            // WAL data message header:
            // byte 0: 'w'
            // bytes 1-8:  dataStart (LSN)
            // bytes 9-16: walEnd (LSN)
            // bytes 17-24: sendTime
            // bytes 25+: actual WAL message
            if (outLength > 25)
            {
                outData   = outData + 25;
                outLength = outLength - 25;
                return StreamReadResult::Data;
            }
            return StreamReadResult::Timeout;
        }

        return StreamReadResult::Data;
    }

    if (rc == 0)
        return StreamReadResult::Timeout;

    if (rc == -1)
    {
        // End of COPY stream
        PGresult* res = PQgetResult(_Connection);
        if (res)
        {
            auto status = PQresultStatus(res);
            if (status != PGRES_COMMAND_OK)
                LOG_ERROR("Replication stream ended with error: {}", PQerrorMessage(_Connection));
            PQclear(res);
        }
        _Streaming = false;
        return StreamReadResult::EndOfStream;
    }

    // rc == -2: error
    LOG_ERROR("PQgetCopyData error: {}", PQerrorMessage(_Connection));
    return StreamReadResult::Error;
}

bool ReplicationStream::SendStandbyStatus(LSN writtenLSN, LSN flushedLSN, LSN appliedLSN,
                                           bool replyRequested)
{
    if (!IsConnected() || !_Streaming)
        return false;

    // Build standby status update message
    // Format: 'r' + writePos(8) + flushPos(8) + applyPos(8) + timestamp(8) + reply(1) = 34 bytes
    Byte buf[34];
    buf[0] = 'r';

    auto writeBE64 = [](Byte* dest, uint64_t val)
    {
        dest[0] = static_cast<Byte>(val >> 56);
        dest[1] = static_cast<Byte>(val >> 48);
        dest[2] = static_cast<Byte>(val >> 40);
        dest[3] = static_cast<Byte>(val >> 32);
        dest[4] = static_cast<Byte>(val >> 24);
        dest[5] = static_cast<Byte>(val >> 16);
        dest[6] = static_cast<Byte>(val >>  8);
        dest[7] = static_cast<Byte>(val);
    };

    writeBE64(buf + 1,  writtenLSN);
    writeBE64(buf + 9,  flushedLSN);
    writeBE64(buf + 17, appliedLSN);

    // Timestamp: microseconds since 2000-01-01
    auto now = SystemClock::now();
    auto epoch2000 = SystemClock::from_time_t(946684800);
    auto elapsed = std::chrono::duration_cast<Microseconds>(now - epoch2000);
    uint64_t pgTimestamp = static_cast<uint64_t>(elapsed.count());
    writeBE64(buf + 25, pgTimestamp);

    buf[33] = replyRequested ? 1 : 0;

    int rc = PQputCopyData(_Connection, reinterpret_cast<const char*>(buf), 34);
    if (rc != 1)
    {
        LOG_ERROR("SendStandbyStatus failed: {}", PQerrorMessage(_Connection));
        return false;
    }

    rc = PQflush(_Connection);
    if (rc < 0)
    {
        LOG_ERROR("PQflush failed after standby status: {}", PQerrorMessage(_Connection));
        return false;
    }

    LOG_TRACE("Standby status sent: written={} flushed={} applied={}",
              FormatLSN(writtenLSN), FormatLSN(flushedLSN), FormatLSN(appliedLSN));

    return true;
}

std::string ReplicationStream::LastError() const
{
    if (_Connection)
        return PQerrorMessage(_Connection);
    return "Not connected";
}

std::string ReplicationStream::ConnectionInfo() const
{
    return std::format("{}:{}/{}@{}",
                       _Config.Host, _Config.Port, _Config.DBName, _Config.User);
}

PGresult* ReplicationStream::ExecSimple(const std::string& query)
{
    if (!IsConnected()) return nullptr;
    return PQexec(_Connection, query.c_str());
}

} // namespace DC
