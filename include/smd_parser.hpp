// smd_parser.hpp
// Single-pass parser for the Status Monitor Design (.smd) overlay format.
//
// Workflow:
//   smd::Document doc;
//   doc.LoadFromFile("Design.smd");
//   doc.BindInt64 ("CPU_Hz_int", &liveCpuHz);
//   doc.BindBool  ("showFPS",    &liveShowFps);
//   // ...bind every predefined runtime variable referenced by the file...
//   doc.Compile();
//
//   // Per frame:
//   doc.Evaluate(&MyRenderCallback, this);
//
// Inside MyRenderCallback you receive a smd::RenderCommand with everything
// you need to draw it. For RenderCmdType::GetDimensions, fill cmd.outDims->{x,y}
// before returning so following commands see the result.
//
// Manual cleanup: doc.Free() (destructor calls it too).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

// Forward-declared so consumers don't need to include tinyexpr.h.
struct te_expr;

namespace smd {

// ---------------------------------------------------------------------------
// Configuration values
// ---------------------------------------------------------------------------

enum class ConfigKind : uint8_t {
    Integer,           // decimal or hex int64 literal
    Bool,              // true / false
    String,            // bare "..." literal
    Format,            // { "fmt", arg, arg, ... } -- re-evaluated each frame
    History,           // HISTORY{type, capacity}: ring buffer of values
    GraphConditions,   // GRAPH_CONDITIONS{{cond, lineCol, fillCol}, ...}
    List               // LIST{type, {elem, elem, ...}}: typed array
};

// Element type for ConfigKind::List. Iteration via `#for $x in $list`
// binds $x with the matching type. RANGE{...} inside a LIST{int, ...} body
// only produces integer elements.
enum class ListElemType : uint8_t {
    Int,
    Float,
    Double,
    String
};

struct ListValue {
    ListElemType elemType = ListElemType::Int;
    // Populated according to elemType:
    //   Int    -> ints
    //   Float  -> doubles (promoted; loses no precision beyond what float had)
    //   Double -> doubles
    //   String -> strings (post-unescape)
    std::vector<int64_t>     ints;
    std::vector<double>      doubles;
    std::vector<std::string> strings;
};

struct FormatSpec {
    std::string fmt;
    std::vector<std::string> argExprs;
    std::vector<bool>        argIsString;
    std::vector<te_expr*>    compiledArgs;
    std::vector<void*>       argTernaries;
};

struct ConfigValue {
    ConfigKind   kind = ConfigKind::Integer;
    int64_t      intVal  = 0;
    bool         boolVal = false;
    std::string  stringVal;
    FormatSpec   fmtVal;
    ListValue    listVal;
    // True when the config line used `key = value` instead of `key: value`.
    // The two are both compile-time-once seeds for matching VAR storage;
    // the difference is mutability:
    //   `:` -> default-kind. The script may re-assign via VAR.
    //   `=` -> constant-kind. The name is immutable; VAR cannot shadow or
    //          reassign it (compile-time error if it tries).
    // Neither kind is re-seeded by Reset(); both persist whatever the
    // script (or for `:`, the script) most recently wrote. Use `=` for
    // names that the script should treat as fixed (constants, history
    // declarations, layer dimensions, the overlay Name, etc.).
    bool         isStateKind = false;
};

struct Dimensions {
    int64_t x = 0;
    int64_t y = 0;
};

// Resolution telemetry entry. Host packs an array of 8 of these and passes
// the pointer to BindResolutionArray(). Layout is { width, height, calls }
// in this order so it maps cleanly onto SaltyNX's uint16 packing.
struct ResolutionEntry {
    uint16_t width;
    uint16_t height;
    uint16_t calls;
};

enum class RenderCmdType : uint8_t {
    Text,
    Box,
    RoundedBox,
    EmptyBox,
    DashedLine,
    GetDimensions,
#ifdef DEBUG
    // HistoryUpdate and HistoryClean fire whenever a HISTORY_UPDATE /
    // HISTORY_CLEAN command runs in the script. They're debug-only: in
    // release builds the parser still maintains the underlying ring
    // buffer (used by GraphLineChart) but never invokes the host callback
    // for these node kinds. Define DEBUG (e.g. `-DDEBUG=1`) to receive
    // them and use them for instrumentation.
    HistoryUpdate,
    HistoryClean,
#endif
    GraphLineChart
};

enum class GraphDirection : uint8_t { LeftToRight, RightToLeft };

// Element type of the samples carried by a GraphLineChart RenderCommand.
// The .smd author picks this via `HISTORY{int|float|double, capacity}`.
//   - Int    -> read cmd.samples  (const int64_t*)
//   - Float  -> read cmd.samplesD (const double*); values originated as float
//               and were promoted to double for transport (no precision lost
//               beyond what float already had)
//   - Double -> read cmd.samplesD (const double*)
// Exactly one of `samples` / `samplesD` is non-null; the other is nullptr.
enum class HistorySampleType : uint8_t {
    Int,
    Float,
    Double,
};

struct GraphConditionView {
    int64_t  lineColor;
    int64_t  fillColor;
    // Sample value is delivered as double regardless of the history's
    // declared type -- the underlying condition is just arithmetic on `x`
    // (e.g. `x < 30`), which the parser already evaluates in double.
    // Int-typed histories convert their int64 samples to double on the
    // way in.
    bool (*matches)(void* state, double sampleValue);
    void* state;
};

struct RenderCommand {
    RenderCmdType type;

    int64_t  x = 0, y = 0, width = 0, height = 0, fontSize = 0;
    uint16_t color = 0;

    int64_t  x2 = 0, y2 = 0, dashOn = 0, dashOff = 0;
    float roundnessTl = 0, roundnessTr = 0, roundnessBl = 0, roundnessBr = 0;

    int64_t  minClamp = 0, maxClamp = 0;
    uint16_t fillColor = 0;
    GraphDirection direction = GraphDirection::LeftToRight;
    // GraphLineChart sample view.
    //
    // Inspect `sampleType` and then read from the matching pointer:
    //   sampleType == HistorySampleType::Int    -> samples  (int64_t)
    //   sampleType == HistorySampleType::Float  -> samplesD (double, was float)
    //   sampleType == HistorySampleType::Double -> samplesD (double)
    // Whichever pointer doesn't apply is nullptr. sampleCount/sampleCapacity
    // are valid regardless of type. Both pointers can be nullptr when the
    // ring is empty (sampleCount == 0).
    HistorySampleType sampleType = HistorySampleType::Int;
    const int64_t* samples = nullptr;
    const double*  samplesD = nullptr;
    size_t         sampleCount = 0;
    size_t         sampleCapacity = 0;
    const GraphConditionView* conditions = nullptr;
    size_t                    condCount  = 0;

    // historyName is set by GraphLineChart (always) and by the debug-only
    // HistoryUpdate/HistoryClean commands. It points at the history buffer
    // referenced by the command and is valid for the duration of the
    // callback.
    const char* historyName = nullptr;
#ifdef DEBUG
    // historyValue is set only by the debug-only HistoryUpdate command
    // (carrying the value being pushed onto the history ring). Gated on
    // the same macro as RenderCmdType::HistoryUpdate / HistoryClean.
    // Always delivered as double so float / double HISTORYs surface their
    // full precision; for int HISTORYs cast back with (int64_t).
    double      historyValue = 0.0;
#endif

    std::string text;

    Dimensions* outDims = nullptr;
    const char* dimsName = nullptr;
};

class Document {
public:
    typedef void (*Callback)(RenderCommand& cmd, void* user);

    // Called once per Document on the first IETF{} declaration encountered
    // during LoadFromFile / LoadFromMemory. The host writes the desired IETF
    // locale key (e.g. "PL-PL") into outLocaleCode. The parser caches it for
    // the Document's lifetime; it will not be queried again even after a
    // subsequent Load. Must be set before Load for it to take effect.
    typedef void (*RecordCallback)(std::string& outLocaleCode, void* user);

    Document();
    ~Document();

    Document(const Document&)            = delete;
    Document& operator=(const Document&) = delete;
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;

    bool LoadFromFile  (const char* path);
    bool LoadFromMemory(const char* data, size_t size);
    uint32_t GetFileHash();

    // Set the callback used to query the host for the active IETF locale key.
    // Call before LoadFromFile / LoadFromMemory. The callback and user pointer
    // survive Free() so they do not need to be re-set between loads.
    void SetRecordCallback(RecordCallback cb, void* user);

    struct PeekInfo {
        std::string name;
        int64_t     layerWidth  = 0;
        int64_t     layerHeight = 0;
    };
    // ietf_code: optional IETF locale key (e.g. "PL-PL"). When non-null and
    // a matching NAME_LOCALE{} directive is present in the file, the
    // localised name is returned instead of the default Name value.
    static bool Peek          (const char* path, PeekInfo& out,
                               const char* ietf_code = nullptr);
    static bool PeekFromMemory(const char* data, size_t size, PeekInfo& out,
                               const char* ietf_code = nullptr);
    static bool PeekName          (const char* path, std::string& outName,
                                   const char* ietf_code = nullptr);
    static bool PeekNameFromMemory(const char* data, size_t size,
                                   std::string& outName,
                                   const char* ietf_code = nullptr);

    void BindInt64 (const char* name, const int64_t* ptr);
    void BindFloat (const char* name, const float*   ptr);
    void BindDouble(const char* name, const double*  ptr);
    void BindBool  (const char* name, const bool*    ptr);
    void BindString(const char* name, const std::string* ptr);

    // Bind a resolution-telemetry array of 8 entries. Used for the predefined
    // names Game_ResolutionRenderCalls_int and Game_ResolutionViewportCalls_int
    // (and any other array of ResolutionEntry the script references in the
    // future). The pointer must outlive the Document; the parser reads
    // width/height/calls per entry every frame inside Evaluate().
    void BindResolutionArray(const char* name, const ResolutionEntry* entries);

    bool Compile();
    bool Evaluate(Callback cb, void* user);
    void Reset(bool freeze = false);
    void ClearDimsMeasureCache();
    void Free();

    const ConfigValue* GetConfig(const char* key) const;
    int64_t            GetConfigInt   (const char* key, int64_t def = 0)       const;
    bool               GetConfigBool  (const char* key, bool def = false)      const;
    const char*        GetConfigString(const char* key, const char* def = "")  const;

    bool FormatConfigString(const char* key, std::string& out);

    const char* LastError() const;

    struct Impl;

private:
    Impl* m_impl;
};

} // namespace smd
