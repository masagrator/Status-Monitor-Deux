#pragma once
#include <switch.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <variant>
#include <span>
#include <optional>

// ---------------------------------------------------------------------------
// Error context
// ---------------------------------------------------------------------------

enum class SmseErrorCode : u32 {
    Ok = 0,
    // connection
    ServiceConnectFailed,
    // parsing
    ParseUnknownSection,
    ParseUnknownType,
    ParseMalformedLine,
    ParseUnknownStruct,
    ParseStructSizeConflict,
    // asserts
    AssertVersionTooLow,
    AssertVersionStringMismatch,
    AssertUnknownCommand,
    AssertBadOutputType,
    // execution
    ExecCommandFailed,
    ExecCommandNotFound,
};

struct SmseError {
    SmseErrorCode code   = SmseErrorCode::Ok;
    Result        rc     = 0;          // libnx Result when relevant
    std::string   file;                // .smse filename
    std::string   detail;              // human-readable description

    [[nodiscard]] bool ok() const noexcept { return code == SmseErrorCode::Ok; }

    static SmseError success() noexcept { return {}; }

    static SmseError make(SmseErrorCode c, std::string_view f,
                          std::string_view msg, Result r = 0)
    {
        return { c, r, std::string(f), std::string(msg) };
    }
};

// ---------------------------------------------------------------------------
// Value type enum
// ---------------------------------------------------------------------------

enum class SmseValueType : u8 {
    u8_, u16_, u32_, u64_,
    s8_, s16_, s32_, s64_,
    f32_, f64_,
    buffer_char,
    buffer_struct,
};

inline std::optional<SmseValueType> parseValueType(std::string_view s) noexcept {
    if (s == "u8")  return SmseValueType::u8_;
    if (s == "u16") return SmseValueType::u16_;
    if (s == "u32") return SmseValueType::u32_;
    if (s == "u64") return SmseValueType::u64_;
    if (s == "s8")  return SmseValueType::s8_;
    if (s == "s16") return SmseValueType::s16_;
    if (s == "s32") return SmseValueType::s32_;
    if (s == "s64") return SmseValueType::s64_;
    if (s == "f32") return SmseValueType::f32_;
    if (s == "f64") return SmseValueType::f64_;
    return std::nullopt;
}

inline size_t valueTypeSize(SmseValueType t) noexcept {
    switch (t) {
        case SmseValueType::u8_:  case SmseValueType::s8_:  return 1;
        case SmseValueType::u16_: case SmseValueType::s16_: return 2;
        case SmseValueType::u32_: case SmseValueType::s32_:
        case SmseValueType::f32_:                           return 4;
        case SmseValueType::u64_: case SmseValueType::s64_:
        case SmseValueType::f64_:                           return 8;
        default: return 0;
    }
}

// ---------------------------------------------------------------------------
// Struct field descriptor
// ---------------------------------------------------------------------------

struct SmseFieldDesc {
    size_t       offset;
    SmseValueType type;
    std::string  name;
};

// ---------------------------------------------------------------------------
// Struct descriptor  (name -> fields, fixed buffer size)
// ---------------------------------------------------------------------------

struct SmseStructDesc {
    std::string              name;
    size_t                   bufSize = 0;
    std::vector<SmseFieldDesc> fields;
};

// ---------------------------------------------------------------------------
// Command descriptor
// ---------------------------------------------------------------------------

struct SmseCommandDesc {
    std::string   name;
    u32           cmdId;
    SmseValueType outputType;         // for non-buffer commands
    bool          isBuffer  = false;
    bool          isInlineStruct = false;
    size_t        bufSize   = 0;
    bool          bufIsChar = false;
    std::string   bufStructName;      // only when !bufIsChar
};

// ---------------------------------------------------------------------------
// Assert descriptor
// ---------------------------------------------------------------------------

enum class SmseAssertOp { 
    // Numeric
    EQ, NE, LT, LE, GT, GE, 
    // Regex
    EQ_REGEX, NE_REGEX 
};

struct SmseAssertDesc {
    std::string  cmdName;
    SmseAssertOp op;
    u64          numericValue = 0;   // for numeric operators
    std::string  regex;              // for EQ_REGEX / NE_REGEX
};

// ---------------------------------------------------------------------------
// Parsed .smse file (pre-connect, lightweight)
// ---------------------------------------------------------------------------

struct SmseParsedFile {
    std::string                                     sourceFile;
    std::string                                     serviceName;
    std::unordered_map<std::string, SmseStructDesc> structs;
    std::vector<SmseCommandDesc>                    cmds;
    std::vector<SmseAssertDesc>                     asserts;
    std::vector<std::string>                        execCmds;  // ordered
};


// ---------------------------------------------------------------------------
// snprintf helper – returns a std::string, mirrors std::format ergonomics
// ---------------------------------------------------------------------------

template<typename... Args>
static inline std::string fmt(const char* f, Args&&... a) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), f, std::forward<Args>(a)...);
    return buf;
}