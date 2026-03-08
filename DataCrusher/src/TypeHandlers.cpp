// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Type handlers implementation
// =============================================================================
#include "TypeHandlers.h"
#include "Logger.h"
#include <cmath>
#include <bit>
#include <sstream>
#include <iomanip>

namespace DC
{

TypeHandlers& TypeHandlers::Instance()
{
    static TypeHandlers instance;
    return instance;
}

void TypeHandlers::Register(OID typeOID, HandlerFn handler)
{
    _Handlers[typeOID] = handler;
}

std::string TypeHandlers::Convert(OID typeOID, const Byte* data, size_t length) const
{
    if (data == nullptr || length == 0)
        return "\\N";

    auto it = _Handlers.find(typeOID);
    if (it != _Handlers.end())
        return it->second(data, length);

    // Fallback: treat as text
    return HandleGenericText(data, length);
}

bool TypeHandlers::HasHandler(OID typeOID) const
{
    return _Handlers.contains(typeOID);
}

void TypeHandlers::InitializeBuiltins()
{
    Register(PgTypeOID::Bool,        HandleBool);
    Register(PgTypeOID::Int2,        HandleInt2);
    Register(PgTypeOID::Int4,        HandleInt4);
    Register(PgTypeOID::Int8,        HandleInt8);
    Register(PgTypeOID::Float4,      HandleFloat4);
    Register(PgTypeOID::Float8,      HandleFloat8);
    Register(PgTypeOID::Numeric,     HandleNumeric);
    Register(PgTypeOID::Text,        HandleText);
    Register(PgTypeOID::Varchar,     HandleVarchar);
    Register(PgTypeOID::Char,        HandleText);
    Register(PgTypeOID::BPChar,      HandleBPChar);
    Register(PgTypeOID::Bytea,       HandleBytea);
    Register(PgTypeOID::UUID,        HandleUUID);
    Register(PgTypeOID::Timestamp,   HandleTimestamp);
    Register(PgTypeOID::TimestampTZ, HandleTimestampTZ);
    Register(PgTypeOID::Date,        HandleDate);
    Register(PgTypeOID::Time,        HandleTime);
    Register(PgTypeOID::JSON,        HandleJSON);
    Register(PgTypeOID::JSONB,       HandleJSONB);
    Register(PgTypeOID::OIDType,     HandleOID);
    Register(PgTypeOID::Name,        HandleText);
    Register(PgTypeOID::Inet,        HandleGenericText);
    Register(PgTypeOID::Cidr,        HandleGenericText);
    Register(PgTypeOID::MacAddr,     HandleGenericText);
    Register(PgTypeOID::XML,         HandleText);
    Register(PgTypeOID::Money,       HandleInt8);

    LOG_DEBUG("TypeHandlers initialized with {} built-in handlers", _Handlers.size());
}

// =============================================================================
// Big-Endian Read Helpers
// =============================================================================
uint16_t TypeHandlers::ReadBE16(const Byte* p)
{
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1])
    );
}

uint32_t TypeHandlers::ReadBE32(const Byte* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) <<  8) |
            static_cast<uint32_t>(p[3]);
}

uint64_t TypeHandlers::ReadBE64(const Byte* p)
{
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) <<  8) |
            static_cast<uint64_t>(p[7]);
}

int16_t TypeHandlers::ReadBE16s(const Byte* p) { return static_cast<int16_t>(ReadBE16(p)); }
int32_t TypeHandlers::ReadBE32s(const Byte* p) { return static_cast<int32_t>(ReadBE32(p)); }
int64_t TypeHandlers::ReadBE64s(const Byte* p) { return static_cast<int64_t>(ReadBE64(p)); }

float TypeHandlers::ReadBEFloat(const Byte* p)
{
    uint32_t bits = ReadBE32(p);
    return std::bit_cast<float>(bits);
}

double TypeHandlers::ReadBEDouble(const Byte* p)
{
    uint64_t bits = ReadBE64(p);
    return std::bit_cast<double>(bits);
}

// =============================================================================
// COPY TEXT Escaping
// =============================================================================
std::string TypeHandlers::EscapeForCopy(std::string_view input)
{
    std::string result;
    result.reserve(input.size() + 16);

    for (char c : input)
    {
        switch (c)
        {
            case '\\': result += "\\\\"; break;
            case '\t': result += "\\t";  break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

// =============================================================================
// Type Handler Implementations
// =============================================================================

std::string TypeHandlers::HandleBool(const Byte* data, size_t length)
{
    if (length < 1) return "\\N";
    return data[0] ? "t" : "f";
}

std::string TypeHandlers::HandleInt2(const Byte* data, size_t length)
{
    if (length < 2) return "\\N";
    return std::to_string(ReadBE16s(data));
}

std::string TypeHandlers::HandleInt4(const Byte* data, size_t length)
{
    if (length < 4) return "\\N";
    return std::to_string(ReadBE32s(data));
}

std::string TypeHandlers::HandleInt8(const Byte* data, size_t length)
{
    if (length < 8) return "\\N";
    return std::to_string(ReadBE64s(data));
}

std::string TypeHandlers::HandleFloat4(const Byte* data, size_t length)
{
    if (length < 4) return "\\N";
    float val = ReadBEFloat(data);
    if (std::isnan(val)) return "NaN";
    if (std::isinf(val)) return val > 0 ? "Infinity" : "-Infinity";
    return std::format("{:.7g}", val);
}

std::string TypeHandlers::HandleFloat8(const Byte* data, size_t length)
{
    if (length < 8) return "\\N";
    double val = ReadBEDouble(data);
    if (std::isnan(val)) return "NaN";
    if (std::isinf(val)) return val > 0 ? "Infinity" : "-Infinity";
    return std::format("{:.15g}", val);
}

std::string TypeHandlers::HandleNumeric(const Byte* data, size_t length)
{
    // PostgreSQL numeric binary format:
    // int16 ndigits, int16 weight, int16 sign, int16 dscale, int16[ndigits] digits
    if (length < 8) return "\\N";

    int16_t ndigits = ReadBE16s(data);
    int16_t weight  = ReadBE16s(data + 2);
    uint16_t sign   = ReadBE16(data + 4);
    int16_t dscale  = ReadBE16s(data + 6);

    constexpr uint16_t NUMERIC_NEG  = 0x4000;
    constexpr uint16_t NUMERIC_NAN  = 0xC000;
    constexpr uint16_t NUMERIC_PINF = 0xD000;
    constexpr uint16_t NUMERIC_NINF = 0xF000;

    if (sign == NUMERIC_NAN)  return "NaN";
    if (sign == NUMERIC_PINF) return "Infinity";
    if (sign == NUMERIC_NINF) return "-Infinity";

    if (ndigits == 0)
    {
        if (dscale > 0)
        {
            std::string result = "0.";
            result.append(static_cast<size_t>(dscale), '0');
            return sign == NUMERIC_NEG ? "-" + result : result;
        }
        return sign == NUMERIC_NEG ? "-0" : "0";
    }

    // Build the decimal string from base-10000 digits
    std::string intPart;
    std::string fracPart;

    for (int16_t i = 0; i < ndigits; ++i)
    {
        if (8 + i * 2 + 1 >= static_cast<int16_t>(length))
            break;
        int16_t digit = ReadBE16s(data + 8 + i * 2);

        if (i <= weight)
        {
            // Integer part
            if (i == 0)
                intPart += std::to_string(digit);
            else
                intPart += std::format("{:04d}", digit);
        }
        else
        {
            // Fractional part
            fracPart += std::format("{:04d}", digit);
        }
    }

    // Fill missing integer digits
    for (int16_t i = ndigits; i <= weight; ++i)
        intPart += "0000";

    if (intPart.empty()) intPart = "0";

    std::string result;
    if (sign == NUMERIC_NEG)
        result = "-";
    result += intPart;

    if (dscale > 0)
    {
        // Truncate or pad fractional part to dscale
        if (fracPart.size() > static_cast<size_t>(dscale))
            fracPart.resize(static_cast<size_t>(dscale));
        else
            fracPart.append(static_cast<size_t>(dscale) - fracPart.size(), '0');
        result += '.';
        result += fracPart;
    }

    return result;
}

std::string TypeHandlers::HandleText(const Byte* data, size_t length)
{
    std::string_view sv(reinterpret_cast<const char*>(data), length);
    return EscapeForCopy(sv);
}

std::string TypeHandlers::HandleVarchar(const Byte* data, size_t length)
{
    return HandleText(data, length);
}

std::string TypeHandlers::HandleBPChar(const Byte* data, size_t length)
{
    return HandleText(data, length);
}

std::string TypeHandlers::HandleBytea(const Byte* data, size_t length)
{
    // Output as hex: \\x followed by hex digits (escaped backslash for COPY)
    std::string result = "\\\\x";
    result.reserve(result.size() + length * 2);
    constexpr char hexDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i)
    {
        result += hexDigits[data[i] >> 4];
        result += hexDigits[data[i] & 0x0F];
    }
    return result;
}

std::string TypeHandlers::HandleUUID(const Byte* data, size_t length)
{
    if (length < 16) return "\\N";
    constexpr char hex[] = "0123456789abcdef";
    char buf[37]; // 8-4-4-4-12 + null
    int pos = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            buf[pos++] = '-';
        buf[pos++] = hex[data[i] >> 4];
        buf[pos++] = hex[data[i] & 0x0F];
    }
    buf[pos] = '\0';
    return std::string(buf, 36);
}

std::string TypeHandlers::HandleTimestamp(const Byte* data, size_t length)
{
    if (length < 8) return "\\N";

    // PostgreSQL timestamp: microseconds since 2000-01-01 00:00:00
    int64_t pgTimestampUS = ReadBE64s(data);

    // Special values
    constexpr int64_t PG_TIMESTAMP_INF  = INT64_MAX;
    constexpr int64_t PG_TIMESTAMP_NINF = INT64_MIN;
    if (pgTimestampUS == PG_TIMESTAMP_INF)  return "infinity";
    if (pgTimestampUS == PG_TIMESTAMP_NINF) return "-infinity";

    // Convert to Unix epoch: PostgreSQL epoch is 2000-01-01, Unix is 1970-01-01
    // Difference: 10957 days = 946684800 seconds
    constexpr int64_t PG_EPOCH_OFFSET_US = 946'684'800'000'000LL;
    int64_t unixUS = pgTimestampUS + PG_EPOCH_OFFSET_US;
    int64_t unixSec = unixUS / 1'000'000;
    int64_t fracUS = unixUS % 1'000'000;
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
        return std::format("{}.{:06d}", buf, fracUS);
    return std::string(buf);
}

std::string TypeHandlers::HandleTimestampTZ(const Byte* data, size_t length)
{
    // Same binary format as timestamp, but interpreted as UTC
    auto result = HandleTimestamp(data, length);
    if (result != "\\N" && result != "infinity" && result != "-infinity")
        result += "+00";
    return result;
}

std::string TypeHandlers::HandleDate(const Byte* data, size_t length)
{
    if (length < 4) return "\\N";

    // PostgreSQL date: days since 2000-01-01
    int32_t pgDays = ReadBE32s(data);

    constexpr int32_t PG_DATE_INF  = INT32_MAX;
    constexpr int32_t PG_DATE_NINF = INT32_MIN;
    if (pgDays == PG_DATE_INF)  return "infinity";
    if (pgDays == PG_DATE_NINF) return "-infinity";

    // Convert to Unix days
    constexpr int32_t PG_EPOCH_JDATE = 10957; // days from 1970-01-01 to 2000-01-01
    int32_t unixDays = pgDays + PG_EPOCH_JDATE;
    time_t tt = static_cast<time_t>(unixDays) * 86400;

    struct tm tmBuf;
#if DC_PLATFORM_WINDOWS
    gmtime_s(&tmBuf, &tt);
#else
    gmtime_r(&tt, &tmBuf);
#endif

    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tmBuf);
    return std::string(buf);
}

std::string TypeHandlers::HandleTime(const Byte* data, size_t length)
{
    if (length < 8) return "\\N";

    // PostgreSQL time: microseconds since midnight
    int64_t timeUS = ReadBE64s(data);
    int64_t hours = timeUS / 3'600'000'000LL;
    int64_t mins  = (timeUS % 3'600'000'000LL) / 60'000'000LL;
    int64_t secs  = (timeUS % 60'000'000LL) / 1'000'000LL;
    int64_t frac  = timeUS % 1'000'000LL;

    if (frac > 0)
        return std::format("{:02d}:{:02d}:{:02d}.{:06d}", hours, mins, secs, frac);
    return std::format("{:02d}:{:02d}:{:02d}", hours, mins, secs);
}

std::string TypeHandlers::HandleJSON(const Byte* data, size_t length)
{
    return HandleText(data, length);
}

std::string TypeHandlers::HandleJSONB(const Byte* data, size_t length)
{
    // JSONB binary has a 1-byte version prefix
    if (length < 1) return "\\N";
    return HandleText(data + 1, length - 1);
}

std::string TypeHandlers::HandleOID(const Byte* data, size_t length)
{
    if (length < 4) return "\\N";
    return std::to_string(ReadBE32(data));
}

std::string TypeHandlers::HandleGenericText(const Byte* data, size_t length)
{
    return HandleText(data, length);
}

} // namespace DC
