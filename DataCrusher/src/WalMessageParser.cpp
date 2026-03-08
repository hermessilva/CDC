// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// WAL binary message parser implementation
// =============================================================================
#include "WalMessageParser.h"
#include "Logger.h"
#include <sstream>
#include <unordered_map>

namespace DC
{

// =============================================================================
// ChangeEvent JSON serialization
// =============================================================================
static std::string ColumnsToJSON(const std::vector<ColumnValue>& columns)
{
    if (columns.empty())
        return "\\N";

    std::string json;
    json.reserve(256);
    json += '{';

    bool first = true;
    for (const auto& col : columns)
    {
        if (!first) json += ',';
        first = false;

        // Key
        json += '"';
        json += col.Name;
        json += "\":";

        // Value
        if (col.IsNull)
        {
            json += "null";
        }
        else
        {
            // For text/varchar/uuid/timestamp: wrap in quotes
            // For numeric types: raw value
            bool isNumeric = (col.TypeOID == PgTypeOID::Int2 ||
                              col.TypeOID == PgTypeOID::Int4 ||
                              col.TypeOID == PgTypeOID::Int8 ||
                              col.TypeOID == PgTypeOID::Float4 ||
                              col.TypeOID == PgTypeOID::Float8 ||
                              col.TypeOID == PgTypeOID::Numeric ||
                              col.TypeOID == PgTypeOID::Bool ||
                              col.TypeOID == PgTypeOID::OIDType);

            if (col.TypeOID == PgTypeOID::Bool)
            {
                json += (col.TextValue == "t") ? "true" : "false";
            }
            else if (isNumeric)
            {
                json += col.TextValue;
            }
            else if (col.TypeOID == PgTypeOID::JSON || col.TypeOID == PgTypeOID::JSONB)
            {
                // JSON values embedded directly (already valid JSON)
                json += col.TextValue;
            }
            else
            {
                json += '"';
                // Escape for JSON string
                for (char c : col.TextValue)
                {
                    switch (c)
                    {
                        case '"':  json += "\\\""; break;
                        case '\\': json += "\\\\"; break;
                        case '\n': json += "\\n";  break;
                        case '\r': json += "\\r";  break;
                        case '\t': json += "\\t";  break;
                        default:   json += c;      break;
                    }
                }
                json += '"';
            }
        }
    }

    json += '}';
    return json;
}

std::string ChangeEvent::DeltaJSON() const
{
    if (Operation == DC::Operation::Insert)
        return ColumnsToJSON(NewValues);

    if (Operation == DC::Operation::Delete)
        return ColumnsToJSON(OldValues);

    if (Operation == DC::Operation::Update)
    {
        // Collect only changed fields (new value when old != new, or no OLD tuple)
        // Build old-value index for O(1) lookup
        std::unordered_map<std::string_view, const ColumnValue*> oldMap;
        oldMap.reserve(OldValues.size());
        for (const auto& col : OldValues)
            oldMap.emplace(col.Name, &col);

        std::vector<const ColumnValue*> changed;
        changed.reserve(NewValues.size());
        for (const auto& newCol : NewValues)
        {
            auto it = oldMap.find(newCol.Name);
            if (it == oldMap.end())
            {
                // No OLD tuple for this field — include it (REPLICA IDENTITY DEFAULT)
                changed.push_back(&newCol);
            }
            else
            {
                const ColumnValue& oldCol = *it->second;
                bool same = (oldCol.IsNull == newCol.IsNull) &&
                            (oldCol.IsNull || oldCol.TextValue == newCol.TextValue);
                if (!same)
                    changed.push_back(&newCol);
            }
        }

        if (changed.empty())
            return "\\N";

        // Serialize only the delta fields
        std::string json;
        json.reserve(128);
        json += '{';
        bool first = true;
        for (const ColumnValue* col : changed)
        {
            if (!first) json += ',';
            first = false;
            json += '"';
            json += col->Name;
            json += "\":";
            if (col->IsNull)
            {
                json += "null";
            }
            else
            {
                bool isNumeric = (col->TypeOID == PgTypeOID::Int2 ||
                                  col->TypeOID == PgTypeOID::Int4 ||
                                  col->TypeOID == PgTypeOID::Int8 ||
                                  col->TypeOID == PgTypeOID::Float4 ||
                                  col->TypeOID == PgTypeOID::Float8 ||
                                  col->TypeOID == PgTypeOID::Numeric ||
                                  col->TypeOID == PgTypeOID::Bool ||
                                  col->TypeOID == PgTypeOID::OIDType);
                if (col->TypeOID == PgTypeOID::Bool)
                {
                    json += (col->TextValue == "t") ? "true" : "false";
                }
                else if (isNumeric)
                {
                    json += col->TextValue;
                }
                else if (col->TypeOID == PgTypeOID::JSON || col->TypeOID == PgTypeOID::JSONB)
                {
                    json += col->TextValue;
                }
                else
                {
                    json += '"';
                    for (char c : col->TextValue)
                    {
                        switch (c)
                        {
                            case '"':  json += "\\\""; break;
                            case '\\': json += "\\\\"; break;
                            case '\n': json += "\\n";  break;
                            case '\r': json += "\\r";  break;
                            case '\t': json += "\\t";  break;
                            default:   json += c;      break;
                        }
                    }
                    json += '"';
                }
            }
        }
        json += '}';
        return json;
    }

    return "\\N";
}

// =============================================================================
// Parser Construction
// =============================================================================
WalMessageParser::WalMessageParser()
{
    _Relations.reserve(128);
}

// =============================================================================
// Cursor-advancing big-endian readers
// =============================================================================
uint8_t WalMessageParser::ReadU8(const Byte*& cursor)
{
    return *cursor++;
}

uint16_t WalMessageParser::ReadBE16(const Byte*& cursor)
{
    uint16_t val = (static_cast<uint16_t>(cursor[0]) << 8) |
                    static_cast<uint16_t>(cursor[1]);
    cursor += 2;
    return val;
}

uint32_t WalMessageParser::ReadBE32(const Byte*& cursor)
{
    uint32_t val = (static_cast<uint32_t>(cursor[0]) << 24) |
                   (static_cast<uint32_t>(cursor[1]) << 16) |
                   (static_cast<uint32_t>(cursor[2]) <<  8) |
                    static_cast<uint32_t>(cursor[3]);
    cursor += 4;
    return val;
}

uint64_t WalMessageParser::ReadBE64(const Byte*& cursor)
{
    uint64_t val = (static_cast<uint64_t>(cursor[0]) << 56) |
                   (static_cast<uint64_t>(cursor[1]) << 48) |
                   (static_cast<uint64_t>(cursor[2]) << 40) |
                   (static_cast<uint64_t>(cursor[3]) << 32) |
                   (static_cast<uint64_t>(cursor[4]) << 24) |
                   (static_cast<uint64_t>(cursor[5]) << 16) |
                   (static_cast<uint64_t>(cursor[6]) <<  8) |
                    static_cast<uint64_t>(cursor[7]);
    cursor += 8;
    return val;
}

int32_t WalMessageParser::ReadBE32s(const Byte*& cursor) { return static_cast<int32_t>(ReadBE32(cursor)); }
int64_t WalMessageParser::ReadBE64s(const Byte*& cursor) { return static_cast<int64_t>(ReadBE64(cursor)); }

std::string WalMessageParser::ReadString(const Byte*& cursor, const Byte* end)
{
    const Byte* start = cursor;
    while (cursor < end && *cursor != 0)
        ++cursor;
    std::string result(reinterpret_cast<const char*>(start), cursor - start);
    if (cursor < end) ++cursor; // skip null terminator
    return result;
}

// =============================================================================
// Main Parse Dispatch
// =============================================================================
char WalMessageParser::Parse(const Byte* data, size_t length)
{
    if (length == 0)
        return '\0';

    ++_TotalMessages;
    char msgType = static_cast<char>(data[0]);
    const Byte* payload = data + 1;
    size_t payloadLen = length - 1;

    switch (msgType)
    {
        case Operation::Begin:    ParseBegin(payload, payloadLen);    break;
        case Operation::Commit:   ParseCommit(payload, payloadLen);   break;
        case Operation::Relation: ParseRelation(payload, payloadLen); break;
        case Operation::Insert:   ParseInsert(payload, payloadLen);   break;
        case Operation::Update:   ParseUpdate(payload, payloadLen);   break;
        case Operation::Delete:   ParseDelete(payload, payloadLen);   break;
        case Operation::Truncate: ParseTruncate(payload, payloadLen); break;
        case Operation::Type:     ParseType(payload, payloadLen);     break;
        case Operation::Origin:   ParseOrigin(payload, payloadLen);   break;
        case Operation::Message:  ParseMessage(payload, payloadLen);  break;
        default:
            LOG_WARN("Unknown WAL message type: 0x{:02X} ('{}')", data[0], msgType);
            ++_ParseErrors;
            return '\0';
    }

    return msgType;
}

// =============================================================================
// Message Parsers
// =============================================================================

void WalMessageParser::ParseBegin(const Byte* data, size_t length)
{
    if (length < 20)
    {
        LOG_ERROR("BEGIN message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    _LastBegin.FinalLSN       = ReadBE64(cursor);
    _LastBegin.CommitTimestamp = ReadBE64s(cursor);
    _LastBegin.TransactionXID = ReadBE32(cursor);
    _CurrentXID = _LastBegin.TransactionXID;

    LOG_TRACE("BEGIN xid={} final_lsn={}", _CurrentXID, FormatLSN(_LastBegin.FinalLSN));
}

void WalMessageParser::ParseCommit(const Byte* data, size_t length)
{
    if (length < 25)
    {
        LOG_ERROR("COMMIT message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    _LastCommit.Flags          = ReadU8(cursor);
    _LastCommit.CommitLSN      = ReadBE64(cursor);
    _LastCommit.EndLSN         = ReadBE64(cursor);
    _LastCommit.CommitTimestamp = ReadBE64s(cursor);

    LOG_TRACE("COMMIT xid={} lsn={} end_lsn={}",
              _CurrentXID, FormatLSN(_LastCommit.CommitLSN), FormatLSN(_LastCommit.EndLSN));

    _CurrentXID = XID_INVALID;
}

void WalMessageParser::ParseRelation(const Byte* data, size_t length)
{
    if (length < 8)
    {
        LOG_ERROR("RELATION message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    const Byte* end = data + length;

    RelationInfo rel;
    rel.RelationOID     = ReadBE32(cursor);
    rel.SchemaName      = ReadString(cursor, end);
    rel.TableName       = ReadString(cursor, end);
    rel.ReplicaIdentity = ReadU8(cursor);

    uint16_t colCount = ReadBE16(cursor);
    rel.Columns.reserve(colCount);

    for (uint16_t i = 0; i < colCount && cursor < end; ++i)
    {
        ColumnInfo col;
        col.Flags       = ReadU8(cursor);
        col.Name        = ReadString(cursor, end);
        col.TypeOID     = ReadBE32(cursor);
        col.TypeModifier = ReadBE32s(cursor);
        rel.Columns.push_back(std::move(col));
    }

    LOG_DEBUG("RELATION oid={} {}.{} cols={}",
              rel.RelationOID, rel.SchemaName, rel.TableName, rel.Columns.size());

    _Relations[rel.RelationOID] = std::move(rel);
}

void WalMessageParser::ParseInsert(const Byte* data, size_t length)
{
    if (length < 7)
    {
        LOG_ERROR("INSERT message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    const Byte* end = data + length;

    OID relOID = ReadBE32(cursor);
    uint8_t tupleType = ReadU8(cursor); // should be 'N'

    auto relIt = _Relations.find(relOID);
    if (relIt == _Relations.end())
    {
        LOG_ERROR("INSERT references unknown relation OID {}", relOID);
        ++_ParseErrors;
        return;
    }

    _LastChange = {};
    _LastChange.Operation      = Operation::Insert;
    _LastChange.TransactionXID = _CurrentXID;
    _LastChange.SchemaName     = relIt->second.SchemaName;
    _LastChange.TableName      = relIt->second.TableName;
    _LastChange.CaptureTime    = SteadyClock::now();

    if (tupleType == 'N')
        _LastChange.NewValues = ParseTupleData(cursor, end, relIt->second);

    LOG_TRACE("INSERT {}.{} cols={}",
              _LastChange.SchemaName, _LastChange.TableName, _LastChange.NewValues.size());
}

void WalMessageParser::ParseUpdate(const Byte* data, size_t length)
{
    if (length < 7)
    {
        LOG_ERROR("UPDATE message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    const Byte* end = data + length;

    OID relOID = ReadBE32(cursor);

    auto relIt = _Relations.find(relOID);
    if (relIt == _Relations.end())
    {
        LOG_ERROR("UPDATE references unknown relation OID {}", relOID);
        ++_ParseErrors;
        return;
    }

    _LastChange = {};
    _LastChange.Operation      = Operation::Update;
    _LastChange.TransactionXID = _CurrentXID;
    _LastChange.SchemaName     = relIt->second.SchemaName;
    _LastChange.TableName      = relIt->second.TableName;
    _LastChange.CaptureTime    = SteadyClock::now();

    uint8_t tupleType = ReadU8(cursor);

    // 'K' = old key data, 'O' = full old tuple
    if (tupleType == 'K' || tupleType == 'O')
    {
        _LastChange.OldValues = ParseTupleData(cursor, end, relIt->second);
        tupleType = ReadU8(cursor); // next should be 'N'
    }

    if (tupleType == 'N')
        _LastChange.NewValues = ParseTupleData(cursor, end, relIt->second);

    LOG_TRACE("UPDATE {}.{} old={} new={}",
              _LastChange.SchemaName, _LastChange.TableName,
              _LastChange.OldValues.size(), _LastChange.NewValues.size());
}

void WalMessageParser::ParseDelete(const Byte* data, size_t length)
{
    if (length < 7)
    {
        LOG_ERROR("DELETE message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    const Byte* end = data + length;

    OID relOID = ReadBE32(cursor);

    auto relIt = _Relations.find(relOID);
    if (relIt == _Relations.end())
    {
        LOG_ERROR("DELETE references unknown relation OID {}", relOID);
        ++_ParseErrors;
        return;
    }

    _LastChange = {};
    _LastChange.Operation      = Operation::Delete;
    _LastChange.TransactionXID = _CurrentXID;
    _LastChange.SchemaName     = relIt->second.SchemaName;
    _LastChange.TableName      = relIt->second.TableName;
    _LastChange.CaptureTime    = SteadyClock::now();

    uint8_t tupleType = ReadU8(cursor);
    if (tupleType == 'K' || tupleType == 'O')
        _LastChange.OldValues = ParseTupleData(cursor, end, relIt->second);

    LOG_TRACE("DELETE {}.{} old={}",
              _LastChange.SchemaName, _LastChange.TableName, _LastChange.OldValues.size());
}

void WalMessageParser::ParseTruncate(const Byte* data, size_t length)
{
    if (length < 9)
    {
        LOG_ERROR("TRUNCATE message too short: {} bytes", length);
        ++_ParseErrors;
        return;
    }

    const Byte* cursor = data;
    uint32_t relCount = ReadBE32(cursor);
    uint8_t  options  = ReadU8(cursor);

    _LastChange = {};
    _LastChange.Operation      = Operation::Truncate;
    _LastChange.TransactionXID = _CurrentXID;
    _LastChange.CaptureTime    = SteadyClock::now();

    // Read first relation for schema/table
    if (relCount > 0)
    {
        OID relOID = ReadBE32(cursor);
        auto relIt = _Relations.find(relOID);
        if (relIt != _Relations.end())
        {
            _LastChange.SchemaName = relIt->second.SchemaName;
            _LastChange.TableName  = relIt->second.TableName;
        }
    }

    LOG_DEBUG("TRUNCATE {} relations, options=0x{:02X}", relCount, options);
}

void WalMessageParser::ParseType([[maybe_unused]] const Byte* data, size_t length)
{
    // Type messages: informational, log and skip
    LOG_TRACE("TYPE message received ({} bytes)", length);
}

void WalMessageParser::ParseOrigin([[maybe_unused]] const Byte* data, size_t length)
{
    LOG_TRACE("ORIGIN message received ({} bytes)", length);
}

void WalMessageParser::ParseMessage([[maybe_unused]] const Byte* data, size_t length)
{
    LOG_TRACE("MESSAGE (logical decoding) received ({} bytes)", length);
}

// =============================================================================
// Tuple Data Parser
// =============================================================================
std::vector<ColumnValue> WalMessageParser::ParseTupleData(
    const Byte*& cursor, const Byte* end, const RelationInfo& relation)
{
    std::vector<ColumnValue> columns;

    if (cursor >= end) return columns;

    uint16_t colCount = ReadBE16(cursor);
    columns.reserve(colCount);

    auto& typeHandlers = TypeHandlers::Instance();

    for (uint16_t i = 0; i < colCount && cursor < end; ++i)
    {
        ColumnValue col;
        if (i < relation.Columns.size())
        {
            col.Name    = relation.Columns[i].Name;
            col.TypeOID = relation.Columns[i].TypeOID;
        }

        uint8_t colType = ReadU8(cursor);

        switch (colType)
        {
            case 'n': // NULL
                col.IsNull = true;
                col.TextValue = "\\N";
                break;

            case 'u': // Unchanged TOAST (only in UPDATE)
                col.IsNull = false;
                col.TextValue = ""; // unchanged
                break;

            case 't': // Text format
            {
                int32_t dataLen = ReadBE32s(cursor);
                if (dataLen > 0 && cursor + dataLen <= end)
                {
                    col.IsNull = false;
                    // Text mode: data is already text, but escape for COPY
                    col.TextValue = std::string(reinterpret_cast<const char*>(cursor), dataLen);
                    cursor += dataLen;
                }
                else
                {
                    col.IsNull = true;
                }
                break;
            }

            case 'b': // Binary format
            {
                int32_t dataLen = ReadBE32s(cursor);
                if (dataLen > 0 && cursor + dataLen <= end)
                {
                    col.IsNull = false;
                    col.TextValue = typeHandlers.Convert(col.TypeOID, cursor, dataLen);
                    cursor += dataLen;
                }
                else
                {
                    col.IsNull = true;
                }
                break;
            }

            default:
                LOG_WARN("Unknown tuple column type: 0x{:02X}", colType);
                col.IsNull = true;
                break;
        }

        columns.push_back(std::move(col));
    }

    return columns;
}

const RelationInfo* WalMessageParser::GetRelation(OID relOID) const
{
    auto it = _Relations.find(relOID);
    return (it != _Relations.end()) ? &it->second : nullptr;
}

} // namespace DC
