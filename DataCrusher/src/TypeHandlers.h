// =============================================================================
// DataCrusher - High-Performance PostgreSQL CDC Engine
// Tootega Pesquisa e Inovação - All rights reserved © 1999-2026
// Type handlers for PostgreSQL data type conversion
// =============================================================================
#pragma once

#include "Platform.h"
#include "Constants.h"
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>

namespace DC
{

// Output format for COPY command: tab-delimited text
// Each TypeHandler converts binary WAL data to text suitable for COPY STDIN

class TypeHandlers final
{
public:
    // Handler function signature: takes raw bytes, returns text representation
    // suitable for PostgreSQL COPY TEXT format
    using HandlerFn = std::string(*)(const Byte* data, size_t length);

    static TypeHandlers& Instance();

    // Register a handler for a PostgreSQL OID
    void Register(OID typeOID, HandlerFn handler);

    // Convert binary data for a given type OID to COPY-ready text
    // Returns the formatted string, or "\\N" for NULL
    std::string Convert(OID typeOID, const Byte* data, size_t length) const;

    // Check if a handler exists for a given OID
    bool HasHandler(OID typeOID) const;

    // Initialize all built-in type handlers
    void InitializeBuiltins();

private:
    TypeHandlers() = default;

    std::unordered_map<OID, HandlerFn> _Handlers;

    // Built-in handler implementations
    static std::string HandleBool(const Byte* data, size_t length);
    static std::string HandleInt2(const Byte* data, size_t length);
    static std::string HandleInt4(const Byte* data, size_t length);
    static std::string HandleInt8(const Byte* data, size_t length);
    static std::string HandleFloat4(const Byte* data, size_t length);
    static std::string HandleFloat8(const Byte* data, size_t length);
    static std::string HandleNumeric(const Byte* data, size_t length);
    static std::string HandleText(const Byte* data, size_t length);
    static std::string HandleVarchar(const Byte* data, size_t length);
    static std::string HandleBPChar(const Byte* data, size_t length);
    static std::string HandleBytea(const Byte* data, size_t length);
    static std::string HandleUUID(const Byte* data, size_t length);
    static std::string HandleTimestamp(const Byte* data, size_t length);
    static std::string HandleTimestampTZ(const Byte* data, size_t length);
    static std::string HandleDate(const Byte* data, size_t length);
    static std::string HandleTime(const Byte* data, size_t length);
    static std::string HandleJSON(const Byte* data, size_t length);
    static std::string HandleJSONB(const Byte* data, size_t length);
    static std::string HandleOID(const Byte* data, size_t length);
    static std::string HandleGenericText(const Byte* data, size_t length);

    // Helpers
    static uint16_t ReadBE16(const Byte* p);
    static uint32_t ReadBE32(const Byte* p);
    static uint64_t ReadBE64(const Byte* p);
    static int16_t  ReadBE16s(const Byte* p);
    static int32_t  ReadBE32s(const Byte* p);
    static int64_t  ReadBE64s(const Byte* p);
    static float    ReadBEFloat(const Byte* p);
    static double   ReadBEDouble(const Byte* p);

    // Escape special chars for COPY TEXT format (tab, newline, backslash)
    static std::string EscapeForCopy(std::string_view input);
};

} // namespace DC
