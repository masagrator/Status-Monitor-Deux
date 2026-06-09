#pragma once
#include "smse_types.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <charconv>


// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace smse::detail {

inline std::string_view trim(std::string_view s) noexcept {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' ||
                          s.front() == '\r' || s.front() == '\n'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

inline std::string_view stripQuotes(std::string_view s) noexcept {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

inline std::vector<std::string_view> split(std::string_view s, char delim) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == delim) {
            auto part = trim(s.substr(start, i - start));
            if (!part.empty())
                out.push_back(part);
            start = i + 1;
        }
    }
    return out;
}

// Parse hex (0x...) or decimal into size_t
inline std::optional<size_t> parseNum(std::string_view s) noexcept {
    s = trim(s);
    if (s.starts_with("0x") || s.starts_with("0X")) {
        size_t v = 0;
        auto [ptr, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), v, 16);
        if (ec != std::errc{}) return std::nullopt;
        return v;
    }
    size_t v = 0;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 10);
    if (ec != std::errc{}) return std::nullopt;
    return v;
}

enum class Section { None, Structs, Cmds, Asserts, Execution };

// Return true if line is only whitespace / empty
inline bool blankLine(std::string_view s) noexcept {
    return std::all_of(s.begin(), s.end(),
                       [](char c){ return c == ' ' || c == '\t' ||
                                          c == '\r' || c == '\n'; });
}

} // namespace smse::detail

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

namespace smse {

using detail::trim;
using detail::stripQuotes;
using detail::split;
using detail::parseNum;
using detail::blankLine;
using detail::Section;

// Forward-declare result type for parser
using ParseResult = std::variant<SmseParsedFile, SmseError>;

// Current struct being accumulated inside the structs section
struct StructAccum {
    std::string              name;
    size_t                   bufSize = 0;  // from command referencing it; set later
    std::vector<SmseFieldDesc> fields;
};

static ParseResult parseFile(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return SmseError::make(SmseErrorCode::ParseMalformedLine, path,
                               "Cannot open file");

    std::fseek(f, 0, SEEK_END);
    long fileSize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);

    std::string fileContents(static_cast<size_t>(fileSize), '\0');
    std::fread(fileContents.data(), 1, static_cast<size_t>(fileSize), f);
    std::fclose(f);

    SmseParsedFile out;
    out.sourceFile = path;

    Section section = Section::None;

    // Accumulates current struct while inside structs section
    std::string   curStructName;
    std::vector<SmseFieldDesc> curStructFields;

    // Lambda to flush accumulated struct into out.structs
    auto flushStruct = [&]() {
        if (curStructName.empty()) return;
        auto& sd = out.structs[curStructName];
        sd.name = curStructName;
        // Merge fields (later files add more fields to same struct)
        for (auto& fld : curStructFields)
            sd.fields.push_back(std::move(fld));
        curStructName.clear();
        curStructFields.clear();
    };

    // Walk the buffer line by line without any stream overhead
    const char* cur = fileContents.data();
    const char* end = cur + fileContents.size();
    while (cur < end) {
        const char* nl = static_cast<const char*>(
            std::memchr(cur, '\n', static_cast<size_t>(end - cur)));
        const char* lineEnd = nl ? nl : end;

        // Strip trailing CR (CRLF normalisation)
        size_t lineLen = static_cast<size_t>(lineEnd - cur);
        if (lineLen > 0 && cur[lineLen - 1] == '\r') --lineLen;

        std::string_view line = trim(std::string_view(cur, lineLen));
        cur = nl ? nl + 1 : end;

        if (blankLine(line)) continue;

        // ---- Section headers ----
        if (line == "structs:") { flushStruct(); section = Section::Structs; continue; }
        if (line == "cmds:")    { flushStruct(); section = Section::Cmds;    continue; }
        if (line == "asserts:") { flushStruct(); section = Section::Asserts; continue; }
        if (line == "execution:") { flushStruct(); section = Section::Execution; continue; }

        // ---- service: <name> ----
        if (line.starts_with("service:")) {
            out.serviceName = std::string(trim(line.substr(8)));
            continue;
        }

        // ---- Comments ----
        if (line.starts_with('#') || line.starts_with("//")) continue;

        switch (section) {
        // ----------------------------------------------------------------
        case Section::Structs: {
            // "- HocClkContext:"  -> starts a new struct block
            if (line.starts_with("- ") && line.ends_with(':')) {
                flushStruct();
                curStructName = std::string(trim(line.substr(2, line.size() - 3)));
                break;
            }
            // "- 0x14, u32, "CPU_RealHz_int""
            if (line.starts_with("- ") && !curStructName.empty()) {
                auto parts = split(line.substr(2), ',');
                if (parts.size() < 3)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad field line: %s", std::string(line).c_str()));
                auto offset = parseNum(parts[0]);
                if (!offset)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad offset: %s", std::string(parts[0]).c_str()));
                auto vt = parseValueType(parts[1]);
                if (!vt)
                    return SmseError::make(SmseErrorCode::ParseUnknownType,
                                           path, fmt("Unknown type: %s", std::string(parts[1]).c_str()));
                curStructFields.push_back({
                    *offset, *vt,
                    std::string(stripQuotes(parts[2]))
                });
                break;
            }
            return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                   path, fmt("Unexpected line in structs: %s", std::string(line).c_str()));
        }

        // ----------------------------------------------------------------
        case Section::Cmds: {
            // - "GetApiVersion", 0, u32
            // - "GetCurrentContext", 2, buffer, 0x500, struct HocClkContext
            // - "GetVersionString", 1, buffer, 0x100, char
            if (!line.starts_with("- "))
                return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                       path, fmt("Expected '- ' in cmds: %s", std::string(line).c_str()));
            auto parts = split(line.substr(2), ',');
            if (parts.size() < 3)
                return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                       path, fmt("Bad cmd line: %s", std::string(line).c_str()));

            SmseCommandDesc cmd;
            cmd.name  = std::string(stripQuotes(parts[0]));
            {
                auto id = parseNum(parts[1]);
                if (!id)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad cmd id: %s", std::string(parts[1]).c_str()));
                cmd.cmdId = static_cast<u32>(*id);
            }

            std::string_view typeStr = parts[2];
            if (typeStr == "buffer" || typeStr == "struct") {
                cmd.isBuffer = (typeStr == "buffer");
                cmd.isInlineStruct = (typeStr == "struct");
                
                if (parts.size() < 5)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad complex cmd: %s", std::string(line).c_str()));
                auto bsz = parseNum(parts[3]);
                if (!bsz)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad size: %s", std::string(parts[3]).c_str()));
                cmd.bufSize = *bsz;

                std::string_view bufType = trim(parts[4]);
                if (bufType == "char") {
                    cmd.bufIsChar = true;
                } else if (bufType.starts_with("struct ")) {
                    cmd.bufStructName = std::string(trim(bufType.substr(7)));
                } else {
                    return SmseError::make(SmseErrorCode::ParseUnknownType,
                                           path, fmt("Unknown complex type: %s", std::string(bufType).c_str()));
                }
                // Validate / register struct size
                if (!cmd.bufIsChar) {
                    auto it = out.structs.find(cmd.bufStructName);
                    if (it == out.structs.end())
                        return SmseError::make(SmseErrorCode::ParseUnknownStruct,
                                               path, fmt("Unknown struct: %s", cmd.bufStructName.c_str()));
                    if (it->second.bufSize == 0) {
                        it->second.bufSize = cmd.bufSize;
                    } else if (it->second.bufSize != cmd.bufSize) {
                        return SmseError::make(SmseErrorCode::ParseStructSizeConflict,
                                               path,
                                               fmt("Struct '%s' size conflict: previously %zu now %zu",
                                                           cmd.bufStructName.c_str(),
                                                           it->second.bufSize,
                                                           cmd.bufSize));
                    }
                }
            } else {
                auto vt = parseValueType(typeStr);
                if (!vt)
                    return SmseError::make(SmseErrorCode::ParseUnknownType,
                                           path, fmt("Unknown type: %s", std::string(typeStr).c_str()));
                cmd.outputType = *vt;
            }

            // Deduplicate by cmdId per service (handled later at load time; store all here)
            out.cmds.push_back(std::move(cmd));
            break;
        }

        // ----------------------------------------------------------------
        case Section::Asserts: {
            std::string_view sv = line;
            auto parenClose = sv.find("()");
            if (parenClose == std::string_view::npos)
                return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                       path, fmt("Bad assert: %s", std::string(line).c_str()));
            
            std::string cmdName(sv.substr(0, parenClose));
            sv = trim(sv.substr(parenClose + 2));

            SmseAssertDesc ad;
            ad.cmdName = cmdName;

            // 1. Look up the command to determine its return type
            auto cmdIt = std::find_if(out.cmds.begin(), out.cmds.end(),
                                      [&](const SmseCommandDesc& c) { return c.name == cmdName; });
            
            if (cmdIt == out.cmds.end()) {
                return SmseError::make(SmseErrorCode::AssertUnknownCommand,
                                       path, fmt("Assert references unknown command: %s", cmdName.c_str()));
            }

            // 2. Check if the command outputs a char buffer (string)
            bool isString = (cmdIt->isBuffer || cmdIt->isInlineStruct) && cmdIt->bufIsChar;

            if (isString) {
                // -- String / Regex Assertions --
                if (sv.starts_with("==")) {
                    ad.op = SmseAssertOp::EQ_REGEX;
                    sv = trim(sv.substr(2));
                } else if (sv.starts_with("!=")) {
                    ad.op = SmseAssertOp::NE_REGEX;
                    sv = trim(sv.substr(2));
                } else {
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Invalid string assert op (expected == or !=): %s", std::string(line).c_str()));
                }
                ad.regex = std::string(stripQuotes(sv));
            } else {
                // -- Numeric Assertions --
                size_t opLen = 0;
                
                // Check 2-character ops first
                if (sv.starts_with(">="))      { ad.op = SmseAssertOp::GE; opLen = 2; }
                else if (sv.starts_with("<=")) { ad.op = SmseAssertOp::LE; opLen = 2; }
                else if (sv.starts_with("==")) { ad.op = SmseAssertOp::EQ; opLen = 2; }
                else if (sv.starts_with("!=")) { ad.op = SmseAssertOp::NE; opLen = 2; }
                // Check 1-character ops
                else if (sv.starts_with(">"))  { ad.op = SmseAssertOp::GT; opLen = 1; }
                else if (sv.starts_with("<"))  { ad.op = SmseAssertOp::LT; opLen = 1; }
                else {
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Invalid numeric assert op: %s", std::string(line).c_str()));
                }

                auto numStr = trim(sv.substr(opLen));
                auto v = parseNum(numStr);
                if (!v)
                    return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                           path, fmt("Bad numeric assert value: %s", std::string(numStr).c_str()));
                
                ad.numericValue = *v;
            }

            out.asserts.push_back(std::move(ad));
            break;
        }

        // ----------------------------------------------------------------
        case Section::Execution: {
            // GetCurrentContext()
            std::string_view sv = line;
            auto paren = sv.find('(');
            if (paren == std::string_view::npos)
                return SmseError::make(SmseErrorCode::ParseMalformedLine,
                                       path, fmt("Bad exec entry: %s", std::string(line).c_str()));
            out.execCmds.push_back(std::string(sv.substr(0, paren)));
            break;
        }

        case Section::None:
            return SmseError::make(SmseErrorCode::ParseUnknownSection,
                                   path, fmt("Line before any section: %s", std::string(line).c_str()));
        }
    }

    flushStruct();
    return out;
}

} // namespace smse