// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// WAL binary message parser (pgoutput protocol)
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include "TypeHandlers.h"
#include <string>
#include <vector>
#include <optional>

namespace DC
{

// =============================================================================
// Relation metadata cached from 'R' messages
// =============================================================================
struct ColumnInfo
{
    std::string Name;
    OID         TypeOID    = 0;
    int32_t     TypeModifier = -1;
    uint8_t     Flags      = 0; // 1 = part of key
};

struct RelationInfo
{
    OID                     RelationOID = 0;
    std::string             SchemaName;
    std::string             TableName;
    uint8_t                 ReplicaIdentity = 0; // d=default, n=nothing, f=full, i=index
    std::vector<ColumnInfo> Columns;
};

// =============================================================================
// Parsed change event
// =============================================================================
struct ColumnValue
{
    std::string Name;
    OID         TypeOID = 0;
    bool        IsNull  = false;
    std::string TextValue; // Already converted by TypeHandler
};

struct ChangeEvent
{
    char                    Operation = '\0';  // I, U, D, T
    LSN                     EventLSN  = LSN_INVALID;
    XID                     TransactionXID = XID_INVALID;
    std::string             SchemaName;
    std::string             TableName;
    std::vector<ColumnValue> OldValues; // For UPDATE (key) / DELETE
    std::vector<ColumnValue> NewValues; // For INSERT / UPDATE
    TimePoint               CaptureTime;      // When we received this event

    // Serialize the delta as a single JSON object for journal storage:
    // INSERT  -> all new values
    // DELETE  -> all old values
    // UPDATE  -> only fields whose value changed (new value of each changed field)
    std::string DeltaJSON() const;
};

struct BeginMessage
{
    LSN     FinalLSN       = LSN_INVALID;
    int64_t CommitTimestamp = 0;   // PostgreSQL epoch microseconds
    XID     TransactionXID = XID_INVALID;
};

struct CommitMessage
{
    uint8_t Flags          = 0;
    LSN     CommitLSN      = LSN_INVALID;
    LSN     EndLSN         = LSN_INVALID;
    int64_t CommitTimestamp = 0;
};

// =============================================================================
// WAL Message Parser
// =============================================================================
class WalMessageParser final
{
public:
    WalMessageParser();

    // Parse a raw WAL message and dispatch to appropriate handler
    // Returns the message type character or '\0' on error
    char Parse(const Byte* data, size_t length);

    // Accessors for latest parsed message
    const BeginMessage&  LastBegin()    const { return _LastBegin; }
    const CommitMessage& LastCommit()   const { return _LastCommit; }
    const ChangeEvent&   LastChange()   const { return _LastChange; }
    const RelationInfo*  GetRelation(OID relOID) const;

    // Statistics
    int64_t TotalMessagesParsed() const { return _TotalMessages; }
    int64_t TotalParseErrors()    const { return _ParseErrors; }

private:
    void ParseBegin(const Byte* data, size_t length);
    void ParseCommit(const Byte* data, size_t length);
    void ParseRelation(const Byte* data, size_t length);
    void ParseInsert(const Byte* data, size_t length);
    void ParseUpdate(const Byte* data, size_t length);
    void ParseDelete(const Byte* data, size_t length);
    void ParseTruncate(const Byte* data, size_t length);
    void ParseType(const Byte* data, size_t length);
    void ParseOrigin(const Byte* data, size_t length);
    void ParseMessage(const Byte* data, size_t length);

    // Parse a TupleData section, returns columns
    std::vector<ColumnValue> ParseTupleData(const Byte*& cursor, const Byte* end,
                                             const RelationInfo& relation);

    // Big-endian readers (using cursor advancement)
    static uint8_t  ReadU8(const Byte*& cursor);
    static uint16_t ReadBE16(const Byte*& cursor);
    static uint32_t ReadBE32(const Byte*& cursor);
    static uint64_t ReadBE64(const Byte*& cursor);
    static int32_t  ReadBE32s(const Byte*& cursor);
    static int64_t  ReadBE64s(const Byte*& cursor);
    static std::string ReadString(const Byte*& cursor, const Byte* end);

    // Relation cache
    std::unordered_map<OID, RelationInfo> _Relations;

    // Last parsed messages
    BeginMessage  _LastBegin;
    CommitMessage _LastCommit;
    ChangeEvent   _LastChange;

    // Current transaction context
    XID _CurrentXID = XID_INVALID;

    // Stats
    int64_t _TotalMessages = 0;
    int64_t _ParseErrors   = 0;
};

} // namespace DC
