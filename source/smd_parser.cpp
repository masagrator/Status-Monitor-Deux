// smd_parser.cpp
// Implementation of the .smd parser & evaluator.
// SMD file design by MasaGratoR
// Implementation of parser was done mostly by Claude Opus 4.7 with tidbits of Sonnet 4.6

#include "smd_parser.hpp"

extern "C" {
#include "tinyexpr.h"
}

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <switch.h>

// Force everything in this block to be optimized for speed (O3)
#pragma GCC push_options
#pragma GCC optimize ("O3")

namespace smd {

// ===========================================================================
// AST
// ===========================================================================

// A compiled expression plus its original source. Source is kept so we can
// identifier-scan during Compile() and so error messages can quote it.
struct CExpr {
	std::string source;
	te_expr*    compiled = nullptr;
};

enum class NodeKind : uint8_t {
	If,
	For,
	Text,
	Box,
	RoundedBox,
	EmptyBox,
	DashedLine,
	Var,
	GetDimensions,
	HistoryUpdate,
	HistoryClean,
	GraphLineChart
};

// A string expression is a sequence of segments joined by `+`. Each segment
// is either a string literal (already escape-decoded) or an identifier name
// that resolves at materialise time. Used for:
//   * the RHS of VAR{name, ...} when classified as string-valued
//   * (future) any context that wants concatenation
//
// Pure-identifier text sources (TEXT / GET_DIMENSIONS last arg without `+`)
// still use the older textIsLiteral/textStrOrKey pair, since they don't
// need concatenation.
struct StringExprSegment {
	bool        isLiteral;   // true: text is the literal contents
							 // false: text is an identifier name
	std::string text;
	// Non-null overrides the above: this segment is a `{"fmt", args...}`
	// inline format spec to materialise at eval time. The pointer is
	// typed-erased (FormatSpec*) and owned by Document::Impl::ownedNestedFormats.
	void*       nestedFmt = nullptr;
};
struct StringExpr {
	std::vector<StringExprSegment> segments;
};

// ---- Format-arg trees ----
//
// A FormatSpec's positional argument can be one of:
//   * Numeric -- compiled tinyexpr expression; result is a double.
//   * String literal expression (a StringExpr with `+` joins).
//   * Nested FormatSpec (a "{"...", ...}" literal sitting in an arg slot).
//   * Ternary -- pick between two child FormatArg nodes based on a
//     numeric condition (compiled tinyexpr expression).
//
// We use one struct with a discriminator. Children are owned by the
// Impl's `ownedFormatArgs` pool so the public FormatSpec only holds
// pointers (kept stable across moves).

enum class FormatArgKind : uint8_t {
	Numeric,
	StringExpr_,
	NestedFormat,
	Ternary
};

struct FormatArg {
	FormatArgKind kind = FormatArgKind::Numeric;

	// Numeric
	std::string  numericSource;
	te_expr*     numericCompiled = nullptr;

	// String expression
	StringExpr   strExpr;

	// Nested format spec ({"...", ...} appearing in a ternary branch).
	// Stored as a raw FormatSpec; compileTime walks it.
	std::unique_ptr<FormatSpec> nested;

	// Ternary
	std::string condSource;
	te_expr*    condCompiled = nullptr;
	FormatArg*  thenArg = nullptr;
	FormatArg*  elseArg = nullptr;
};

// ---- History buffers (HISTORY{int|float|double, capacity}) ----
enum class HistoryType : uint8_t {
	Int,
	Float,
	Double,
};

struct HistoryDecl {
	HistoryType type     = HistoryType::Int;
	size_t      capacity = 0;
	// Ring buffer of samples. Only the vector matching `type` is ever
	// non-empty; the others stay at size() == 0. We keep all three rather
	// than a tagged union so update/read code can fan out at the type
	// switch and use plain `.push_back` / `.erase` / `.data` afterwards.
	// The wasted size_t triple per HISTORY is fine -- there are typically
	// 1-3 HISTORY entries per .smd file.
	std::vector<int64_t> samplesI;
	std::vector<float>   samplesF;
	std::vector<double>  samplesD;
};

// ---- Graph-condition list (GRAPH_CONDITIONS{{cond, line, fill}, ...}) ----
struct GraphCondParsed {
	std::string condSource;   // tinyexpr source; references `x`
	std::string lineSource;
	std::string fillSource;

	// Per-Evaluate state: compiled expressions plus a backing scratch
	// double for `x` so the host can ask "does this fire?".
	te_expr* condCompiled = nullptr;
	te_expr* lineCompiled = nullptr;
	te_expr* fillCompiled = nullptr;
	double   xScratch     = 0.0;
};

struct GraphCondDecl {
	std::vector<GraphCondParsed> rows;
};

struct AstNode {
	NodeKind kind;

	// ---- If ----
	std::string             condSource;       // raw, post-preprocess
	std::vector<AstNode>    thenBranch;
	std::vector<AstNode>    elseBranch;

	// ---- For ----
	// `#for $<loopVarName> in $<listName> ... #endfor`
	// listName must reference a ConfigKind::List in m_impl->config. The
	// classifier pass looks up the list and remembers its elemType so the
	// evaluator can pick the right list field (ints / doubles / strings).
	// body shares the same vector<AstNode> shape as If's then/else branches.
	std::string             loopVarName;
	std::string             listName;
	std::vector<AstNode>    body;
	// Resolved during the classify pass from m_impl->config[listName].listVal.
	ListElemType            loopVarType = ListElemType::Int;

	// ---- Text & GetDimensions ----
	CExpr       eX, eY, eFontSize, eColor;    // Text uses all four
	bool        textIsLiteral = false;        // last arg is "..." (true)
											  // or config key (false).
											  // Ignored when textIsInlineFormat
											  // is set.
	std::string textStrOrKey;
	// When the TEXT or GET_DIMENSIONS last arg is itself an inline format
	// (e.g. `TEXT{..., {"%d", x}}`), the parser stores it here as a single-
	// segment StringExpr whose nestedFmt is filled by the classify pass via
	// UpgradeNestedFormatSegments. Evaluation routes through EvalStringExpr
	// when this flag is set, bypassing MaterializeText's name-lookup path.
	bool        textIsInlineFormat = false;
	StringExpr  textInlineExpr;

	// ---- Box / EmptyBox (5-arg: x, y, w, h, color; reuses eX/eY) ----
	CExpr       eWidth, eHeight, eColorBox;

	// ---- RoundedBox / (9-arg: x, y, w, h, roundenssTl, roundnessTr, roundnessBl, roundnessBr, color; reuses eX/eY/eWidth/eHeight/eColorBox) ----
	CExpr       eRoundnessTL,  eRoundnessTR,  eRoundnessBL,  eRoundnessBR;

	// ---- DashedLine (x, y, lineLength, dashOn, dashOff, colour) ----
	CExpr       eX2, eY2, eDashOn, eDashOff;

	// ---- HistoryUpdate (target name, value) / HistoryClean (target only) ----
	std::string historyTarget;
	CExpr       eHistoryValue;

	// ---- GraphLineChart ----
	// x, y, width, height, min, max, direction (compile-time enum),
	// line color, fill color, conditions (config key), history (config key).
	CExpr            eMin, eMax;
	CExpr            eLineColor;     // separate from eColor / eColorBox to
									 // keep the AST clearer
	CExpr            eFillColor;
	GraphDirection   graphDir = GraphDirection::LeftToRight;
	std::string      graphCondsName;
	std::string      graphHistoryName;

	// ---- Var ----
	std::string varName;
	bool        varIsString = false;          // true: stringRhs is the value
											  // false: eVarValue (numeric)
	CExpr       eVarValue;                    // numeric mode (non-ternary)
	StringExpr  stringRhs;                    // string mode

	// Numeric VAR ternary: when set, eVarValue is unused; instead we
	// evaluate varCondSource (through EvalCondition, to allow comparisons)
	// and pick eVarThen or eVarElse.
	bool        varIsTernary = false;
	std::string varCondSource;
	CExpr       eVarThen;
	CExpr       eVarElse;

	// Struct-copy VAR: VAR{Dst, Src[N]} captures three uint16-projected
	// doubles (width/height/calls) from the source resolution-array slot
	// into private scratch doubles named <Dst>_width / <Dst>_height /
	// <Dst>_calls. The classifier sets this flag and stashes the mangled
	// source names below; the evaluator does the 3-field copy.
	bool        varIsStruct = false;
	std::string varSrcWidthName;
	std::string varSrcHeightName;
	std::string varSrcCallsName;

	// ---- GetDimensions ----
	std::string dimsName;
	// Reuses eFontSize / textIsLiteral / textStrOrKey from above.
};

// ===========================================================================
// Document::Impl
// ===========================================================================

// Source of a runtime variable bound by the host. The host pointer is
// dereferenced into m_scratch every frame so tinyexpr can read a stable
// `double*`.
enum class BindType : uint8_t { Int64, Float, Double, Bool };

struct HostBinding {
	BindType    type;
	const void* src;     // host pointer (read-only from our side)
	double      scratch; // refreshed each Evaluate()
};

// Host-bound string. Read directly from *src whenever a %s in a format
// spec references this binding -- no scratch buffer needed.
struct HostStringBinding {
	const std::string* src;
};

// Resolution-array binding: 8 entries of {width, height, calls}. The
// host gives us a pointer to its array and we project the 24 uint16
// fields into per-frame scratch doubles named
//   <base>_<idx>_width / <base>_<idx>_height / <base>_<idx>_calls
// so tinyexpr can resolve them as ordinary identifiers.
struct HostResolutionArrayBinding {
	const ResolutionEntry* src;
	// 24 scratch doubles (8 entries x 3 fields), refreshed each Evaluate().
	// Stored as a fixed-size inline array so pointers are stable across
	// unordered_map rehashes.
	double scratch[8 * 3];
};

// -----------------------------------------------------------------------------
// Predefined names (hardcoded into the parser, per Marek's spec)
// -----------------------------------------------------------------------------
//
// These three lists drive: (1) the read-only check enforced at VAR
// classification time -- VAR writes to a read-only name produce an error;
// (2) seeding the System_Key_* bitmask constants into the te_variable
// table; (3) seeding the read/write config defaults so files can omit
// them and still get sensible values.

// Names the file is NOT permitted to write to via VAR. Anything supplied
// by the host as live telemetry, plus the System_Key_* bitmask constants
// and the resolution-array index/field names.
static bool IsPredefinedReadOnlyName(const std::string& n) {
	static const char* kFixed[] = {
		// CPU
		"CPU_Hz_int", "CPU_RealHz_int", "CPU_DeltaHz_int",
		"CPU_Core0Load_double", "CPU_Core1Load_double",
		"CPU_Core2Load_double", "CPU_Core3Load_double",
		// GPU
		"GPU_Hz_int", "GPU_RealHz_int", "GPU_DeltaHz_int", "GPU_Load_int",
		// RAM
		"RAM_Hz_int", "RAM_RealHz_int", "RAM_DeltaHz_int",
		"RAM_LoadAll_int", "RAM_LoadCPU_int",
		"RAM_UsedAllMB_float", "RAM_TotalAllMB_float",
		"RAM_UsedApplicationMB_float", "RAM_TotalApplicationMB_float",
		"RAM_UsedAppletMB_float", "RAM_TotalAppletMB_float",
		"RAM_UsedSystemMB_float", "RAM_TotalSystemMB_float",
		"RAM_UsedSystemUnsafeMB_float", "RAM_TotalSystemUnsafeMB_float",
		// Board
		"Board_ChargerCurrentLimit_int", "Board_ChargerVoltageLimit_int",
		"Board_ChargerConnected_int",
		"Board_BatteryCurrentAvg_float", "Board_BatteryVoltageAvg_float",
		"Board_IsBatteryFiltered",
		"Board_BatteryAgePercentage_float", "Board_BatteryChargePercentage_float",
		"Board_BatteryTemperatureCelcius_float",
		"Board_DesignedFullBatteryCapacity_float",
		"Board_ActualFullBatteryCapacity_float",
		"Board_PowerConsumption_float",
		"Board_BatteryTimeEstimateInMinutes_int",
		"Board_SocTemperatureCelsius_float", "Board_PcbTemperatureCelsius_float",
		"Board_SkinTemperatureMiliCelsius_int",
		"Board_FanRotationPercentageLevel_float",
		// Game
		"Game_LastFrameNumber_int", "Game_IsGameRunning",
		"Game_FPS_int", "Game_FpsAvgOld_float", "Game_FpsAvg_float",
		"Game_ReadSpeedPerSecond_float",
		"Game_ResolutionRenderCalls_int", "Game_ResolutionViewportCalls_int",
		// System
		"System_DisplayRefreshRate_int", "System_IsDocked",
		"System_KeysDown_int", "System_KeysHeld_int", "formattedKeyCombo",
		// Misc
		"Misc_IsWiFiPassphrase",
		"Misc_NvDecHz_int", "Misc_NvEncHz_int", "Misc_NvJpgHz_int",
		"Misc_NetworkConnectionType_int", "Misc_WiFiPassphrase_str",
		nullptr,
	};
	for (const char** p = kFixed; *p; ++p) if (n == *p) return true;
	// System_Key_* constants
	if (n.size() > 11 && std::memcmp(n.data(), "System_Key_", 11) == 0)
		return true;
	// Mangled resolution-array slots: Game_ResolutionRenderCalls_int_<N>_<field>
	// / Game_ResolutionViewportCalls_int_<N>_<field>
	static const char* kArrPrefixes[] = {
		"Game_ResolutionRenderCalls_int_",
		"Game_ResolutionViewportCalls_int_",
		nullptr,
	};
	for (const char** p = kArrPrefixes; *p; ++p) {
		size_t pl = std::strlen(*p);
		if (n.size() > pl && std::memcmp(n.data(), *p, pl) == 0) return true;
	}
	return false;
}

// System_Key_* bitmask constants. Bit values match libnx HidNpadButton.
// (Compiled lazily at registration time.)
struct PredefinedConst { const char* name; int64_t value; };
static const PredefinedConst kSystemKeys[] = {
	{ "System_Key_A",       1LL << 0  },
	{ "System_Key_B",       1LL << 1  },
	{ "System_Key_X",       1LL << 2  },
	{ "System_Key_Y",       1LL << 3  },
	{ "System_Key_StickL",  1LL << 4  },
	{ "System_Key_StickR",  1LL << 5  },
	{ "System_Key_L",       1LL << 6  },
	{ "System_Key_R",       1LL << 7  },
	{ "System_Key_ZL",      1LL << 8  },
	{ "System_Key_ZR",      1LL << 9  },
	{ "System_Key_Plus",    1LL << 10 },
	{ "System_Key_Minus",   1LL << 11 },
	{ "System_Key_Left",    1LL << 12 },
	{ "System_Key_Up",      1LL << 13 },
	{ "System_Key_Right",   1LL << 14 },
	{ "System_Key_Down",    1LL << 15 }
};
static constexpr size_t kSystemKeysCount = sizeof(kSystemKeys) / sizeof(kSystemKeys[0]);

// Read/write config defaults. These are seeded into the config map on
// every Load (before parsing the file's lines, so the file overrides
// them) and re-seeded after Free() so a freed Document still answers
// GetConfigInt("LayerWidth") with 448, etc.
struct PredefinedConfigDefault {
	const char* name;
	ConfigKind  kind;
	int64_t     intVal;        // for Integer / Bool
	const char* stringVal;     // for String (or nullptr)
};
static const PredefinedConfigDefault kConfigDefaults[] = {
	{ "COMMON_MARGIN",       ConfigKind::Integer, 20,      nullptr },
	{ "BackgroundColor",     ConfigKind::Integer, 0xD000,  nullptr },
	{ "ComboButtonFooter",   ConfigKind::String,  0,       "\xEE\x83\xA1  Back     \xEE\x83\xA0  OK" },
	{ "Movable",             ConfigKind::Bool,    0,       nullptr },
	{ "User_RefreshRate",    ConfigKind::Integer, 60,      nullptr },
	{ "EnableGame",          ConfigKind::Bool,    0,       nullptr },
	{ "EnableCPU",           ConfigKind::Bool,    0,       nullptr },
	{ "EnableGPU",           ConfigKind::Bool,    0,       nullptr },
	{ "EnableRAM",           ConfigKind::Bool,    0,       nullptr },
	{ "EnableBoard",         ConfigKind::Bool,    0,       nullptr },
	{ "EnableMisc",          ConfigKind::Bool,    0,       nullptr },
	{ "LayerWidth",          ConfigKind::Integer, 448,     nullptr },
	{ "LayerHeight",         ConfigKind::Integer, 720,     nullptr },
	{ "LayerPos_x",          ConfigKind::Integer, 0,       nullptr },
	{ "LayerPos_y",          ConfigKind::Integer, 0,       nullptr },
	{ "HeaderText",          ConfigKind::Bool,    1,       nullptr },
	{ "FooterText",          ConfigKind::Bool,    1,       nullptr },
	{ "UseCustomExitCombo",  ConfigKind::Bool,    0,       nullptr },
	{ "EnableControls",      ConfigKind::Bool,    1,       nullptr },
};
static constexpr size_t kConfigDefaultsCount =
	sizeof(kConfigDefaults) / sizeof(kConfigDefaults[0]);

struct Document::Impl {
	// Parsed config (key -> ConfigValue).
	std::unordered_map<std::string, ConfigValue> config;

	// Render script (top-level).
	std::vector<AstNode> script;

	// ---- Bindings ----
	// Stable storage for HostBinding entries. We rely on std::unordered_map's
	// pointer stability for values across insertions (which it provides --
	// only iterators are invalidated, references/pointers to values are not).
	std::unordered_map<std::string, HostBinding> hostBinds;

	// Host-bound strings, used to resolve %s placeholders in format specs.
	std::unordered_map<std::string, HostStringBinding> hostStringBinds;

	// Host-bound resolution arrays. unique_ptr keeps scratch addresses
	// stable across map rehashes (te_variable holds raw double*).
	std::unordered_map<std::string, std::unique_ptr<HostResolutionArrayBinding>> hostResArrays;

	// Private VARs created by VAR{name, expr}. Stable address per name.
	// Allocated on demand during parse so tinyexpr can bind to &varStore[name].
	std::unordered_map<std::string, std::unique_ptr<double>> privateVars;

	// String-valued private VARs (those whose RHS evaluates to a string,
	// not a number). Disjoint from privateVars -- a name appears in at
	// most one of the two. Storage is std::string rather than a double.
	std::unordered_map<std::string, std::string> stringVars;

	// GET_DIMENSIONS scratch structs. Exposed to host through outDims.
	// Each Dimensions also has two doubles tracking .x and .y for tinyexpr.
	struct DimsEntry {
		std::unique_ptr<Dimensions> dims;
		std::unique_ptr<double>     xMirror;  // == dims->x as double
		std::unique_ptr<double>     yMirror;  // == dims->y as double
	};
	std::unordered_map<std::string, DimsEntry> dimsVars;

	// Memoised GET_DIMENSIONS results, keyed by (text + 0x01 + fontSize).
	// Survives across Evaluate() calls so unchanged text doesn't re-fire
	// the host's measurement callback every frame. Cleared on Free() or
	// explicit ClearDimsMeasureCache().
	std::unordered_map<std::string, Dimensions> dimsMeasureCache;

	// Names bound to zero because they appeared in expressions but no
	// host or private var matches. Spec: "If there is no such thing,
	// treat it as bool (== 0 is false)".
	std::unordered_map<std::string, std::unique_ptr<double>> implicitZero;

	// Pool of ConfigValue Integer/Bool mirrors so config keys can also be
	// referenced from expressions (e.g. COMMON_MARGIN).
	std::unordered_map<std::string, std::unique_ptr<double>> configMirror;

	// Snapshot of every te_variable handed to te_compile -- so we can
	// refresh values before evaluation and free the array on Free().
	std::vector<te_variable> teVarTable;

	// Track every te_expr* we own for Free().
	std::vector<te_expr*> ownedExprs;

	// Persistent cache of te_expr trees compiled on-demand by EvalCondition
	// for ad-hoc arithmetic chunks inside conditions. Keyed by source
	// string; the te_expr*s themselves are also pushed into ownedExprs so
	// Free() releases them. Persisting this across frames is essential --
	// a fresh per-frame map would push a NEW te_expr into ownedExprs every
	// frame for every condition, leaking memory at a steady rate.
	std::unordered_map<std::string, te_expr*> arithCache;

	// Owned FormatArg trees -- referenced by FormatSpec::argTernaries
	// (when the entry is a ternary -- otherwise the simpler scalar shape
	// is used). All allocated lazily during the parse / classify pass.
	std::vector<std::unique_ptr<FormatArg>>  ownedFormatArgs;
	std::vector<std::unique_ptr<FormatSpec>> ownedNestedFormats;

	// HISTORY{type, capacity} buffers, keyed by the config name.
	std::unordered_map<std::string, HistoryDecl> histories;

	// Float-history -> double scratch buffer, keyed by history name.
	// GraphLineChart hands the host a uniform `const double*` view for
	// both Float and Double histories; for Float we promote each sample
	// into the matching scratch here on the way out. The scratch is
	// resized in place each emit (push_back+pop_back churn is avoided by
	// assign() over the whole source vector) so this is one allocation
	// amortised across the history's lifetime, sized to capacity.
	std::unordered_map<std::string, std::vector<double>> floatSampleScratch;

	// GRAPH_CONDITIONS lists, keyed by the config name.
	std::unordered_map<std::string, GraphCondDecl> graphConds;

	std::string lastError;
	// Word-wrapped copy of lastError, computed lazily by LastError() so that
	// every assignment site can keep writing the natural-language message
	// without thinking about the on-screen layout. Re-derived whenever
	// lastError changes (we just compare lengths + first chars cheaply).
	mutable std::string lastErrorWrapped;
	mutable size_t      lastErrorWrappedOf = 0;  // size of lastError when wrapped

	uint32_t crc32;

	// ---- IETF locale support ----
	// RecordCallback fires once per Document (on the first IETF{} declaration
	// in the config section) to ask the host which locale key it wants.
	// The result is cached for the Document's lifetime.
	// ietfRecordCb / ietfRecordUser survive Free() so the host only sets
	// them once; ietfLocaleCached / ietfLocaleCode are reset by Free() so a
	// reload re-fires the callback.
	Document::RecordCallback ietfRecordCb   = nullptr;
	void*                    ietfRecordUser = nullptr;
	std::string              ietfLocaleCode;
	bool                     ietfLocaleCached = false;

	// Set by Reset(/*freeze=*/true). When true, Evaluate() skips
	// RefreshScratches() so every host-bound value and every cached string
	// VAR stays exactly as it was on the previous frame. Cleared at the
	// start of Evaluate() after the skip decision is made, so it is
	// automatically a one-shot: the NEXT Reset()/Evaluate() cycle behaves
	// normally unless the caller passes freeze=true again.
	bool m_frozen = false;
};

// ===========================================================================
// Small string helpers
// ===========================================================================

static std::string Trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && std::isspace((unsigned char)s[a])) ++a;
	while (b > a && std::isspace((unsigned char)s[b-1])) --b;
	return s.substr(a, b - a);
}

static bool StartsWith(const std::string& s, const char* p) {
	size_t n = std::strlen(p);
	return s.size() >= n && std::memcmp(s.data(), p, n) == 0;
}

// Strip a line-comment starting with ';', but ignore ';' inside "..." strings.
static std::string StripLineComment(const std::string& line) {
	bool inStr = false;
	for (size_t i = 0; i < line.size(); ++i) {
		char c = line[i];
		if (c == '"' && (i == 0 || line[i-1] != '\\')) inStr = !inStr;
		else if (c == ';' && !inStr) return line.substr(0, i);
	}
	return line;
}

// Hex helper for \uXXXX decoding.
static int HexNibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Process C-style escapes inside a string literal body (no surrounding "").
// Handles \n \r \t \\ \" \0 and \uXXXX (UTF-8 encoded).
static std::string Unescape(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c != '\\' || i + 1 >= s.size()) { out.push_back(c); continue; }
		char e = s[++i];
		switch (e) {
			case 'n':  out.push_back('\n'); break;
			case 'r':  out.push_back('\r'); break;
			case 't':  out.push_back('\t'); break;
			case '0':  out.push_back('\0'); break;
			case '\\': out.push_back('\\'); break;
			case '"':  out.push_back('"');  break;
			case '\'': out.push_back('\''); break;
			case 'u': {
				if (i + 4 >= s.size()) { out.push_back('\\'); out.push_back('u'); break; }
				int h1 = HexNibble(s[i+1]), h2 = HexNibble(s[i+2]),
					h3 = HexNibble(s[i+3]), h4 = HexNibble(s[i+4]);
				if ((h1|h2|h3|h4) < 0) { out.push_back('\\'); out.push_back('u'); break; }
				uint32_t cp = (h1<<12) | (h2<<8) | (h3<<4) | h4;
				i += 4;
				// Encode as UTF-8
				if (cp < 0x80) {
					out.push_back((char)cp);
				} else if (cp < 0x800) {
					out.push_back((char)(0xC0 | (cp >> 6)));
					out.push_back((char)(0x80 | (cp & 0x3F)));
				} else {
					out.push_back((char)(0xE0 | (cp >> 12)));
					out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
					out.push_back((char)(0x80 | (cp & 0x3F)));
				}
				break;
			}
			default: out.push_back('\\'); out.push_back(e); break;
		}
	}
	return out;
}

// Re-escape a plain string so it can be embedded as a "..." literal in an
// .smd source line. The inverse of Unescape: control characters that would
// otherwise confuse the parser are turned back into their backslash forms.
// Used by the IETF pre-pass to bake resolved IETF strings into the source
// lines that the main parser then reads normally.
static std::string RescapeForLiteral(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 4);
	for (unsigned char c : s) {
		switch (c) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n";  break;
			case '\r': out += "\\r";  break;
			case '\t': out += "\\t";  break;
			case '\0': out += "\\0";  break;
			default:   out.push_back((char)c); break;
		}
	}
	return out;
}

// Split a comma-separated argument list, respecting nesting in (), [], {}
// and quoted strings. Returns trimmed items.
static std::vector<std::string> SplitTopLevelCommas(const std::string& s) {
	std::vector<std::string> out;
	int depth = 0;
	bool inStr = false;
	size_t start = 0;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (inStr) {
			if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; continue; }
		if (c == '(' || c == '[' || c == '{') ++depth;
		else if (c == ')' || c == ']' || c == '}') --depth;
		else if (c == ',' && depth == 0) {
			out.push_back(Trim(s.substr(start, i - start)));
			start = i + 1;
		}
	}
	out.push_back(Trim(s.substr(start)));
	return out;
}

// Split a top-level C-style ternary `cond ? a : b` into three parts, with
// `cond`, `a`, `b` each containing the surrounding whitespace/parens stripped
// to the caller's taste. Returns false if no top-level `?` is present.
//
// Right-associative: `a ? b : c ? d : e` parses as `a ? b : (c ? d : e)`,
// matching C. Internally we find the FIRST top-level `?`, then walk forward
// counting nested `?`/`:` to find the matching `:`. Quotes and brackets are
// respected.
static bool SplitTopLevelTernary(const std::string& s,
								 std::string& cond,
								 std::string& thenPart,
								 std::string& elsePart) {
	int  depth = 0;
	bool inStr = false;
	intptr_t qpos = -1;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (inStr) {
			if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; continue; }
		if (c == '(' || c == '[' || c == '{') ++depth;
		else if (c == ')' || c == ']' || c == '}') --depth;
		else if (c == '?' && depth == 0) { qpos = (intptr_t)i; break; }
	}
	if (qpos < 0) return false;

	// Find the matching `:` -- skipping past any nested ternaries that
	// appear inside the then-branch.
	int qDepth = 1;
	intptr_t cpos = -1;
	inStr = false;
	depth = 0;
	for (size_t i = (size_t)qpos + 1; i < s.size(); ++i) {
		char c = s[i];
		if (inStr) {
			if (c == '\\' && i + 1 < s.size()) { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; continue; }
		if (c == '(' || c == '[' || c == '{') { ++depth; continue; }
		if (c == ')' || c == ']' || c == '}') { --depth; continue; }
		if (depth) continue;
		if (c == '?') { ++qDepth; continue; }
		if (c == ':') {
			if (--qDepth == 0) { cpos = (intptr_t)i; break; }
		}
	}
	if (cpos < 0) return false;
	cond     = Trim(s.substr(0, (size_t)qpos));
	thenPart = Trim(s.substr((size_t)qpos + 1, (size_t)(cpos - qpos - 1)));
	elsePart = Trim(s.substr((size_t)cpos + 1));
	return true;
}

// Preprocess an expression: strip leading $, replace '.' inside identifiers
// with '_' (so `dimensions.x` becomes `dimensions_x`).
static std::string PreprocessExpr(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	bool inStr = false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (inStr) {
			out.push_back(c);
			if (c == '\\' && i + 1 < s.size()) { out.push_back(s[++i]); continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; out.push_back(c); continue; }
		if (c == '$') continue;            // drop variable marker
		// Bare `true` / `false` as numeric literals. The config parser
		// recognises these for `key: true` / `key: false`, so authors
		// reasonably expect `VAR{flag, true}` and `#if $cond == true`
		// to work too. tinyexpr knows nothing about them; replace at
		// identifier boundaries with 1 / 0. We require the surrounding
		// characters NOT be identifier-continuation chars so a name like
		// `truesz` doesn't get mangled.
		auto isIdent = [](char ch) {
			return std::isalnum((unsigned char)ch) || ch == '_';
		};
		if ((c == 't' || c == 'f') && (i == 0 || !isIdent(s[i-1]))) {
			if (c == 't' && i + 4 <= s.size()
				&& s[i+1] == 'r' && s[i+2] == 'u' && s[i+3] == 'e'
				&& (i + 4 == s.size() || !isIdent(s[i+4]))) {
				out.push_back('1'); i += 3; continue;
			}
			if (c == 'f' && i + 5 <= s.size()
				&& s[i+1] == 'a' && s[i+2] == 'l' && s[i+3] == 's' && s[i+4] == 'e'
				&& (i + 5 == s.size() || !isIdent(s[i+5]))) {
				out.push_back('0'); i += 4; continue;
			}
		}
		// COLOR{expr} -> color(expr). Literal-bodied COLOR{0xHHHH} forms
		// were already handled at line-load time; anything we see here is
		// the expression form, mapped onto the `color` tinyexpr builtin
		// (TE_FUNCTION1) that does the nibble swap on a double argument.
		if (c == 'C' && i + 6 <= s.size()
			&& s[i+1] == 'O' && s[i+2] == 'L' && s[i+3] == 'O'
			&& s[i+4] == 'R' && s[i+5] == '{'
			&& (i == 0 || (!std::isalnum((unsigned char)s[i-1]) && s[i-1] != '_'))) {
			out.append("color(");
			i += 5;        // advance to the '{'
			// Rewrite the body by walking until matching '}', swapping
			// braces for parens. We recurse one character at a time so
			// nested '.' / '$' / array-index rewriting still applies.
			int depth = 0;
			bool sIn = false;
			for (size_t j = i; j < s.size(); ++j) {
				char cc = s[j];
				if (sIn) {
					out.push_back(cc);
					if (cc == '\\' && j + 1 < s.size()) { out.push_back(s[++j]); continue; }
					if (cc == '"') sIn = false;
					continue;
				}
				if (cc == '"') { sIn = true; out.push_back(cc); continue; }
				if (cc == '{') {
					if (depth == 0) { ++depth; continue; }   // consume the opening brace
					++depth; out.push_back(cc); continue;
				}
				if (cc == '}') {
					if (depth == 1) { out.push_back(')'); i = j; break; }
					--depth; out.push_back(cc); continue;
				}
				// Identifier/dot handling: replicate the dot-collapse rule
				// from the outer loop so dotted names work inside COLOR{}.
				if (cc == '$') continue;
				if (cc == '.') {
					bool L = !out.empty()
						  && (std::isalnum((unsigned char)out.back()) || out.back() == '_');
					bool R = j + 1 < s.size()
						  && (std::isalpha((unsigned char)s[j+1]) || s[j+1] == '_');
					out.push_back((L && R) ? '_' : '.');
					continue;
				}
				out.push_back(cc);
			}
			continue;
		}
		// Array index: <ident>[<digits>] -> <ident>_<digits>.
		// Triggered only when '[' follows an identifier char and the body
		// is digits-then-']'. Anything else (e.g. a hypothetical future use
		// of [] for something non-index) passes through untouched.
		if (c == '[' && !out.empty()
			&& (std::isalnum((unsigned char)out.back()) || out.back() == '_')) {
			size_t j = i + 1;
			while (j < s.size() && std::isdigit((unsigned char)s[j])) ++j;
			if (j > i + 1 && j < s.size() && s[j] == ']') {
				out.push_back('_');
				out.append(s, i + 1, j - i - 1);
				i = j;  // consume up to-and-including ']'
				continue;
			}
			out.push_back(c);
			continue;
		}
		if (c == '.') {
			// Only collapse if both sides are identifier chars.
			bool L = !out.empty()
				  && (std::isalnum((unsigned char)out.back()) || out.back() == '_');
			bool R = i + 1 < s.size()
				  && (std::isalpha((unsigned char)s[i+1]) || s[i+1] == '_');
			out.push_back((L && R) ? '_' : '.');
			continue;
		}
		out.push_back(c);
	}
	return out;
}

// Find every identifier-looking token in an expression. Used to bind
// implicit zeros for unbound names so te_compile doesn't fail.
static void ScanIdentifiers(const std::string& expr, std::vector<std::string>& out) {
	size_t i = 0, n = expr.size();
	bool inStr = false;
	while (i < n) {
		char c = expr[i];
		if (inStr) {
			if (c == '\\' && i + 1 < n) { i += 2; continue; }
			if (c == '"') inStr = false;
			++i; continue;
		}
		if (c == '"') { inStr = true; ++i; continue; }
		if (std::isalpha((unsigned char)c) || c == '_') {
			size_t s = i;
			while (i < n && (std::isalnum((unsigned char)expr[i]) || expr[i] == '_')) ++i;
			// Function call -> not a variable (tinyexpr resolves it itself).
			size_t j = i;
			while (j < n && std::isspace((unsigned char)expr[j])) ++j;
			if (j < n && expr[j] == '(') continue;
			out.push_back(expr.substr(s, i - s));
			continue;
		}
		++i;
	}
}

// Walk a printf-style format string and emit, in order, one bool per
// positional argument: true iff that argument is consumed by a %s/%S
// conversion. %% is escaped and consumes no argument; unknown conversions
// are assumed to consume one (numeric) argument.
static void ScanFormatArgKinds(const std::string& fmt, std::vector<bool>& out) {
	for (size_t i = 0; i < fmt.size(); ) {
		if (fmt[i] != '%') { ++i; continue; }
		++i;
		if (i < fmt.size() && fmt[i] == '%') { ++i; continue; }   // %%
		// flags
		while (i < fmt.size()
			&& (fmt[i]=='-'||fmt[i]=='+'||fmt[i]==' '
				||fmt[i]=='#'||fmt[i]=='0')) ++i;
		// width
		while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
		// precision
		if (i < fmt.size() && fmt[i] == '.') {
			++i;
			while (i < fmt.size() && std::isdigit((unsigned char)fmt[i])) ++i;
		}
		// length modifiers
		while (i < fmt.size()
			&& (fmt[i]=='h'||fmt[i]=='l'||fmt[i]=='z'
				||fmt[i]=='j'||fmt[i]=='t'||fmt[i]=='L')) ++i;
		if (i >= fmt.size()) return;
		char conv = fmt[i++];
		out.push_back(conv == 's' || conv == 'S');
	}
}

// ===========================================================================
// Parser
// ===========================================================================

// Parse a value string (right-hand side of "key: <value>") into ConfigValue.
static bool ParseConfigValue(const std::string& raw, ConfigValue& out,
							 std::string& err) {
	std::string v = Trim(raw);
	if (v.empty()) {
		err = "empty value";
		return false;
	}

	// HISTORY{int, capacity}
	if (v.size() > 7 && v.compare(0, 7, "HISTORY") == 0
		&& (v[7] == '{' || v[7] == ' ' || v[7] == '\t')) {
		size_t lb = v.find('{');
		size_t rb = v.rfind('}');
		if (lb == std::string::npos || rb == std::string::npos || rb < lb) {
			err = "HISTORY: malformed braces"; return false;
		}
		std::string body = Trim(v.substr(lb + 1, rb - lb - 1));
		std::vector<std::string> parts = SplitTopLevelCommas(body);
		if (parts.size() != 2) {
			err = "HISTORY: expected {type, capacity}"; return false;
		}
		std::string ty = Trim(parts[0]);
		char* endp = nullptr;
		long long cap = std::strtoll(parts[1].c_str(), &endp, 10);
		if (endp == parts[1].c_str() || cap <= 0) {
			err = "HISTORY: bad capacity '" + parts[1] + "'"; return false;
		}
		if (ty != "int" && ty != "float" && ty != "double") {
			err = "HISTORY: unsupported type '" + ty
				+ "' (expected int, float, or double)";
			return false;
		}
		out.kind   = ConfigKind::History;
		out.intVal = cap;   // remembered so post-parse can allocate
		// Note: we use stringVal to stash the type name. Lazy but works.
		out.stringVal = ty;
		return true;
	}

	// GRAPH_CONDITIONS{ {cond, lineCol, fillCol}, ... }
	if (v.size() > 16 && v.compare(0, 16, "GRAPH_CONDITIONS") == 0
		&& (v[16] == '{' || v[16] == ' ' || v[16] == '\t')) {
		size_t lb = v.find('{');
		size_t rb = v.rfind('}');
		if (lb == std::string::npos || rb == std::string::npos || rb < lb) {
			err = "GRAPH_CONDITIONS: malformed braces"; return false;
		}
		std::string body = Trim(v.substr(lb + 1, rb - lb - 1));
		out.kind = ConfigKind::GraphConditions;
		// Store body so the post-parse pass can split it into rows.
		// (We can't allocate GraphCondDecl here -- Impl isn't visible.)
		out.stringVal = body;
		return true;
	}

	// LIST{type, {elem, elem, ...}}
	//   type: int | float | double | str
	//   element form: {elem1, elem2, ...} or RANGE{start, end, step}
	// RANGE only valid for int-typed lists; produces a Python-like sequence
	// (start, start+step, ..., < end if step > 0; > end if step < 0).
	if (v.size() > 4 && v.compare(0, 4, "LIST") == 0
		&& (v[4] == '{' || v[4] == ' ' || v[4] == '\t')) {
		size_t lb = v.find('{');
		size_t rb = v.rfind('}');
		if (lb == std::string::npos || rb == std::string::npos || rb < lb) {
			err = "LIST: malformed braces"; return false;
		}
		std::string body = Trim(v.substr(lb + 1, rb - lb - 1));
		// Top-level split: first part is type, rest is the element block
		// (which itself starts with `{` or with `RANGE{...}`).
		// We can't use SplitTopLevelCommas naively because the element
		// block may contain its own commas. Find the FIRST top-level comma.
		size_t commaPos = std::string::npos;
		{
			int depth = 0;
			bool inStr = false;
			for (size_t i = 0; i < body.size(); ++i) {
				char c = body[i];
				if (inStr) {
					if (c == '\\' && i + 1 < body.size()) { ++i; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{' || c == '(' || c == '[') ++depth;
				else if (c == '}' || c == ')' || c == ']') --depth;
				else if (c == ',' && depth == 0) { commaPos = i; break; }
			}
		}
		if (commaPos == std::string::npos) {
			err = "LIST: expected {type, elements}"; return false;
		}
		std::string ty = Trim(body.substr(0, commaPos));
		std::string elems = Trim(body.substr(commaPos + 1));

		ListElemType etype;
		if      (ty == "int")    etype = ListElemType::Int;
		else if (ty == "float")  etype = ListElemType::Float;
		else if (ty == "double") etype = ListElemType::Double;
		else if (ty == "str")    etype = ListElemType::String;
		else { err = "LIST: unknown type '" + ty + "' (use int/float/double/str)"; return false; }

		out.kind = ConfigKind::List;
		out.listVal.elemType = etype;

		// RANGE form?
		if (elems.size() > 5 && elems.compare(0, 5, "RANGE") == 0
			&& (elems[5] == '{' || elems[5] == ' ' || elems[5] == '\t')) {
			if (etype != ListElemType::Int) {
				err = "LIST: RANGE is only valid for int-typed lists"; return false;
			}
			size_t rlb = elems.find('{');
			size_t rrb = elems.rfind('}');
			if (rlb == std::string::npos || rrb == std::string::npos || rrb < rlb) {
				err = "RANGE: malformed braces"; return false;
			}
			std::string rbody = Trim(elems.substr(rlb + 1, rrb - rlb - 1));
			std::vector<std::string> rparts = SplitTopLevelCommas(rbody);
			if (rparts.size() != 3) {
				err = "RANGE: expected {start, end, step}"; return false;
			}
			char* endp = nullptr;
			long long start = std::strtoll(Trim(rparts[0]).c_str(), &endp, 0);
			if (endp == rparts[0].c_str()) { err = "RANGE: bad start"; return false; }
			long long end_  = std::strtoll(Trim(rparts[1]).c_str(), &endp, 0);
			if (endp == rparts[1].c_str()) { err = "RANGE: bad end"; return false; }
			long long step = std::strtoll(Trim(rparts[2]).c_str(), &endp, 0);
			if (endp == rparts[2].c_str()) { err = "RANGE: bad step"; return false; }
			if (step == 0) { err = "RANGE: step must be non-zero"; return false; }
			// Python-style: keep going while we haven't reached end_.
			// Positive step: while value < end. Negative step: while value > end.
			// Mismatched direction yields an empty list (matches Python).
			if (step > 0) {
				for (long long x = start; x < end_; x += step)
					out.listVal.ints.push_back((int64_t)x);
			} else {
				for (long long x = start; x > end_; x += step)
					out.listVal.ints.push_back((int64_t)x);
			}
			return true;
		}

		// Inline element list: {a, b, c, ...}
		if (elems.empty() || elems.front() != '{' || elems.back() != '}') {
			err = "LIST: expected `{elem, ...}` or RANGE{...} after type"; return false;
		}
		std::string inside = Trim(elems.substr(1, elems.size() - 2));
		if (inside.empty()) return true;  // empty list is OK
		std::vector<std::string> parts = SplitTopLevelCommas(inside);
		for (auto& p : parts) {
			std::string pp = Trim(p);
			if (etype == ListElemType::String) {
				if (pp.size() < 2 || pp.front() != '"' || pp.back() != '"') {
					err = "LIST{str, ...}: element must be a quoted string: " + pp;
					return false;
				}
				out.listVal.strings.push_back(Unescape(pp.substr(1, pp.size() - 2)));
			} else if (etype == ListElemType::Int) {
				char* endp = nullptr;
				long long iv = std::strtoll(pp.c_str(), &endp, 0);
				if (endp == pp.c_str()) {
					err = "LIST{int, ...}: not an integer: " + pp; return false;
				}
				out.listVal.ints.push_back((int64_t)iv);
			} else {
				// float or double -- stored in `doubles`
				char* endp = nullptr;
				double dv = std::strtod(pp.c_str(), &endp);
				if (endp == pp.c_str()) {
					err = "LIST{float/double, ...}: not a number: " + pp; return false;
				}
				out.listVal.doubles.push_back(dv);
			}
		}
		return true;
	}

	// Boolean
	if (v == "true" || v == "false") {
		out.kind = ConfigKind::Bool;
		out.boolVal = (v == "true");
		out.intVal  = out.boolVal ? 1 : 0;
		return true;
	}

	// String literal
	if (v.front() == '"') {
		// Find closing quote (allow escapes)
		size_t i = 1;
		while (i < v.size()) {
			if (v[i] == '\\' && i + 1 < v.size()) { i += 2; continue; }
			if (v[i] == '"') break;
			++i;
		}
		if (i >= v.size()) { err = "unterminated string literal"; return false; }
		out.kind = ConfigKind::String;
		out.stringVal = Unescape(v.substr(1, i - 1));
		return true;
	}

	// Format spec: {"fmt", arg, arg, ...}
	if (v.front() == '{') {
		if (v.back() != '}') { err = "format spec missing closing '}'"; return false; }
		std::string body = v.substr(1, v.size() - 2);
		std::vector<std::string> parts = SplitTopLevelCommas(body);
		if (parts.empty() || parts[0].empty() || parts[0].front() != '"') {
			err = "format spec must start with quoted fmt string";
			return false;
		}
		// Extract the fmt string from parts[0]
		const std::string& p0 = parts[0];
		size_t i = 1;
		while (i < p0.size()) {
			if (p0[i] == '\\' && i + 1 < p0.size()) { i += 2; continue; }
			if (p0[i] == '"') break;
			++i;
		}
		if (i >= p0.size()) { err = "format spec: unterminated fmt"; return false; }
		out.kind = ConfigKind::Format;
		out.fmtVal.fmt = Unescape(p0.substr(1, i - 1));
		ScanFormatArgKinds(out.fmtVal.fmt, out.fmtVal.argIsString);
		// Keep raw arg text -- ternary trees and final preprocessing happen
		// during a separate pass that has access to Impl maps (so it can
		// own the produced FormatArg trees).
		for (size_t k = 1; k < parts.size(); ++k)
			out.fmtVal.argExprs.push_back(parts[k]);
		return true;
	}

	// Integer (hex or decimal, optional leading sign)
	if (v.front() == '-' || v.front() == '+'
		|| (v.front() >= '0' && v.front() <= '9')) {
		char* endp = nullptr;
		int base = 10;
		const char* s = v.c_str();
		if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
			base = 16;
		long long n = std::strtoll(s, &endp, base);
		if (endp == s) { err = "bad integer literal"; return false; }
		out.kind = ConfigKind::Integer;
		out.intVal = (int64_t)n;
		out.boolVal = (n != 0);
		return true;
	}

	// Fallback: bare unquoted text -> treat as a string literal.
	// Useful for entries like "Name: Full" that don't fit any other category.
	out.kind = ConfigKind::String;
	out.stringVal = v;
	return true;
}

// Find the body inside CMD{...} on a single logical line. Returns body and
// updates `endIndex` past the closing '}'.
static bool ExtractBraceBody(const std::string& line, size_t openIdx,
							 std::string& body, size_t& endIdx,
							 std::string& err) {
	int depth = 0;
	bool inStr = false;
	for (size_t i = openIdx; i < line.size(); ++i) {
		char c = line[i];
		if (inStr) {
			if (c == '\\' && i + 1 < line.size()) { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; continue; }
		if (c == '{') ++depth;
		else if (c == '}') {
			--depth;
			if (depth == 0) {
				body = line.substr(openIdx + 1, i - openIdx - 1);
				endIdx = i + 1;
				return true;
			}
		}
	}
	err = "unbalanced braces";
	return false;
}

// Split a string expression on `+` at top level (not inside quotes or
// parens / brackets / braces).
static std::vector<std::string> SplitTopLevelPlus(const std::string& s) {
	std::vector<std::string> out;
	std::string cur;
	int  depth   = 0;
	bool inStr   = false;
	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (inStr) {
			cur.push_back(c);
			if (c == '\\' && i + 1 < s.size()) { cur.push_back(s[++i]); continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"') { inStr = true; cur.push_back(c); continue; }
		if (c == '(' || c == '[' || c == '{') { ++depth; cur.push_back(c); continue; }
		if (c == ')' || c == ']' || c == '}') { --depth; cur.push_back(c); continue; }
		if (c == '+' && depth == 0) {
			out.push_back(cur);
			cur.clear();
			continue;
		}
		cur.push_back(c);
	}
	out.push_back(cur);
	return out;
}

// Parse a VAR RHS that has been classified as a string expression. Splits
// on top-level `+`; each piece is either:
//   - "quoted literal" (escape-decoded into a Literal segment)
//   - a bare identifier (becomes an Identifier segment; `$` prefix stripped)
//   - a `{"fmt", args...}` nested format spec (stored as a raw-source
//     segment with isLiteral=false and a leading `{` in text; the
//     UpgradeNestedFormatSegments pass converts these to true nested
//     FormatSpec pointers once Impl is available)
// Returns false on malformed input.
static bool ParseStringExpr(const std::string& raw, StringExpr& out,
							std::string& err) {
	out.segments.clear();
	auto parts = SplitTopLevelPlus(raw);
	for (auto& rawPart : parts) {
		std::string p = Trim(rawPart);
		if (p.empty()) continue;   // tolerate stray whitespace
		if (p.front() == '{') {
			// Find the matching close brace.
			int depth = 0; bool inStr = false; size_t end = std::string::npos;
			for (size_t i = 0; i < p.size(); ++i) {
				char c = p[i];
				if (inStr) {
					if (c == '\\' && i + 1 < p.size()) { ++i; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{') ++depth;
				else if (c == '}') { if (--depth == 0) { end = i; break; } }
			}
			if (end == std::string::npos) {
				err = "unterminated brace expression: " + raw;
				return false;
			}
			std::string tail = Trim(p.substr(end + 1));
			if (!tail.empty()) {
				err = "unexpected text after brace expression: " + tail;
				return false;
			}
			// ParseStringExpr only handles brace forms that look like a
			// format spec (`{"fmt", arg, ...}`); brace-wrapped expressions
			// like `{cond ? a : b}` are handled at the BuildFormatArg level
			// (which unwraps and recurses with the inner ternary). If we
			// get one here, treat the whole thing as a single nested-fmt
			// segment so the upgrade pass can give a clean error.
			StringExprSegment seg;
			seg.isLiteral = false;
			seg.text = p.substr(0, end + 1);
			out.segments.push_back(std::move(seg));
			continue;
		}
		if (p.front() == '"') {
			// Quoted literal -- find the matching closing quote.
			size_t i = 1;
			while (i < p.size()) {
				if (p[i] == '\\' && i + 1 < p.size()) { i += 2; continue; }
				if (p[i] == '"') break;
				++i;
			}
			if (i >= p.size()) {
				err = "unterminated string literal in string expression: " + raw;
				return false;
			}
			StringExprSegment seg;
			seg.isLiteral = true;
			seg.text = Unescape(p.substr(1, i - 1));
			out.segments.push_back(std::move(seg));
			// anything after the closing quote (other than whitespace) is a
			// user error -- the `+` should have split before it
			std::string tail = Trim(p.substr(i + 1));
			if (!tail.empty()) {
				err = "unexpected text after string literal: " + tail;
				return false;
			}
		} else {
			// Identifier segment. Strip leading `$` if present; `.` was
			// already turned into `_` by PreprocessExpr (which we don't run
			// for string segments, so do it manually here).
			if (p.front() == '$') p.erase(0, 1);
			// Normalise dotted names like dimensions.x -> dimensions_x so
			// numeric coercion can find the mirror double if needed.
			for (char& c : p) if (c == '.') c = '_';
			StringExprSegment seg;
			seg.isLiteral = false;
			seg.text = p;
			out.segments.push_back(std::move(seg));
		}
	}
	return true;
}

// Quick test: does a raw RHS look like it should be parsed as a string?
// Triggers on: any quoted string literal, OR any top-level identifier that
// matches a known string source (config String, config Format, previously
// classified private string VAR). Host string bindings are unknown at parse
// time so they don't influence classification -- if you want a string VAR
// driven by a host string binding alone, prime it with a literal first:
//     VAR{X, ""}     -- forces X to be classified string
//     VAR{X, X + something}
static bool ExprLooksLikeString(
		const std::string& raw,
		const std::unordered_map<std::string, ConfigValue>& config,
		const std::unordered_map<std::string, bool>& priorStringVars)
{
	// A top-level brace form is always a format spec ({"fmt", args...}),
	// which materialises to a string. Catch that before falling into the
	// generic top-level-quote scan -- the quote inside is at depth 1 so
	// the loop below wouldn't recognise it.
	{
		std::string t = Trim(raw);
		if (t.size() >= 2 && t.front() == '{' && t.back() == '}')
			return true;
	}
	// Quick scan for a top-level quote.
	int  depth = 0;
	bool inStr = false;
	for (size_t i = 0; i < raw.size(); ++i) {
		char c = raw[i];
		if (inStr) {
			if (c == '\\' && i + 1 < raw.size()) { ++i; continue; }
			if (c == '"') inStr = false;
			continue;
		}
		if (c == '"' && depth == 0) return true;
		if (c == '"') inStr = true;
		else if (c == '(' || c == '[' || c == '{') ++depth;
		else if (c == ')' || c == ']' || c == '}') --depth;
	}
	// No quoted literal -- look for a string-typed identifier.
	auto parts = SplitTopLevelPlus(raw);
	for (auto& p : parts) {
		std::string s = Trim(p);
		if (s.empty()) continue;
		if (s.front() == '$') s.erase(0, 1);
		// Identifier must consist of [A-Za-z_][A-Za-z0-9_]* only -- numeric
		// tokens and complex expressions are definitely not string.
		bool ident = !s.empty()
					 && (std::isalpha((unsigned char)s.front()) || s.front() == '_');
		if (ident) for (char c : s)
			if (!std::isalnum((unsigned char)c) && c != '_') { ident = false; break; }
		if (!ident) continue;
		auto pit = priorStringVars.find(s);
		if (pit != priorStringVars.end() && pit->second) return true;
		auto cit = config.find(s);
		if (cit != config.end()
			&& (cit->second.kind == ConfigKind::String
			 || cit->second.kind == ConfigKind::Format)) return true;
	}
	return false;
}

// Try to recognise an arg source as a nested format-spec literal -- i.e.
// the source begins with `{"`, ends with `}`, and the second character
// is a quote. Returns the format spec on success, or null.
static std::unique_ptr<FormatSpec> TryParseNestedFormat(const std::string& raw,
													   std::string& err) {
	std::string v = Trim(raw);
	if (v.size() < 4 || v.front() != '{' || v.back() != '}') return nullptr;
	if (v[1] != '"') return nullptr;
	ConfigValue cv;
	if (!ParseConfigValue(v, cv, err)) return nullptr;
	if (cv.kind != ConfigKind::Format) {
		err = "expected nested format spec";
		return nullptr;
	}
	auto out = std::unique_ptr<FormatSpec>(new FormatSpec());
	*out = std::move(cv.fmtVal);
	return out;
}

// Build a FormatArg tree from raw source text. `isString` says whether
// the parent format-spec slot is %s (string) or numeric. The tree is
// owned by `impl.ownedFormatArgs` -- caller passes a non-owning pointer
// out via the return value.
//
// For numeric slots:
//   - leaf:  `expr`              -> Numeric(PreprocessExpr(expr))
//   - tree:  `cond ? a : b`      -> Ternary(cond, BuildArg(a), BuildArg(b))
// For string slots:
//   - leaf:  `"literal"`         -> StringExpr with one literal segment
//   - leaf:  `ident + "x" + ...` -> StringExpr from ParseStringExpr
//   - leaf:  `{"%d", ...}`       -> NestedFormat
//   - leaf:  bare identifier     -> StringExpr with one identifier segment
//   - tree:  `cond ? a : b`      -> Ternary(...)
// Forward decl -- defined immediately after BuildFormatArg; called from
// BuildFormatArg's string-fallback branch.
static bool UpgradeNestedFormatSegments(Document::Impl& impl, StringExpr& se,
										std::string& err);

static FormatArg* BuildFormatArg(Document::Impl& impl,
								 const std::string& raw,
								 bool isString,
								 std::string& err) {
	std::string src = Trim(raw);

	// Brace-wrapped grouping: the file author wraps a non-format-spec
	// expression like `{cond ? "X" : "Y"}` or `{x + y}` in braces to mark
	// it as a single arg. We unwrap exactly one layer when the brace body
	// is NOT a format spec (doesn't start with `"`). Format specs starting
	// with `"` fall through to the existing nested-format handling.
	if (src.size() >= 2 && src.front() == '{' && src.back() == '}') {
		// Confirm the outer braces are matched (no early closure).
		int depth = 0; bool inStr = false; bool fullyEnclosed = true;
		for (size_t i = 0; i < src.size(); ++i) {
			char c = src[i];
			if (inStr) {
				if (c == '\\' && i + 1 < src.size()) { ++i; continue; }
				if (c == '"') inStr = false;
				continue;
			}
			if (c == '"') { inStr = true; continue; }
			if (c == '{') ++depth;
			else if (c == '}') {
				--depth;
				if (depth == 0 && i + 1 < src.size()) { fullyEnclosed = false; break; }
			}
		}
		if (fullyEnclosed && depth == 0) {
			std::string body = Trim(src.substr(1, src.size() - 2));
			if (!body.empty() && body.front() != '"') {
				// Unwrap and recurse with the body.
				return BuildFormatArg(impl, body, isString, err);
			}
		}
	}

	// Top-level ternary?
	std::string cs, ts, es;
	if (SplitTopLevelTernary(src, cs, ts, es)) {
		auto node = std::unique_ptr<FormatArg>(new FormatArg());
		node->kind       = FormatArgKind::Ternary;
		node->condSource = PreprocessExpr(cs);
		node->thenArg = BuildFormatArg(impl, ts, isString, err);
		if (!node->thenArg) return nullptr;
		node->elseArg = BuildFormatArg(impl, es, isString, err);
		if (!node->elseArg) return nullptr;
		FormatArg* raw_ptr = node.get();
		impl.ownedFormatArgs.push_back(std::move(node));
		return raw_ptr;
	}

	auto node = std::unique_ptr<FormatArg>(new FormatArg());

	if (!isString) {
		node->kind          = FormatArgKind::Numeric;
		node->numericSource = PreprocessExpr(src);
		FormatArg* raw_ptr = node.get();
		impl.ownedFormatArgs.push_back(std::move(node));
		return raw_ptr;
	}

	// String slot: try nested format first, then string expression.
	{
		std::string nestedErr;
		if (auto nested = TryParseNestedFormat(src, nestedErr)) {
			// Recursively build the FormatArg tree for the nested format's
			// own positional args -- otherwise the inner args never get
			// compiled (the top-level ResolveFormatArgs only iterates
			// Impl::config).
			FormatSpec& nf = *nested;
			nf.argTernaries.clear();
			nf.argTernaries.reserve(nf.argExprs.size());
			for (size_t i = 0; i < nf.argExprs.size(); ++i) {
				bool nIsStr = i < nf.argIsString.size() && nf.argIsString[i];
				FormatArg* sub = BuildFormatArg(impl, nf.argExprs[i],
												nIsStr, err);
				if (!sub) return nullptr;
				nf.argTernaries.push_back(static_cast<void*>(sub));
			}
			node->kind   = FormatArgKind::NestedFormat;
			node->nested = std::move(nested);
			FormatArg* raw_ptr = node.get();
			impl.ownedFormatArgs.push_back(std::move(node));
			return raw_ptr;
		}
	}
	// Treat as a string expression (literal "..." or ident + ...).
	{
		std::string strErr;
		StringExpr se;
		if (!ParseStringExpr(src, se, strErr)) {
			// Fallback: treat the whole thing as a bare identifier.
			StringExprSegment seg;
			seg.isLiteral = false;
			std::string id = src;
			if (!id.empty() && id.front() == '$') id.erase(0, 1);
			for (char& c : id) if (c == '.') c = '_';
			seg.text = id;
			se.segments.push_back(std::move(seg));
		}
		// Upgrade any `{...}` segments into nested FormatSpecs before
		// freezing the segment list.
		if (!UpgradeNestedFormatSegments(impl, se, err)) return nullptr;
		node->kind    = FormatArgKind::StringExpr_;
		node->strExpr = std::move(se);
		FormatArg* raw_ptr = node.get();
		impl.ownedFormatArgs.push_back(std::move(node));
		return raw_ptr;
	}
}

// Walk a StringExpr and convert any segment whose text is a `{...}` brace
// form into a nested FormatSpec, parsed and owned by Impl. This runs at
// the BuildFormatArg / VAR-classifier call sites where Impl is in scope --
// ParseStringExpr itself can't do it because of the layering.
static bool UpgradeNestedFormatSegments(Document::Impl& impl, StringExpr& se,
										std::string& err) {
	for (auto& seg : se.segments) {
		if (seg.isLiteral || seg.nestedFmt) continue;
		if (seg.text.empty() || seg.text.front() != '{') continue;

		std::string nestedErr;
		auto nested = TryParseNestedFormat(seg.text, nestedErr);
		if (!nested) {
			err = "nested format spec '" + seg.text + "': " + nestedErr;
			return false;
		}
		FormatSpec& nf = *nested;
		nf.argTernaries.clear();
		nf.argTernaries.reserve(nf.argExprs.size());
		for (size_t i = 0; i < nf.argExprs.size(); ++i) {
			bool nIsStr = i < nf.argIsString.size() && nf.argIsString[i];
			FormatArg* sub = BuildFormatArg(impl, nf.argExprs[i], nIsStr, err);
			if (!sub) return false;
			nf.argTernaries.push_back(static_cast<void*>(sub));
		}
		// Park the FormatSpec in Impl's pool so the pointer is stable.
		impl.ownedNestedFormats.push_back(std::move(nested));
		seg.nestedFmt = (void*)impl.ownedNestedFormats.back().get();
		seg.text.clear();   // raw source no longer needed
	}
	return true;
}

// Build trees for every arg in every Format-typed config value. Stash the
// tree pointers in argTernaries (we ALWAYS create one per arg now, since
// it's the cleanest way to handle string vs numeric vs nested vs ternary
// uniformly downstream).
static bool ResolveFormatArgs(Document::Impl& impl, std::string& err) {
	for (auto& kv : impl.config) {
		if (kv.second.kind != ConfigKind::Format) continue;
		FormatSpec& f = kv.second.fmtVal;
		f.argTernaries.clear();
		f.argTernaries.reserve(f.argExprs.size());
		for (size_t i = 0; i < f.argExprs.size(); ++i) {
			bool isStr = i < f.argIsString.size() && f.argIsString[i];
			FormatArg* node = BuildFormatArg(impl, f.argExprs[i], isStr, err);
			if (!node) {
				err = "config '" + kv.first + "' arg " + std::to_string(i)
					+ ": " + err;
				return false;
			}
			f.argTernaries.push_back(static_cast<void*>(node));
		}
	}
	return true;
}

// Parse the right-hand-side string of a TEXT/GETDIMS last argument: either
// a string literal "..." or a bare identifier (config key or string VAR).
// Parse the trailing "text" argument of TEXT / GET_DIMENSIONS. The arg can
// be one of:
//   "..."                       -- quoted literal           (isLit=true)
//   identifier or $identifier   -- name to look up later    (isLit=false)
//   {"fmt", arg, ...}           -- inline format spec       (isInline=true)
// When isInline is true, the parsed StringExpr is written to inlineOut and
// the textStrOrKey/isLit pair is left untouched; the classifier pass will
// later call UpgradeNestedFormatSegments to materialise the nested format.
static bool ParseTextArg(const std::string& s, bool& isLit,
						 std::string& outStrOrKey,
						 bool& isInline, StringExpr& inlineOut,
						 std::string& err) {
	std::string v = Trim(s);
	if (v.empty()) { err = "missing text arg"; return false; }
	isInline = false;
	if (v.front() == '{') {
		// Reuse ParseStringExpr which already produces a single-segment
		// StringExpr for a bare `{...}` form; UpgradeNestedFormatSegments
		// will then process the segment into a proper FormatSpec*.
		if (!ParseStringExpr(v, inlineOut, err)) return false;
		isInline = true;
		// Leave isLit/outStrOrKey defaulted; the inline path ignores them.
		return true;
	}
	if (v.front() == '"') {
		size_t i = 1;
		while (i < v.size()) {
			if (v[i] == '\\' && i + 1 < v.size()) { i += 2; continue; }
			if (v[i] == '"') break;
			++i;
		}
		if (i >= v.size()) { err = "unterminated string in TEXT arg"; return false; }
		isLit = true;
		outStrOrKey = Unescape(v.substr(1, i - 1));
		return true;
	}
	isLit = false;
	// strip optional leading $
	if (!v.empty() && v.front() == '$') v.erase(0, 1);
	outStrOrKey = v;
	return true;
}

// Parse a single render-script command (TEXT/BOX/VAR/GET_DIMENSIONS) given
// its name, body inside braces, and the AstNode to fill.
static bool ParseCommandBody(const std::string& name, const std::string& body,
							 AstNode& out, std::string& err) {
	std::vector<std::string> args = SplitTopLevelCommas(body);

	if (name == "TEXT") {
		if (args.size() != 5) { err = "TEXT expects 5 args"; return false; }
		out.kind = NodeKind::Text;
		out.eX.source        = PreprocessExpr(args[0]);
		out.eY.source        = PreprocessExpr(args[1]);
		out.eFontSize.source = PreprocessExpr(args[2]);
		out.eColor.source    = PreprocessExpr(args[3]);
		return ParseTextArg(args[4], out.textIsLiteral, out.textStrOrKey,
							out.textIsInlineFormat, out.textInlineExpr, err);
	}
	if (name == "BOX") {
		if (args.size() != 5) { err = "BOX expects 5 args\n(x, y, w, h, color)"; return false; }
		out.kind = NodeKind::Box;
		out.eX.source        = PreprocessExpr(args[0]);
		out.eY.source        = PreprocessExpr(args[1]);
		out.eWidth.source    = PreprocessExpr(args[2]);
		out.eHeight.source   = PreprocessExpr(args[3]);
		out.eColorBox.source = PreprocessExpr(args[4]);
		return true;
	}
	if (name == "ROUNDED_BOX") {
		if (args.size() != 9) { err = "ROUNDED_BOX expects 9 args\n(x, y, w, h, roundnessTl, roundnessTr, roundnessBl, roundnessBr, color)"; return false; }
		out.kind = NodeKind::RoundedBox;
		out.eX.source           = PreprocessExpr(args[0]);
		out.eY.source           = PreprocessExpr(args[1]);
		out.eWidth.source       = PreprocessExpr(args[2]);
		out.eHeight.source      = PreprocessExpr(args[3]);
		out.eRoundnessTL.source = PreprocessExpr(args[4]);
		out.eRoundnessTR.source = PreprocessExpr(args[5]);
		out.eRoundnessBL.source = PreprocessExpr(args[6]);
		out.eRoundnessBR.source = PreprocessExpr(args[7]);
		out.eColorBox.source    = PreprocessExpr(args[8]);
		return true;
	}
	if (name == "EMPTY_BOX") {
		if (args.size() != 5) { err = "EMPTY_BOX expects 5 args\n(x, y, w, h, color)"; return false; }
		out.kind = NodeKind::EmptyBox;
		out.eX.source        = PreprocessExpr(args[0]);
		out.eY.source        = PreprocessExpr(args[1]);
		out.eWidth.source    = PreprocessExpr(args[2]);
		out.eHeight.source   = PreprocessExpr(args[3]);
		out.eColorBox.source = PreprocessExpr(args[4]);
		return true;
	}
	if (name == "DASHED_LINE") {
		if (args.size() != 7) {
			err = "DASHED_LINE expects 7 args\n"
				  "(x, y, x2, y2, line_length, empty_space, colour)";
			return false;
		}
		out.kind = NodeKind::DashedLine;
		out.eX.source        = PreprocessExpr(args[0]);
		out.eY.source        = PreprocessExpr(args[1]);
		out.eX2.source       = PreprocessExpr(args[2]);
		out.eY2.source       = PreprocessExpr(args[3]);
		out.eDashOn.source   = PreprocessExpr(args[4]);
		out.eDashOff.source  = PreprocessExpr(args[5]);
		out.eColorBox.source = PreprocessExpr(args[6]);
		return true;
	}
	if (name == "HISTORY_UPDATE") {
		if (args.size() != 2) {
			err = "HISTORY_UPDATE expects 2 args\n(history_name, value)";
			return false;
		}
		out.kind = NodeKind::HistoryUpdate;
		out.historyTarget = Trim(args[0]);
		if (!out.historyTarget.empty() && out.historyTarget[0] == '$')
			out.historyTarget.erase(0, 1);
		out.eHistoryValue.source = PreprocessExpr(args[1]);
		return true;
	}
	if (name == "HISTORY_CLEAN") {
		if (args.size() != 1) {
			err = "HISTORY_CLEAN expects 1 arg\n(history_name)";
			return false;
		}
		out.kind = NodeKind::HistoryClean;
		out.historyTarget = Trim(args[0]);
		if (!out.historyTarget.empty() && out.historyTarget[0] == '$')
			out.historyTarget.erase(0, 1);
		return true;
	}
	if (name == "GRAPH_LINE_CHART") {
		if (args.size() != 11) {
			err = "GRAPH_LINE_CHART expects 11 args\n"
				  "(x, y, w, h, min, max, direction,\n"
				  "lineCol, fillCol, conditions, history)";
			return false;
		}
		out.kind = NodeKind::GraphLineChart;
		out.eX.source        = PreprocessExpr(args[0]);
		out.eY.source        = PreprocessExpr(args[1]);
		out.eWidth.source    = PreprocessExpr(args[2]);
		out.eHeight.source   = PreprocessExpr(args[3]);
		out.eMin.source      = PreprocessExpr(args[4]);
		out.eMax.source      = PreprocessExpr(args[5]);
		std::string dir = Trim(args[6]);
		if      (dir == "LEFT_TO_RIGHT") out.graphDir = GraphDirection::LeftToRight;
		else if (dir == "RIGHT_TO_LEFT") out.graphDir = GraphDirection::RightToLeft;
		else { err = "GRAPH_LINE_CHART: bad direction '" + dir + "'"; return false; }
		out.eLineColor.source = PreprocessExpr(args[7]);
		out.eFillColor.source = PreprocessExpr(args[8]);
		out.graphCondsName    = Trim(args[9]);
		if (!out.graphCondsName.empty() && out.graphCondsName[0] == '$')
			out.graphCondsName.erase(0, 1);
		out.graphHistoryName  = Trim(args[10]);
		if (!out.graphHistoryName.empty() && out.graphHistoryName[0] == '$')
			out.graphHistoryName.erase(0, 1);
		return true;
	}
	if (name == "VAR") {
		if (args.size() != 2) { err = "VAR expects 2 args"; return false; }
		out.kind = NodeKind::Var;
		out.varName = Trim(args[0]);
		// strip leading $ if user wrote one
		if (!out.varName.empty() && out.varName[0] == '$')
			out.varName = out.varName.substr(1);
		if (out.varName.empty()) {
			err = "VAR with empty target name";
			return false;
		}
		// Stash the raw (un-preprocessed) RHS so the classification pass
		// can decide whether to parse it as numeric or string. We don't
		// preprocess yet because string segments need the original spelling
		// (in particular `.` -> `_` and `$` stripping happen per-segment
		// for string mode, and we don't want them to apply globally if
		// numeric mode is chosen).
		out.eVarValue.source = Trim(args[1]);
		if (out.eVarValue.source.empty()) {
			err = "VAR " + out.varName + " with empty RHS";
			return false;
		}
		return true;
	}
	if (name == "GET_DIMENSIONS") {
		if (args.size() != 3) { err = "GET_DIMENSIONS expects 3 args"; return false; }
		out.kind = NodeKind::GetDimensions;
		out.dimsName = Trim(args[0]);
		if (!out.dimsName.empty() && out.dimsName[0] == '$')
			out.dimsName = out.dimsName.substr(1);
		out.eFontSize.source = PreprocessExpr(args[1]);
		return ParseTextArg(args[2], out.textIsLiteral, out.textStrOrKey,
							out.textIsInlineFormat, out.textInlineExpr, err);
	}
	err = "unknown command: " + name;
	return false;
}

// Recursively parse a block of script lines until we hit an unmatched
// #else or #endif, which is consumed by the caller. Returns the count of
// input lines consumed.
struct LineCursor {
	std::vector<std::string>* lines;   // mutable so #elif desugar can rewrite
	size_t i;
};

// Context for a block being parsed -- distinguishes whether stray
// directives (`#endif` / `#else` / `#elif` / `#endfor`) should be errors or
// legitimate terminators. Cases:
//   * TopLevel    -- script root. Any of those directives is a stray error.
//   * IfThen      -- inside an #if's then-branch. #endif/#else/#elif all
//                    legitimately end the block.
//   * IfElse      -- inside an #if's else-branch. #endif ends the block;
//                    #else/#elif inside would be a double-else and error.
//   * For         -- inside a #for body. #endfor ends the block.
enum class BlockContext { TopLevel, IfThen, IfElse, For };

static bool ParseBlock(LineCursor& cur, std::vector<AstNode>& out,
					   BlockContext ctx, std::string& err);

static bool ParseScriptNode(LineCursor& cur, AstNode& out, std::string& err) {
	const std::string& line = (*cur.lines)[cur.i];
	std::string t = Trim(line);

	if (StartsWith(t, "#if")) {
		out.kind = NodeKind::If;
		out.condSource = PreprocessExpr(Trim(t.substr(3)));
		++cur.i;
		if (!ParseBlock(cur, out.thenBranch, BlockContext::IfThen, err))
			return false;
		// Either an #elif, #else, or #endif follows.
		if (cur.i >= cur.lines->size()) {
			err = "missing #endif for #if " + out.condSource;
			return false;
		}
		std::string tt = Trim((*cur.lines)[cur.i]);
		if (StartsWith(tt, "#elif")) {
			// Desugar `#elif cond` into  #else \n #if cond \n ... \n #endif.
			// The synthesised #if node lives inside this node's elseBranch
			// and parses recursively, picking up any further #elif chains.
			// Rewrite the line in place so ParseScriptNode sees `#if`.
			std::string& orig = (*cur.lines)[cur.i];
			size_t p = orig.find("#elif");
			if (p != std::string::npos) orig.replace(p, 5, "#if  ");
			AstNode nested;
			if (!ParseScriptNode(cur, nested, err)) return false;
			out.elseBranch.push_back(std::move(nested));
			// ParseScriptNode for the nested #if already consumed its
			// matching #endif, so we're done.
			return true;
		}
		if (StartsWith(tt, "#else")) {
			++cur.i;
			if (!ParseBlock(cur, out.elseBranch, BlockContext::IfElse, err))
				return false;
		}
		// Now we must be at #endif
		if (cur.i >= cur.lines->size()) {
			err = "missing #endif for #if " + out.condSource;
			return false;
		}
		if (!StartsWith(Trim((*cur.lines)[cur.i]), "#endif")) {
			err = "expected #endif for #if " + out.condSource
				+ ", saw: " + Trim((*cur.lines)[cur.i]);
			return false;
		}
		++cur.i;
		return true;
	}

	if (StartsWith(t, "#for")) {
		// Syntax: `#for $loopVar in $listName`
		// The `$` markers are optional on both names (we strip them anyway).
		out.kind = NodeKind::For;
		std::string rest = Trim(t.substr(4));
		// Find " in " (whole-word) separating the two identifiers.
		// Cheap parser: split by whitespace into tokens, find the "in" token.
		std::vector<std::string> tokens;
		{
			size_t i = 0;
			while (i < rest.size()) {
				while (i < rest.size() && std::isspace((unsigned char)rest[i])) ++i;
				size_t b = i;
				while (i < rest.size() && !std::isspace((unsigned char)rest[i])) ++i;
				if (b < i) tokens.push_back(rest.substr(b, i - b));
			}
		}
		// Expect exactly 3 tokens: <loopVar> "in" <listName>.
		if (tokens.size() != 3 || tokens[1] != "in") {
			err = "#for syntax: `#for $loopVar in $listName`, got: " + rest;
			return false;
		}
		auto stripDollar = [](std::string s) -> std::string {
			if (!s.empty() && s.front() == '$') s.erase(0, 1);
			return s;
		};
		out.loopVarName = stripDollar(tokens[0]);
		out.listName    = stripDollar(tokens[2]);
		if (out.loopVarName.empty() || out.listName.empty()) {
			err = "#for: loop variable and list name must be non-empty";
			return false;
		}
		++cur.i;
		if (!ParseBlock(cur, out.body, BlockContext::For, err)) return false;
		if (cur.i >= cur.lines->size()
		 || !StartsWith(Trim((*cur.lines)[cur.i]), "#endfor")) {
			err = "expected #endfor for #for " + out.loopVarName
				+ " in " + out.listName;
			return false;
		}
		++cur.i;
		return true;
	}

	// Otherwise expect a CMD{...}
	size_t open = t.find('{');
	if (open == std::string::npos) { err = "expected '{' in command: " + t; return false; }
	std::string name = Trim(t.substr(0, open));
	std::string body;
	size_t endIdx = 0;
	if (!ExtractBraceBody(t, open, body, endIdx, err)) return false;
	++cur.i;
	return ParseCommandBody(name, body, out, err);
}

static bool ParseBlock(LineCursor& cur, std::vector<AstNode>& out,
					   BlockContext ctx, std::string& err) {
	while (cur.i < cur.lines->size()) {
		std::string t = Trim((*cur.lines)[cur.i]);
		if (t.empty()) { ++cur.i; continue; }

		if (StartsWith(t, "#endif")) {
			if (ctx == BlockContext::TopLevel) {
				err = "stray #endif outside any #if block";
				return false;
			}
			return true;
		}
		if (StartsWith(t, "#else")) {
			if (ctx == BlockContext::TopLevel) {
				err = "stray #else outside any #if block";
				return false;
			}
			if (ctx == BlockContext::IfElse) {
				err = "duplicate #else (already inside an #else branch)";
				return false;
			}
			// ctx == IfThen: legitimate terminator.
			return true;
		}
		if (StartsWith(t, "#elif")) {
			if (ctx == BlockContext::TopLevel) {
				err = "stray #elif outside any #if block";
				return false;
			}
			if (ctx == BlockContext::IfElse) {
				err = "#elif after #else";
				return false;
			}
			// ctx == IfThen: legitimate terminator.
			return true;
		}
		if (StartsWith(t, "#endfor")) {
			if (ctx != BlockContext::For) {
				err = "stray #endfor outside any #for block";
				return false;
			}
			return true;
		}

		AstNode n;
		if (!ParseScriptNode(cur, n, err)) return false;
		out.push_back(std::move(n));
	}
	return true;
}

// ===========================================================================
// Document construction
// ===========================================================================

static void SeedConfigDefaults(Document::Impl& im);  // fwd-decl: defined just below Free()

Document::Document()  : m_impl(new Impl) { SeedConfigDefaults(*m_impl); }
Document::~Document() { Free(); delete m_impl; m_impl = nullptr; }

Document::Document(Document&& other) noexcept : m_impl(other.m_impl) {
	other.m_impl = new Impl;
	SeedConfigDefaults(*other.m_impl);
}
Document& Document::operator=(Document&& other) noexcept {
	if (this != &other) {
		Free();
		delete m_impl;
		m_impl = other.m_impl;
		other.m_impl = new Impl;
		SeedConfigDefaults(*other.m_impl);
	}
	return *this;
}

// Seed the read/write config defaults so files that omit a field still
// get a sensible value. The file's lines, parsed after this runs, will
// override anything the author actually specified.
static void SeedConfigDefaults(Document::Impl& im) {
	for (size_t i = 0; i < kConfigDefaultsCount; ++i) {
		const PredefinedConfigDefault& d = kConfigDefaults[i];
		ConfigValue cv;
		cv.kind = d.kind;
		switch (d.kind) {
			case ConfigKind::Integer:
				cv.intVal = d.intVal;
				break;
			case ConfigKind::Bool:
				cv.boolVal = d.intVal != 0;
				cv.intVal  = d.intVal;   // mirrored for expression access
				break;
			case ConfigKind::String:
				cv.stringVal = d.stringVal ? d.stringVal : "";
				break;
			default:
				break;
		}
		im.config[d.name] = std::move(cv);
	}
}

void Document::Free() {
	if (!m_impl) return;
	for (te_expr* e : m_impl->ownedExprs) if (e) te_free(e);
	m_impl->ownedExprs.clear();
	m_impl->arithCache.clear();   // entries already freed via ownedExprs
	m_impl->teVarTable.clear();
	m_impl->config.clear();
	m_impl->script.clear();
	m_impl->hostBinds.clear();
	m_impl->hostStringBinds.clear();
	m_impl->hostResArrays.clear();
	m_impl->stringVars.clear();
	m_impl->privateVars.clear();
	m_impl->dimsVars.clear();
	m_impl->dimsMeasureCache.clear();
	m_impl->implicitZero.clear();
	m_impl->configMirror.clear();
	m_impl->ownedFormatArgs.clear();
	m_impl->ownedNestedFormats.clear();
	m_impl->histories.clear();
	m_impl->floatSampleScratch.clear();
	m_impl->graphConds.clear();
	m_impl->lastError.clear();
	m_impl->lastErrorWrapped.clear();
	m_impl->lastErrorWrappedOf = 0;
	// Reset locale cache so the next LoadFromMemory re-fires RecordCallback.
	// The callback pointer and user data themselves survive Free() -- the host
	// should only need to call SetRecordCallback() once per Document.
	m_impl->ietfLocaleCached = false;
	m_impl->ietfLocaleCode.clear();
	// After clearing every prior state, restore the hardcoded read/write
	// defaults. Calling GetConfigInt("LayerWidth") on a freed Document
	// returns 448 again, matching the spec ("Free() restores them to
	// defaults").
	SeedConfigDefaults(*m_impl);
}

// Wrap a string to at most `maxCol` columns per line. Wraps on whitespace
// when possible; if a single token is longer than the column limit, it's
// hard-broken at the boundary. Used by LastError() so the host overlay
// doesn't have to deal with long-line layout.
static void WrapTo(const std::string& in, size_t maxCol, std::string& out) {
	out.clear();
	out.reserve(in.size() + in.size() / maxCol + 4);
	size_t i = 0, n = in.size();
	while (i < n) {
		// Honor existing newlines: emit up to them verbatim, then wrap
		// each natural line independently.
		size_t lineEnd = in.find('\n', i);
		if (lineEnd == std::string::npos) lineEnd = n;
		size_t lineStart = i;
		while (i < lineEnd) {
			size_t remaining = lineEnd - i;
			if (remaining <= maxCol) {
				out.append(in, i, remaining);
				i = lineEnd;
				break;
			}
			// Find the last whitespace inside [i, i+maxCol]
			size_t brk = std::string::npos;
			for (size_t j = i + maxCol; j > i; --j) {
				char c = in[j - 1];
				if (c == ' ' || c == '\t') { brk = j - 1; break; }
			}
			if (brk == std::string::npos || brk <= i) {
				// No whitespace found; hard-break at maxCol.
				out.append(in, i, maxCol);
				out.push_back('\n');
				i += maxCol;
			} else {
				out.append(in, i, brk - i);
				out.push_back('\n');
				i = brk + 1;   // skip the whitespace we broke on
				while (i < lineEnd && (in[i] == ' ' || in[i] == '\t')) ++i;
			}
		}
		if (lineEnd < n) { out.push_back('\n'); i = lineEnd + 1; }
		(void)lineStart;
	}
}

const char* Document::LastError() const {
	if (!m_impl) return "";
	const std::string& src = m_impl->lastError;
	if (src.empty()) {
		m_impl->lastErrorWrapped.clear();
		m_impl->lastErrorWrappedOf = 0;
		return "";
	}
	// Re-wrap only when the source error changed. Cheap test: store the
	// size_t the wrapped form corresponds to, plus we always re-derive
	// when the message size differs from the recorded snapshot.
	if (m_impl->lastErrorWrappedOf != src.size()
		|| m_impl->lastErrorWrapped.empty()) {
		WrapTo(src, /*maxCol=*/40, m_impl->lastErrorWrapped);
		m_impl->lastErrorWrappedOf = src.size();
	}
	return m_impl->lastErrorWrapped.c_str();
}

// ===========================================================================
// Loading
// ===========================================================================

// Swap the nibble order of a 16-bit value:
//   0x1234 -> 0x4321
// The script convention is to write colors in RGBA order; the hardware wants
// ABGR, so COLOR{0x1234} pre-swaps at parse/eval time. Used by both the
// literal-form text rewriter and the `color()` tinyexpr builtin.
static inline uint16_t SmdNibbleSwap16(uint16_t v) {
	return (uint16_t)(((v >> 12) & 0xF) << 0)
		 | (uint16_t)(((v >>  8) & 0xF) << 4)
		 | (uint16_t)(((v >>  4) & 0xF) << 8)
		 | (uint16_t)(((v >>  0) & 0xF) << 12);
}

// Rewrite every literal `COLOR{0xHHHH}` (and decimal `COLOR{N}` if the
// body fits in 16 bits) to its pre-swapped hex value in-place. Bodies
// that aren't a pure integer literal are left alone -- they'll be
// translated to `color(...)` calls inside PreprocessExpr and evaluated
// each frame.
static void RewriteLiteralColors(std::string& line) {
	if (line.find("COLOR") == std::string::npos) return;
	std::string out;
	out.reserve(line.size());
	bool inStr = false;
	for (size_t i = 0; i < line.size(); ) {
		char c = line[i];
		if (inStr) {
			out.push_back(c);
			if (c == '\\' && i + 1 < line.size()) { out.push_back(line[++i]); ++i; continue; }
			if (c == '"') inStr = false;
			++i; continue;
		}
		if (c == '"') { inStr = true; out.push_back(c); ++i; continue; }
		// Match COLOR followed by '{'. Word boundary on the left side
		// (otherwise NOT_A_COLOR{...} would catch).
		if (i + 6 <= line.size()
			&& line[i] == 'C' && line[i+1] == 'O' && line[i+2] == 'L'
			&& line[i+3] == 'O' && line[i+4] == 'R' && line[i+5] == '{'
			&& (i == 0 || (!std::isalnum((unsigned char)line[i-1]) && line[i-1] != '_'))) {
			// Find matching '}' on this line, tracking brace depth and strings
			int depth = 1; size_t j = i + 6; bool s = false;
			for (; j < line.size(); ++j) {
				char cc = line[j];
				if (s) {
					if (cc == '\\' && j + 1 < line.size()) { ++j; continue; }
					if (cc == '"') s = false;
					continue;
				}
				if (cc == '"') { s = true; continue; }
				if (cc == '{') ++depth;
				else if (cc == '}') { if (--depth == 0) break; }
			}
			if (j < line.size()) {
				std::string body(line, i + 6, j - i - 6);
				// Try to parse the body as a pure integer literal
				// (hex 0x... or decimal). Strip surrounding whitespace.
				std::string t;
				for (char ch : body)
					if (ch != ' ' && ch != '\t') t.push_back(ch);
				bool ok = !t.empty();
				long val = 0;
				if (ok) {
					char* endp = nullptr;
					val = std::strtol(t.c_str(), &endp,
									  (t.size() > 1 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) ? 16 : 10);
					if (!endp || *endp != '\0' || val < 0 || val > 0xFFFF) ok = false;
				}
				if (ok) {
					char buf[16];
					std::snprintf(buf, sizeof(buf), "0x%04X",
								  (unsigned)SmdNibbleSwap16((uint16_t)val));
					out.append(buf);
					i = j + 1;
					continue;
				}
				// Body isn't a pure literal -- leave the COLOR{...} as-is
				// so PreprocessExpr can translate it to color(...).
			}
		}
		out.push_back(c);
		++i;
	}
	line.swap(out);
}

bool Document::LoadFromFile(const char* path) {
	FILE* fp = std::fopen(path, "rb");
	if (!fp) { m_impl->lastError = "cannot open file"; return false; }
	std::fseek(fp, 0, SEEK_END);
	long sz = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	std::string buf;
	buf.resize(sz > 0 ? (size_t)sz : 0);
	if (sz > 0) {
		size_t got = std::fread(&buf[0], 1, (size_t)sz, fp);
		if (got != (size_t)sz) {
			std::fclose(fp);
			m_impl->lastError = "short read on file";
			return false;
		}
	}
	std::fclose(fp);
	return LoadFromMemory(buf.data(), buf.size());
}

bool Document::LoadFromMemory(const char* data, size_t size) {
	Free();
	// Tokenise into lines, strip comments + trim each.
	std::vector<std::string> rawLines, lines;
	{
		std::string cur;
		for (size_t i = 0; i < size; ++i) {
			char c = data[i];
			if (c == '\r') continue;
			if (c == '\n') { rawLines.push_back(std::move(cur)); cur.clear(); }
			else cur.push_back(c);
		}
		if (!cur.empty()) rawLines.push_back(std::move(cur));
	}
	for (auto& l : rawLines) {
		std::string s = StripLineComment(l);
		s = Trim(s);
		// Replace `COLOR{0xHHHH}` literals with the nibble-swapped hex value
		// so the rest of the parser sees an ordinary integer literal. Bodies
		// that aren't a pure integer (i.e. expression form) are kept as-is
		// and handled later by PreprocessExpr -> tinyexpr `color()` builtin.
		RewriteLiteralColors(s);
		lines.push_back(std::move(s));
	}

	// Split into config part and render part on "Start:"
	size_t startIdx = lines.size();
	for (size_t i = 0; i < lines.size(); ++i) {
		if (lines[i] == "Start:" || lines[i] == "Start: ") {
			startIdx = i;
			break;
		}
	}

	// -------- IETF pre-pass --------
	// Handles three config-section-only directives before the main parse:
	//
	//   key = IETF{"default string"}
	//       Declares an IETF-localised string object. The key lands in the
	//       config map as a ConfigKind::String once the pre-pass has baked
	//       the final (post-locale) value into the source line.
	//
	//   IETF_LOCALE{key, "locale-code", "localised string"}
	//       If locale-code matches the locale code obtained from
	//       RecordCallback, overwrites key's string with the localised value.
	//       Standalone line (no = / : separator); cleared to "" so the main
	//       parse skips it.
	//
	//   NAME_LOCALE{"locale-code", "localised name"}
	//       Only consumed by the Peek* family of functions; cleared here so
	//       the main parse never sees it.
	//
	// Additionally, wherever IETF_GET{key} appears in any line (config or
	// render script), it is replaced with "escaped-value-of-key" so the main
	// parser receives an ordinary quoted string literal.
	{
		// Helper: find the first ':' or '=' at brace/string depth 0.
		auto findSep = [](const std::string& ln, size_t& outPos, char& outCh) -> bool {
			int depth = 0; bool inStr = false;
			for (size_t j = 0; j < ln.size(); ++j) {
				char c = ln[j];
				if (inStr) {
					if (c == '\\' && j + 1 < ln.size()) { ++j; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{') ++depth;
				else if (c == '}') --depth;
				else if (depth == 0 && (c == ':' || c == '=')) {
					outPos = j; outCh = c; return true;
				}
			}
			return false;
		};

		// Helper: parse a raw quoted string ("..."), returning the Unescape'd
		// content. Returns false if the value doesn't start with '"'.
		auto parseQ = [](const std::string& s, std::string& out) -> bool {
			std::string v = Trim(s);
			if (v.size() < 2 || v.front() != '"') return false;
			size_t i = 1;
			while (i < v.size()) {
				if (v[i] == '\\' && i + 1 < v.size()) { i += 2; continue; }
				if (v[i] == '"') break;
				++i;
			}
			if (i >= v.size()) return false;
			out = Unescape(v.substr(1, i - 1));
			return true;
		};

		// Sub-pass A: collect IETF declarations and IETF_LOCALE directives.
		struct PendingLocale { std::string key, locale, value; };
		std::unordered_map<std::string, std::string> ietfMap; // name -> string
		std::vector<PendingLocale> pending;
		bool anyIetf = false;

		for (size_t i = 0; i < startIdx; ++i) {
			const std::string& ln = lines[i];
			if (ln.empty()) continue;

			// IETF_LOCALE{key, "locale", "value"} — standalone directive.
			if (StartsWith(ln, "IETF_LOCALE{")) {
				size_t lb = ln.find('{');
				std::string body; size_t endIdx2; std::string e2;
				if (lb != std::string::npos
					&& ExtractBraceBody(ln, lb, body, endIdx2, e2)) {
					auto parts = SplitTopLevelCommas(body);
					if (parts.size() == 3) {
						PendingLocale pl;
						pl.key = Trim(parts[0]);
						if (!pl.key.empty() && pl.key[0] == '$')
							pl.key.erase(0, 1);
						if (parseQ(parts[1], pl.locale)
							&& parseQ(parts[2], pl.value))
							pending.push_back(std::move(pl));
					}
				}
				continue; // handled; will be cleared in rewrite pass below
			}

			// NAME_LOCALE — only for Peek functions, nothing to collect here.
			if (StartsWith(ln, "NAME_LOCALE{")) continue;

			// key SEP IETF{"..."}
			size_t sepPos = 0; char sepCh = 0;
			if (!findSep(ln, sepPos, sepCh)) continue;
			std::string val = Trim(ln.substr(sepPos + 1));
			if (val.compare(0, 5, "IETF{") != 0) continue;

			std::string key = Trim(ln.substr(0, sepPos));
			// Extract the string from IETF{...}
			size_t lb = val.find('{');
			std::string body; size_t endIdx2; std::string e2;
			if (lb == std::string::npos
				|| !ExtractBraceBody(val, lb, body, endIdx2, e2)) continue;
			std::string bodyT = Trim(body);
			std::string ietfStr;
			if (!bodyT.empty() && bodyT.front() == '"') {
				if (!parseQ(bodyT, ietfStr)) ietfStr = bodyT;
			} else {
				ietfStr = bodyT;
			}
			ietfMap[key] = ietfStr;
			anyIetf = true;
		}

		// Sub-pass B: fire RecordCallback once on first IETF encounter.
		if (anyIetf && m_impl->ietfRecordCb && !m_impl->ietfLocaleCached) {
			m_impl->ietfRecordCb(m_impl->ietfLocaleCode, m_impl->ietfRecordUser);
			m_impl->ietfLocaleCached = true;
		}

		// Sub-pass C: apply matching IETF_LOCALE entries to ietfMap.
		for (auto& pl : pending) {
			if (pl.locale == m_impl->ietfLocaleCode) {
				auto it = ietfMap.find(pl.key);
				if (it != ietfMap.end()) it->second = pl.value;
			}
		}

		// Sub-pass D: rewrite lines.
		//
		// Rewriter for IETF_GET{X}: replaces every top-level (outside string
		// literals) occurrence with "escaped-value-of-X". Returns the line
		// unchanged when no IETF_GET is present (fast path).
		auto rewriteIetfGet = [&](const std::string& ln2) -> std::string {
			if (ln2.find("IETF_GET") == std::string::npos) return ln2;
			std::string out;
			out.reserve(ln2.size());
			bool inStr = false;
			size_t i = 0;
			while (i < ln2.size()) {
				char c = ln2[i];
				if (inStr) {
					out.push_back(c);
					if (c == '\\' && i + 1 < ln2.size())
						out.push_back(ln2[++i]);
					else if (c == '"') inStr = false;
					++i; continue;
				}
				if (c == '"') { inStr = true; out.push_back(c); ++i; continue; }
				// Detect "IETF_GET{" (9 chars: I E T F _ G E T {)
				if (i + 9 <= ln2.size()
					&& ln2.compare(i, 9, "IETF_GET{") == 0) {
					size_t brace = i + 8; // points at '{'
					std::string body2; size_t endIdx3; std::string e3;
					if (ExtractBraceBody(ln2, brace, body2, endIdx3, e3)) {
						std::string ident = Trim(body2);
						if (!ident.empty() && ident[0] == '$') ident.erase(0, 1);
						auto it = ietfMap.find(ident);
						std::string resolved = (it != ietfMap.end())
							? it->second : std::string();
						out.push_back('"');
						out += RescapeForLiteral(resolved);
						out.push_back('"');
						i = endIdx3;
						continue;
					}
				}
				out.push_back(c);
				++i;
			}
			return out;
		};

		for (size_t i = 0; i < lines.size(); ++i) {
			if (i == startIdx) continue; // leave "Start:" intact
			std::string& ln2 = lines[i];
			if (ln2.empty()) continue;

			if (i < startIdx) {
				// Config section ------------------------------------------

				// Clear standalone directives so the main parse skips them.
				if (StartsWith(ln2, "IETF_LOCALE{")
					|| StartsWith(ln2, "NAME_LOCALE{")) {
					ln2.clear();
					continue;
				}

				// Rewrite "key SEP IETF{...}" → "key SEP "escaped-value""
				size_t sepPos = 0; char sepCh2 = 0;
				if (findSep(ln2, sepPos, sepCh2)) {
					std::string val = Trim(ln2.substr(sepPos + 1));
					if (val.compare(0, 5, "IETF{") == 0) {
						std::string key = Trim(ln2.substr(0, sepPos));
						auto it = ietfMap.find(key);
						std::string str = (it != ietfMap.end()) ? it->second : "";
						// Reconstruct line with quoted literal value.
						ln2 = key + (sepCh2 == '=' ? " = " : ": ")
							+ "\"" + RescapeForLiteral(str) + "\"";
						continue;
					}
				}
			}

			// Apply IETF_GET{...} rewriting to config and script lines alike.
			ln2 = rewriteIetfGet(ln2);
		}
	}
	// -------- End IETF pre-pass --------

	// -------- Config section --------
	// Two separator characters are supported:
	//   key: value   -- 'default' kind. Re-seeded every Reset() so a
	//                   transient script-side `VAR{key, ...}` inside an
	//                   `#if` reverts when the branch stops firing.
	//   key = value  -- 'state' kind. Seeded once at Compile() (and on a
	//                   subsequent Free()/Load), never re-touched by
	//                   Reset(). Use for script-managed state that needs
	//                   to persist across frames (e.g. `lastFrame = 0`).
	// We pick whichever separator appears first at the top level (depth 0,
	// outside string literals). That way a Format like `{"FPS=%d", ...}`
	// doesn't accidentally trip the `=` path.
	for (size_t i = 0; i < startIdx; ++i) {
		const std::string& line = lines[i];
		if (line.empty()) continue;
		size_t sep = std::string::npos;
		char   sepCh = 0;
		{
			int depth = 0; bool inStr = false;
			for (size_t j = 0; j < line.size(); ++j) {
				char c = line[j];
				if (inStr) {
					if (c == '\\' && j + 1 < line.size()) { ++j; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{') ++depth;
				else if (c == '}') --depth;
				else if (depth == 0 && (c == ':' || c == '=')) {
					sep = j; sepCh = c; break;
				}
			}
		}
		if (sep == std::string::npos) {
			m_impl->lastError = "config line without ':' or '=' -- " + line;
			return false;
		}
		std::string key = Trim(line.substr(0, sep));
		std::string val = Trim(line.substr(sep + 1));
		if (key.empty()) {
			m_impl->lastError = "empty config key";
			return false;
		}
		// A small set of keys are required to be declared with `=` (constant-
		// kind). These describe the overlay itself rather than configurable
		// per-layout choices, so an author writing `Name: Foo` is almost
		// certainly making a typo. Reject up front with a clear message.
		if (sepCh == ':'
			&& (key == "Name" || key == "LayerWidth" || key == "LayerHeight")) {
			m_impl->lastError = "config '" + key
				+ "' must use '=' (constant), not ':'";
			return false;
		}
		ConfigValue cv;
		std::string err;
		if (!ParseConfigValue(val, cv, err)) {
			m_impl->lastError = "config '" + key + "': " + err;
			return false;
		}
		// Mark constant-kind keys so VAR rejects rebinding them and so
		// documentation/tools can distinguish at a glance.
		cv.isStateKind = (sepCh == '=');
		m_impl->config[key] = std::move(cv);
	}

	// -------- Render section --------
	if (startIdx < lines.size()) {
		LineCursor cur{ &lines, startIdx + 1 };
		std::string err;
		if (!ParseBlock(cur, m_impl->script, BlockContext::TopLevel, err)) {
			m_impl->lastError = "render script: " + err;
			return false;
		}
	}

	// -------- HISTORY allocation & GRAPH_CONDITIONS parse --------
	// Allocate ring buffers for HISTORY configs and split each
	// GRAPH_CONDITIONS body into rows (each row's expressions get compiled
	// later, alongside everything else). Done before ResolveFormatArgs
	// so a Format value can reference these names if needed.
	for (auto& kv : m_impl->config) {
		if (kv.second.kind == ConfigKind::History) {
			HistoryDecl h;
			const std::string& ty = kv.second.stringVal;
			if      (ty == "float")  h.type = HistoryType::Float;
			else if (ty == "double") h.type = HistoryType::Double;
			else                     h.type = HistoryType::Int;
			h.capacity = (size_t)kv.second.intVal;
			// Pre-reserve the active vector; the other two stay empty so
			// they cost nothing.
			switch (h.type) {
				case HistoryType::Int:    h.samplesI.reserve(h.capacity); break;
				case HistoryType::Float:  h.samplesF.reserve(h.capacity); break;
				case HistoryType::Double: h.samplesD.reserve(h.capacity); break;
			}
			m_impl->histories[kv.first] = std::move(h);
		} else if (kv.second.kind == ConfigKind::GraphConditions) {
			// body is in kv.second.stringVal (parser stashed it there).
			// Each row is { cond, lineCol, fillCol } -- so we need to walk
			// brace-grouped rows, then split each row by comma.
			const std::string& body = kv.second.stringVal;
			GraphCondDecl gcd;
			int depth = 0;
			bool inStr = false;
			size_t rowStart = std::string::npos;
			for (size_t i = 0; i < body.size(); ++i) {
				char c = body[i];
				if (inStr) {
					if (c == '\\' && i + 1 < body.size()) { ++i; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{') {
					if (depth == 0) rowStart = i + 1;
					++depth;
				} else if (c == '}') {
					--depth;
					if (depth == 0 && rowStart != std::string::npos) {
						std::string row = body.substr(rowStart, i - rowStart);
						auto parts = SplitTopLevelCommas(row);
						if (parts.size() != 3) {
							m_impl->lastError =
								"GRAPH_CONDITIONS row must have 3 entries: " + row;
							return false;
						}
						GraphCondParsed gp;
						gp.condSource = PreprocessExpr(parts[0]);
						gp.lineSource = PreprocessExpr(parts[1]);
						gp.fillSource = PreprocessExpr(parts[2]);
						gcd.rows.push_back(std::move(gp));
						rowStart = std::string::npos;
					}
				}
			}
			m_impl->graphConds[kv.first] = std::move(gcd);
		}
	}

	// -------- Format-arg tree pass --------
	// Build a FormatArg tree for each positional arg of every Format-typed
	// config value. This handles ternaries (`cond ? a : b`), nested format
	// specs in branches, etc. Has to happen here because BuildFormatArg
	// allocates into Impl::ownedFormatArgs.
	{
		std::string err;
		if (!ResolveFormatArgs(*m_impl, err)) {
			m_impl->lastError = err;
			return false;
		}
	}

	// -------- VAR classification pass --------
	// Walk the script in source order and decide for each VAR statement
	// whether the RHS is string-valued or numeric. A VAR is string if its
	// RHS contains a quoted string literal, or references an identifier
	// already known to be string-typed (config String/Format, or a private
	// string VAR set in an earlier source-order VAR statement). Across
	// multiple VAR statements that share the same name, the kind must be
	// consistent -- a name can't switch between numeric and string.
	{
		std::unordered_map<std::string, bool> stringNames;   // name -> true
		std::string classifyErr;
		std::function<bool(std::vector<AstNode>&)> classify =
			[&](std::vector<AstNode>& v) -> bool {
				for (auto& n : v) {
					if (n.kind == NodeKind::If) {
						// Snapshot stringNames so a classification made in
						// the then-branch doesn't leak into the else-branch
						// (and vice-versa), then merge both.
						auto snap = stringNames;
						if (!classify(n.thenBranch)) return false;
						auto afterThen = stringNames;
						stringNames = snap;
						if (!classify(n.elseBranch)) return false;
						for (auto& kv : afterThen)
							if (kv.second) stringNames[kv.first] = true;
						continue;
					}
					if (n.kind == NodeKind::For) {
						// Validate: listName must reference a ConfigKind::List.
						auto cit = m_impl->config.find(n.listName);
						if (cit == m_impl->config.end()
						 || cit->second.kind != ConfigKind::List) {
							classifyErr = "#for: '" + n.listName
								+ "' is not a LIST";
							return false;
						}
						n.loopVarType = cit->second.listVal.elemType;
						// String-typed loop var: stash in stringNames so the
						// body sees the loop variable as string-valued for
						// subsequent VAR string-classification and for the
						// `==`/`!=` string-compare path in EvalCondition.
						auto snap = stringNames;
						if (n.loopVarType == ListElemType::String) {
							stringNames[n.loopVarName] = true;
						} else {
							// Numeric loop var: make sure no stale string
							// classification lingers from an outer scope
							// using the same name.
							stringNames.erase(n.loopVarName);
						}
						if (!classify(n.body)) return false;
						// Restore: classifications made inside the loop body
						// shouldn't escape the loop scope (a VAR declared
						// inside the loop has the same persistence semantics
						// as one in any other block, but the loop variable
						// itself is loop-scoped only for classification).
						stringNames = snap;
						continue;
					}
					// Text and GetDimensions can carry an inline format spec
					// ({"fmt", args...}) as their trailing arg. The classify
					// pass is where every other StringExpr gets its nested-
					// format upgrade, so do the same here. Format args are
					// numeric expressions that the prescan and compile walks
					// below will pick up via the same per-segment loop used
					// for VAR's stringRhs.
					if ((n.kind == NodeKind::Text
					  || n.kind == NodeKind::GetDimensions)
					 && n.textIsInlineFormat) {
						if (!UpgradeNestedFormatSegments(*m_impl,
														n.textInlineExpr,
														classifyErr)) {
							return false;
						}
						continue;
					}
					if (n.kind != NodeKind::Var) continue;

					// Read-only check: VAR{<reserved>, ...} is rejected at
					// classification time. Catches the script trying to
					// overwrite a predefined runtime variable name.
					if (IsPredefinedReadOnlyName(n.varName)) {
						classifyErr = "VAR target '" + n.varName
							+ "' is a predefined read-only name";
						return false;
					}

					// Constant-kind config check: a `key = value` line at
					// the top of the .smd file declares `key` as immutable.
					// VAR cannot rebind such a name. Use `key: value`
					// instead if the script needs to reassign it later.
					{
						auto cit = m_impl->config.find(n.varName);
						if (cit != m_impl->config.end()
							&& cit->second.isStateKind) {
							classifyErr = "VAR target '" + n.varName
								+ "' is declared with '=' (constant); use "
								"':' in the config section if it should be "
								"mutable, or pick a different VAR name";
							return false;
						}
					}

					// Struct-copy form: VAR{Dst, Src[N]} where the RHS is a
					// bare array-slot reference with no .field access. We
					// detect on the raw (pre-preprocess) source so the
					// pattern is unambiguous.
					{
						std::string raw = Trim(n.eVarValue.source);
						if (!raw.empty() && raw[0] == '$') raw.erase(0, 1);
						// Match <ident>[<digits>]
						size_t lb = raw.find('[');
						if (lb != std::string::npos && raw.back() == ']') {
							std::string base = Trim(raw.substr(0, lb));
							std::string idxs = Trim(raw.substr(lb + 1,
													   raw.size() - lb - 2));
							// base is a plain identifier, idxs is digits only,
							// and nothing follows the ].
							auto isIdent = [](const std::string& s) {
								if (s.empty()) return false;
								if (!std::isalpha((unsigned char)s[0]) && s[0] != '_') return false;
								for (char c : s)
									if (!std::isalnum((unsigned char)c) && c != '_') return false;
								return true;
							};
							auto isDigits = [](const std::string& s) {
								if (s.empty()) return false;
								for (char c : s) if (!std::isdigit((unsigned char)c)) return false;
								return true;
							};
							if (isIdent(base) && isDigits(idxs)) {
								n.varIsStruct = true;
								n.varSrcWidthName  = base + "_" + idxs + "_width";
								n.varSrcHeightName = base + "_" + idxs + "_height";
								n.varSrcCallsName  = base + "_" + idxs + "_calls";
								n.eVarValue.source.clear();
								// Don't enter stringNames -- this VAR's name
								// alone is never read as a value; only its
								// .width / .height / .calls sub-fields are.
								continue;
							}
						}
					}

					bool isStr = ExprLooksLikeString(
						n.eVarValue.source, m_impl->config, stringNames);
					auto sit = stringNames.find(n.varName);
					if (sit != stringNames.end() && sit->second != isStr) {
						classifyErr = "VAR " + n.varName
							+ " used inconsistently as both numeric and string";
						return false;
					}
					if (isStr) {
						n.varIsString = true;
						if (!ParseStringExpr(n.eVarValue.source,
											 n.stringRhs, classifyErr)) {
							classifyErr = "VAR " + n.varName + ": " + classifyErr;
							return false;
						}
						if (!UpgradeNestedFormatSegments(*m_impl, n.stringRhs,
														classifyErr)) {
							classifyErr = "VAR " + n.varName + ": " + classifyErr;
							return false;
						}
						// numeric source is now unused; clear it so the
						// compile pass doesn't try to send it to tinyexpr.
						n.eVarValue.source.clear();
						stringNames[n.varName] = true;
					} else {
						n.varIsString = false;
						// Numeric VAR. Check for a top-level ternary first;
						// if present, split into cond/then/else so we can
						// route the cond through EvalCondition (the only
						// path that understands comparison operators).
						std::string cs, ts, es;
						if (SplitTopLevelTernary(n.eVarValue.source, cs, ts, es)) {
							n.varIsTernary = true;
							n.varCondSource = PreprocessExpr(cs);
							n.eVarThen.source = PreprocessExpr(ts);
							n.eVarElse.source = PreprocessExpr(es);
							n.eVarValue.source.clear();
						} else {
							// Now that we know it's plain numeric, run the
							// preprocess we deferred at parse time.
							n.eVarValue.source = PreprocessExpr(n.eVarValue.source);
						}
						stringNames.emplace(n.varName, false);
					}
				}
				return true;
			};
		if (!classify(m_impl->script)) {
			m_impl->lastError = "var classification: " + classifyErr;
			return false;
		}
	}
	m_impl->crc32 = crc32Calculate(data, size);
	return true;
}

uint32_t Document::GetFileHash() {
	return m_impl->crc32;
}

// ---------------------------------------------------------------------------
// Peek: extract Name / LayerWidth / LayerHeight without full parsing.
// ---------------------------------------------------------------------------
//
// Implementation note: this deliberately doesn't share code with the main
// parser. It walks the raw byte buffer line by line, strips comments, and
// inspects only the values of three top-level keys. No tinyexpr, no AST,
// no per-Document allocations. Intended for file-listing UIs that want a
// preview of every .smd in a folder.

// Pull a string-shaped value out of a config RHS. Handles quoted strings
// (with escape decoding) and bare unquoted text. Anything that can't be
// coerced returns false.
static bool ExtractStringLikeValue(const std::string& raw, std::string& out) {
	std::string v = Trim(raw);
	if (v.empty()) return false;
	if (v.front() == '"') {
		size_t i = 1;
		while (i < v.size()) {
			if (v[i] == '\\' && i + 1 < v.size()) { i += 2; continue; }
			if (v[i] == '"') break;
			++i;
		}
		if (i >= v.size()) return false;
		out = Unescape(v.substr(1, i - 1));
		return true;
	}
	out = v;
	return true;
}

// Parse an integer literal (decimal or 0x-prefixed hex, with optional
// sign). Returns true on success and writes the parsed value to *out.
// Trailing garbage after the number is a parse failure.
static bool ParseIntLiteral(const std::string& raw, int64_t* out) {
	std::string v = Trim(raw);
	if (v.empty()) return false;
	int base = 10;
	if (v.size() > 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) base = 16;
	else if (v.size() > 3 && (v[0] == '+' || v[0] == '-')
			 && v[1] == '0' && (v[2] == 'x' || v[2] == 'X')) base = 16;
	char* endp = nullptr;
	long long n = std::strtoll(v.c_str(), &endp, base);
	if (endp == v.c_str()) return false;
	while (endp && std::isspace((unsigned char)*endp)) ++endp;
	if (endp && *endp != '\0') return false;
	*out = (int64_t)n;
	return true;
}

bool Document::PeekFromMemory(const char* data, size_t size, PeekInfo& out,
							  const char* ietf_code) {
	out.name.clear();
	out.layerWidth  = 0;
	out.layerHeight = 0;
	bool foundName = false;

	size_t lineStart = 0;
	for (size_t i = 0; i <= size; ++i) {
		if (i != size && data[i] != '\n') continue;

		// Slice the current line [lineStart, i), peeling off a trailing \r.
		size_t end = i;
		while (end > lineStart && data[end - 1] == '\r') --end;
		std::string line(data + lineStart, end - lineStart);
		lineStart = i + 1;

		line = StripLineComment(line);
		line = Trim(line);
		if (line.empty()) continue;

		// Reached the render section -- everything we care about lives
		// above this point.
		if (line == "Start:" || line == "Start: ") break;

		// Peek only recognises the constant-kind ('=') form for Name,
		// LayerWidth, and LayerHeight: those three are always declared as
		// immutable constants in the .smd file. We pick the first `=` at
		// top level (depth 0, outside strings) -- a `:` is intentionally
		// not honoured here. Lines that don't have an `=` separator (e.g.
		// every `:` config line) are silently skipped.
		size_t sep = std::string::npos;
		{
			int depth = 0; bool inStr = false;
			for (size_t j = 0; j < line.size(); ++j) {
				char c = line[j];
				if (inStr) {
					if (c == '\\' && j + 1 < line.size()) { ++j; continue; }
					if (c == '"') inStr = false;
					continue;
				}
				if (c == '"') { inStr = true; continue; }
				if (c == '{') ++depth;
				else if (c == '}') --depth;
				else if (depth == 0 && c == '=') { sep = j; break; }
			}
		}
		if (sep == std::string::npos) {
			// NAME_LOCALE{"locale-code", "localised name"}
			// Replaces the Name value when the locale code matches ietf_code.
			if (ietf_code && foundName && StartsWith(line, "NAME_LOCALE{")) {
				size_t lb = line.find('{');
				std::string body; size_t endIdx2; std::string e2;
				if (lb != std::string::npos
					&& ExtractBraceBody(line, lb, body, endIdx2, e2)) {
					std::vector<std::string> parts = SplitTopLevelCommas(body);
					if (parts.size() == 2) {
						std::string locale, localName;
						if (ExtractStringLikeValue(parts[0], locale)
							&& ExtractStringLikeValue(parts[1], localName)
							&& locale == ietf_code) {
							out.name = localName;
						}
					}
				}
			}
			continue;
		}

		std::string key  = Trim(line.substr(0, sep));
		std::string rest = Trim(line.substr(sep + 1));

		if (!foundName && key == "Name") {
			if (ExtractStringLikeValue(rest, out.name)) foundName = true;
		} else if (key == "LayerWidth") {
			int64_t v = 0;
			if (ParseIntLiteral(rest, &v)) out.layerWidth = v;
		} else if (key == "LayerHeight") {
			int64_t v = 0;
			if (ParseIntLiteral(rest, &v)) out.layerHeight = v;
		}
	}
	return foundName;
}

bool Document::Peek(const char* path, PeekInfo& out, const char* ietf_code) {
	out.name.clear();
	out.layerWidth  = 0;
	out.layerHeight = 0;
	FILE* fp = std::fopen(path, "rb");
	if (!fp) return false;
	std::fseek(fp, 0, SEEK_END);
	long sz = std::ftell(fp);
	std::fseek(fp, 0, SEEK_SET);
	std::string buf;
	if (sz > 0) {
		buf.resize((size_t)sz);
		size_t got = std::fread(&buf[0], 1, (size_t)sz, fp);
		if (got != (size_t)sz) { std::fclose(fp); return false; }
	}
	std::fclose(fp);
	return PeekFromMemory(buf.data(), buf.size(), out, ietf_code);
}

// Backwards-compatible wrappers that only fill the Name.
bool Document::PeekName(const char* path, std::string& outName,
						const char* ietf_code) {
	PeekInfo info;
	bool ok = Peek(path, info, ietf_code);
	outName = std::move(info.name);
	return ok;
}

bool Document::PeekNameFromMemory(const char* data, size_t size,
								  std::string& outName,
								  const char* ietf_code) {
	PeekInfo info;
	bool ok = PeekFromMemory(data, size, info, ietf_code);
	outName = std::move(info.name);
	return ok;
}

// ===========================================================================
// Binding API
// ===========================================================================

void Document::SetRecordCallback(RecordCallback cb, void* user) {
	m_impl->ietfRecordCb   = cb;
	m_impl->ietfRecordUser = user;
}

static void DoBind(Document::Impl& im, const char* name, const void* p, BindType t) {
	HostBinding b;
	b.type = t;
	b.src = p;
	b.scratch = 0.0;
	im.hostBinds[name] = b;
}
void Document::BindInt64 (const char* n, const int64_t* p){ DoBind(*m_impl,n,p,BindType::Int64); }
void Document::BindFloat (const char* n, const float*   p){ DoBind(*m_impl,n,p,BindType::Float); }
void Document::BindDouble(const char* n, const double*  p){ DoBind(*m_impl,n,p,BindType::Double); }
void Document::BindBool  (const char* n, const bool*    p){ DoBind(*m_impl,n,p,BindType::Bool); }

void Document::BindString(const char* name, const std::string* ptr) {
	HostStringBinding b;
	b.src = ptr;
	m_impl->hostStringBinds[name] = b;
}

void Document::BindResolutionArray(const char* name, const ResolutionEntry* entries) {
	auto b = std::unique_ptr<HostResolutionArrayBinding>(new HostResolutionArrayBinding);
	b->src = entries;
	for (size_t i = 0; i < sizeof(b->scratch)/sizeof(b->scratch[0]); ++i)
		b->scratch[i] = 0.0;
	m_impl->hostResArrays[name] = std::move(b);
}

// ===========================================================================
// Compile
// ===========================================================================
//
// Compile every expression. tinyexpr lacks comparison + logical operators
// so anything that uses `==`, `>=`, `and`, `or`, ... is handled by our own
// `EvalCondition` helper (see below) -- those are NOT compiled here.
//
// To make sure unbound names don't break te_compile, we scan every arithmetic
// expression for identifiers and ensure each one has an entry in the
// te_variable table (zero-initialised if unknown).

// Names of tinyexpr builtins -- ScanIdentifiers already skips function calls
// (foo(...)) but pi/e are constants with no parens.
static const char* kBuiltinConstants[] = { "pi", "e", nullptr };
static bool IsBuiltinConst(const std::string& s) {
	for (size_t i = 0; kBuiltinConstants[i]; ++i)
		if (s == kBuiltinConstants[i]) return true;
	return false;
}

// Ensure a name has a backing double in the te_variable table. Returns
// pointer to the double tinyexpr should bind to.
static double* EnsureVariable(Document::Impl& im, const std::string& name) {
	if (IsBuiltinConst(name)) return nullptr;

	// Resolution order (highest priority first):
	//   1. private VAR     -- explicitly defined by the script
	//   2. config mirror   -- file author's static value (the file wins
	//                         over any host-bound default of the same name)
	//   3. host binding    -- runtime telemetry / defaults for names the
	//                         file doesn't define
	//   4. dims alias      -- `<name>_x` / `<name>_y` mirror
	//   5. implicit zero   -- fallback for unbound identifiers
	if (auto it = im.privateVars.find(name); it != im.privateVars.end())
		return it->second.get();

	if (auto it = im.configMirror.find(name); it != im.configMirror.end())
		return it->second.get();

	if (auto it = im.hostBinds.find(name); it != im.hostBinds.end())
		return &it->second.scratch;

	// Is this a resolution-array slot reference? Must be checked BEFORE
	// implicitZero -- the te-var-table build pass parks these slot names
	// in implicitZero for stable c_str() lifetime, so an implicitZero hit
	// for a resolution slot would shadow the real array scratch.
	//
	// The shape after preprocess is `<base>_<idx>_<field>` where field is
	// width/height/calls. We only treat it as such if the base name has a
	// bound array; otherwise a regular identifier ending in _width could
	// be silently rerouted.
	{
		static const char* kF[3] = { "_width", "_height", "_calls" };
		int fieldIdx = -1; size_t suffixLen = 0;
		for (int k = 0; k < 3; ++k) {
			size_t fl = std::strlen(kF[k]);
			if (name.size() > fl
				&& std::memcmp(name.data() + name.size() - fl, kF[k], fl) == 0) {
				fieldIdx = k; suffixLen = fl; break;
			}
		}
		if (fieldIdx >= 0) {
			std::string head = name.substr(0, name.size() - suffixLen);
			// head ends in `_<digits>`; split it.
			size_t us = head.rfind('_');
			if (us != std::string::npos && us + 1 < head.size()) {
				bool digits = true;
				for (size_t i = us + 1; i < head.size(); ++i)
					if (!std::isdigit((unsigned char)head[i])) { digits = false; break; }
				if (digits) {
					std::string base = head.substr(0, us);
					long idx = std::strtol(head.c_str() + us + 1, nullptr, 10);
					auto arr = im.hostResArrays.find(base);
					if (arr != im.hostResArrays.end() && idx >= 0 && idx < 8) {
						return &arr->second->scratch[idx * 3 + fieldIdx];
					}
				}
			}
		}
	}

	if (auto it = im.implicitZero.find(name); it != im.implicitZero.end())
		return it->second.get();

	// Is this a dims.x / dims.y reference? After preprocess it's `<name>_x`
	// or `<name>_y`. We only treat it as a dims alias if the base dims var
	// has already been registered (otherwise a user-defined identifier like
	// `width_offset_x` would be silently aliased onto a non-existent dims).
	if (name.size() > 2) {
		char last = name.back();
		if ((last == 'x' || last == 'y') && name[name.size()-2] == '_') {
			std::string base = name.substr(0, name.size() - 2);
			auto it = im.dimsVars.find(base);
			if (it != im.dimsVars.end()) {
				return (last == 'x') ? it->second.xMirror.get()
									 : it->second.yMirror.get();
			}
			// Fall through to implicit zero.
		}
	}

	// Otherwise: implicit zero
	auto& slot = im.implicitZero[name];
	if (!slot) slot.reset(new double(0));
	return slot.get();
}

// Forward decl: Compile()'s state-kind seed pass needs to materialise
// config Format values, which calls MaterializeText. The definition lives
// far below; this declaration brings the symbol into scope here. The same
// function gets a redundant forward decl at the start of the rendering
// section (it recurses through itself there too); harmless duplication.
static std::string MaterializeText(Document::Impl& im,
								   bool isLit,
								   const std::string& strOrKey,
								   int depth);
static inline std::string MaterializeText(Document::Impl& im,
										  bool isLit,
										  const std::string& strOrKey);

bool Document::Compile() {
	Impl& im = *m_impl;
	// Free any prior compiled expressions.
	for (te_expr* e : im.ownedExprs) if (e) te_free(e);
	im.ownedExprs.clear();
	im.arithCache.clear();   // dangles after ownedExprs is freed
	im.implicitZero.clear();
	im.configMirror.clear();

	// ----- Pass 1: walk the script and register every VAR / GET_DIMENSIONS
	// declaration. This MUST happen before any identifier prescan, otherwise
	// a `<name>_x` reference can't tell a real dims alias apart from an
	// unrelated user-named variable.
	std::function<void(std::vector<AstNode>&)> registerDecls =
		[&](std::vector<AstNode>& v) {
			for (auto& n : v) {
				switch (n.kind) {
					case NodeKind::If:
						registerDecls(n.thenBranch);
						registerDecls(n.elseBranch);
						break;
					case NodeKind::For: {
						// Look up the referenced list to decide where the loop
						// variable lives (numeric privateVars or stringVars).
						// If the list isn't in config, defer error to the
						// classifier's text classification pass below -- here
						// we just default to numeric so registration still
						// produces a valid backing slot.
						ListElemType lvt = ListElemType::Int;
						auto cit = im.config.find(n.listName);
						if (cit != im.config.end()
						 && cit->second.kind == ConfigKind::List) {
							lvt = cit->second.listVal.elemType;
						}
						n.loopVarType = lvt;
						if (lvt == ListElemType::String) {
							im.privateVars.erase(n.loopVarName);
							im.stringVars[n.loopVarName];   // value-init
						} else {
							if (im.privateVars.find(n.loopVarName)
								== im.privateVars.end()) {
								im.privateVars[n.loopVarName]
									.reset(new double(0));
							}
						}
						registerDecls(n.body);
						break;
					}
					case NodeKind::Var:
						if (n.varIsStruct) {
							// Struct-copy VAR: register three private
							// doubles for the .width / .height / .calls
							// sub-fields. The bare name n.varName itself
							// is never read as a value.
							for (const std::string& f : {
									 n.varName + "_width",
									 n.varName + "_height",
									 n.varName + "_calls" }) {
								if (im.privateVars.find(f) == im.privateVars.end())
									im.privateVars[f].reset(new double(0));
							}
						} else if (n.varIsString) {
							// String VAR: separate storage. Make sure we
							// don't collide with a numeric private VAR of
							// the same name (classification pass already
							// rejects that, but defensive double-check).
							im.privateVars.erase(n.varName);
							im.stringVars[n.varName];   // value-initialise ""
						} else {
							if (im.privateVars.find(n.varName) == im.privateVars.end())
								im.privateVars[n.varName].reset(new double(0));
						}
						break;
					case NodeKind::GetDimensions:
						if (im.dimsVars.find(n.dimsName) == im.dimsVars.end()) {
							Impl::DimsEntry de;
							de.dims    = std::unique_ptr<Dimensions>(new Dimensions);
							de.xMirror = std::unique_ptr<double>(new double(0));
							de.yMirror = std::unique_ptr<double>(new double(0));
							im.dimsVars.emplace(n.dimsName, std::move(de));
						}
						break;
					default:
						break;
				}
			}
		};
	registerDecls(im.script);

	// ----- Pass 2: pre-scan every arithmetic expression so each identifier
	// referenced has a backing double in EnsureVariable's various maps.
	auto preScan = [&](const std::string& src) {
		if (src.empty()) return;
		std::vector<std::string> ids;
		ScanIdentifiers(src, ids);
		for (auto& n : ids) EnsureVariable(im, n);
	};
	auto scanCExpr = [&](CExpr& e) { preScan(e.source); };
	// Walk format-spec args inside config too. String args (%s) are looked
	// up by name at materialise time and never go through tinyexpr, so
	// skip them in the identifier scan.
	//
	// For each Format arg, walk the FormatArg tree built by
	// ResolveFormatArgs and prescan every numeric/cond source it contains.
	std::function<void(FormatArg*, bool)> scanFmtArg =
		[&](FormatArg* a, bool isStrSlot) {
			if (!a) return;
			switch (a->kind) {
				case FormatArgKind::Numeric:
					if (!isStrSlot) preScan(a->numericSource);
					break;
				case FormatArgKind::StringExpr_:
					// identifier resolution at materialise time -- nothing
					// to register here.
					break;
				case FormatArgKind::NestedFormat:
					if (a->nested) {
						FormatSpec& nf = *a->nested;
						for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
							bool nIsStr = i < nf.argIsString.size() && nf.argIsString[i];
							scanFmtArg((FormatArg*)nf.argTernaries[i], nIsStr);
						}
					}
					break;
				case FormatArgKind::Ternary:
					preScan(a->condSource);
					scanFmtArg(a->thenArg, isStrSlot);
					scanFmtArg(a->elseArg, isStrSlot);
					break;
			}
		};
	for (auto& kv : im.config) {
		if (kv.second.kind == ConfigKind::Format) {
			auto& f = kv.second.fmtVal;
			for (size_t i = 0; i < f.argTernaries.size(); ++i) {
				bool isStr = i < f.argIsString.size() && f.argIsString[i];
				scanFmtArg((FormatArg*)f.argTernaries[i], isStr);
			}
		}
	}

	// Prescan GRAPH_CONDITIONS: each row references `x` (which we'll bind
	// per-row to a scratch double), plus any other identifiers in the
	// line/fill colour expressions. The `x` symbol is handled specially
	// during compile.
	for (auto& gkv : im.graphConds) {
		for (auto& row : gkv.second.rows) {
			preScan(row.condSource);
			preScan(row.lineSource);
			preScan(row.fillSource);
		}
	}
	std::function<void(std::vector<AstNode>&)> walk = [&](std::vector<AstNode>& v){
		for (auto& n : v) {
			switch (n.kind) {
				case NodeKind::If:
					preScan(n.condSource);
					walk(n.thenBranch);
					walk(n.elseBranch);
					break;
				case NodeKind::For:
					// No source expressions on the For node itself; loop var
					// and list name are bare identifiers. Just recurse into
					// the body so its expressions get prescanned.
					walk(n.body);
					break;
				case NodeKind::Text:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eFontSize); scanCExpr(n.eColor);
					// Inline `{"%d", args...}` arg: walk its format-arg
					// trees the same way VAR's stringRhs does below.
					if (n.textIsInlineFormat) {
						for (auto& seg : n.textInlineExpr.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								scanFmtArg((FormatArg*)nf.argTernaries[i], nIsStr);
							}
						}
					}
					break;
				case NodeKind::Box:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eWidth); scanCExpr(n.eHeight);
					scanCExpr(n.eColorBox);
					break;
				case NodeKind::RoundedBox:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eWidth); scanCExpr(n.eHeight);
					scanCExpr(n.eRoundnessTL); scanCExpr(n.eRoundnessTR); scanCExpr(n.eRoundnessBL); scanCExpr(n.eRoundnessBR);
					scanCExpr(n.eColorBox);
					break;
				case NodeKind::EmptyBox:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eWidth); scanCExpr(n.eHeight);
					scanCExpr(n.eColorBox);
					break;
				case NodeKind::DashedLine:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eX2); scanCExpr(n.eY2); scanCExpr(n.eDashOn); scanCExpr(n.eDashOff);
					scanCExpr(n.eColorBox);
					break;
				case NodeKind::HistoryUpdate:
					scanCExpr(n.eHistoryValue);
					break;
				case NodeKind::HistoryClean:
					break;
				case NodeKind::GraphLineChart:
					scanCExpr(n.eX); scanCExpr(n.eY);
					scanCExpr(n.eWidth); scanCExpr(n.eHeight);
					scanCExpr(n.eMin); scanCExpr(n.eMax);
					scanCExpr(n.eLineColor); scanCExpr(n.eFillColor);
					break;
				case NodeKind::Var:
					if (n.varIsStruct) {
						// Source slot names need to be reachable from
						// tinyexpr at evaluate time. They get registered
						// as part of either (a) hostResArrays (if the host
						// binds an array of that name) or (b) implicit
						// zero (if no such array exists). Either way, we
						// touch them here so EnsureVariable produces a
						// backing double.
						EnsureVariable(im, n.varSrcWidthName);
						EnsureVariable(im, n.varSrcHeightName);
						EnsureVariable(im, n.varSrcCallsName);
						// Destination V_width / V_height / V_calls were
						// already registered as private VARs in pass 1.
					} else if (n.varIsString) {
						// String VARs use name-resolution for identifier
						// segments, but `{...}` nested-format segments
						// still have numeric/cond expressions inside that
						// need identifier registration.
						for (auto& seg : n.stringRhs.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								scanFmtArg((FormatArg*)nf.argTernaries[i], nIsStr);
							}
						}
					} else if (n.varIsTernary) {
						// Cond goes through EvalCondition, but its identifier
						// tokens still need binding.
						preScan(n.varCondSource);
						scanCExpr(n.eVarThen);
						scanCExpr(n.eVarElse);
					} else {
						scanCExpr(n.eVarValue);
					}
					break;
				case NodeKind::GetDimensions:
					scanCExpr(n.eFontSize);
					// Same inline-format handling as Text above.
					if (n.textIsInlineFormat) {
						for (auto& seg : n.textInlineExpr.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								scanFmtArg((FormatArg*)nf.argTernaries[i], nIsStr);
							}
						}
					}
					break;
			}
		}
	};
	walk(im.script);

	// ----- Build a stable te_variable table.
	// Reserve generously so push_back never reallocates while we hold
	// pointers to its elements during te_compile.
	//   per-array slots: 24 names (8 entries x 3 fields)
	//   System_Key_*  : kSystemKeysCount names
	size_t needed = im.hostBinds.size() + im.privateVars.size()
				  + im.configMirror.size() + im.implicitZero.size()
				  + im.dimsVars.size() * 2
				  + im.hostResArrays.size() * 24
				  + kSystemKeysCount
				  + 8 + 1;  // +4 dim alias slop, +4 base fns, +1 color()
	im.teVarTable.clear();
	im.teVarTable.reserve(needed);

	// Mirror integer/bool config values for expression use.
	for (auto& kv : im.config) {
		if (kv.second.kind == ConfigKind::Integer || kv.second.kind == ConfigKind::Bool) {
			auto& slot = im.configMirror[kv.first];
			if (!slot) slot.reset(new double(0));
			*slot = (double)kv.second.intVal;
		}
	}

	// Build the te_variable table. Each name appears at most once. The
	// priority order (highest first) must match EnsureVariable above:
	//   1. private VAR     -- script-declared, can shadow anything
	//   2. config mirror   -- file author's static integer/bool
	//   3. host binding    -- runtime telemetry; fallback for names the
	//                         file doesn't define
	//   4. implicit zero   -- unbound identifiers
	// dims aliases are pushed separately further down.
	//
	// Also: register a few custom math functions tinyexpr doesn't provide
	// out of the box but our .smd files want -- round(), trunc(), min(),
	// max(). Pointer casts to (const void*) are how tinyexpr stores them.
	static const auto te_round = +[](double x) -> double {
		return x >= 0.0 ? std::floor(x + 0.5) : -std::floor(-x + 0.5);
	};
	static const auto te_trunc = +[](double x) -> double { return (double)(long long)x; };
	static const auto te_min   = +[](double a, double b) -> double { return a < b ? a : b; };
	static const auto te_max   = +[](double a, double b) -> double { return a > b ? a : b; };
	// color(x): nibble-swap a 16-bit value (`0x1234` -> `0x4321`). Used by
	// the expression form of COLOR{} -- the parse-time literal form is
	// already swapped at line ingest. Negative or out-of-range inputs are
	// truncated to 16 bits before swapping.
	static const auto te_color = +[](double x) -> double {
		int64_t v = (int64_t)x;
		uint16_t u = (uint16_t)(v & 0xFFFF);
		return (double)SmdNibbleSwap16(u);
	};
	auto fnPtr1 = [](double (*f)(double)) -> const void* {
		return reinterpret_cast<const void*>(f);
	};
	auto fnPtr2 = [](double (*f)(double, double)) -> const void* {
		return reinterpret_cast<const void*>(f);
	};
	constexpr int TE_FN1 = 9 | 32;
	constexpr int TE_FN2 = 10 | 32;
	im.teVarTable.push_back({ "round", fnPtr1(te_round), TE_FN1, nullptr });
	im.teVarTable.push_back({ "trunc", fnPtr1(te_trunc), TE_FN1, nullptr });
	im.teVarTable.push_back({ "min",   fnPtr2(te_min),   TE_FN2, nullptr });
	im.teVarTable.push_back({ "max",   fnPtr2(te_max),   TE_FN2, nullptr });
	im.teVarTable.push_back({ "color", fnPtr1(te_color), TE_FN1, nullptr });

	{
		std::unordered_set<std::string> claimed;
		claimed.reserve(needed);
		auto pushIfNew = [&](const std::string& name, double* p) {
			if (claimed.insert(name).second)
				im.teVarTable.push_back({ name.c_str(), p, 0, nullptr });
		};
		for (auto& kv : im.privateVars)
			pushIfNew(kv.first, kv.second.get());
		for (auto& kv : im.configMirror)
			pushIfNew(kv.first, kv.second.get());
		for (auto& kv : im.hostBinds)
			pushIfNew(kv.first, &kv.second.scratch);
		for (auto& kv : im.implicitZero)
			pushIfNew(kv.first, kv.second.get());

		// Resolution-array slot names. Each bound array contributes 24 slots
		// (8 entries x {width, height, calls}) named
		//   <base>_<idx>_width / <base>_<idx>_height / <base>_<idx>_calls
		// and pointing at the per-frame scratch doubles in the binding.
		// Name strings are parked in implicitZero so they have stable
		// lifetime; the te_variable points at the scratch double directly.
		static const char* kFields[3] = { "width", "height", "calls" };
		for (auto& kv : im.hostResArrays) {
			const std::string& base = kv.first;
			HostResolutionArrayBinding& rb = *kv.second;
			for (size_t idx = 0; idx < 8; ++idx) {
				for (size_t fi = 0; fi < 3; ++fi) {
					std::string nm = base + "_" + std::to_string(idx) + "_" + kFields[fi];
					// Park the name in implicitZero so its key is stable.
					auto& slot = im.implicitZero[nm];
					if (!slot) slot.reset(new double(0));
					// Don't push if pushIfNew already claimed this name above
					// (would mean an unrelated identifier with the same spelling
					// ended up implicit-zeroed; resolution data wins).
					auto it = im.implicitZero.find(nm);
					if (claimed.insert(nm).second) {
						im.teVarTable.push_back({
							it->first.c_str(),
							&rb.scratch[idx * 3 + fi],
							0, nullptr
						});
					}
				}
			}
		}

		// System_Key_* constants. Their values are immutable; we store them
		// in implicitZero with the literal bit value and point te_variable
		// at that storage.
		for (size_t i = 0; i < kSystemKeysCount; ++i) {
			const std::string nm = kSystemKeys[i].name;
			auto& slot = im.implicitZero[nm];
			if (!slot) slot.reset(new double(0));
			*slot = (double)kSystemKeys[i].value;
			auto it = im.implicitZero.find(nm);
			if (claimed.insert(nm).second) {
				im.teVarTable.push_back({ it->first.c_str(), slot.get(), 0, nullptr });
			}
		}
	}

	// Dim aliases need stable backing strings; store them in Impl directly.
	// Reuse implicitZero map's storage trick: create a private vector below.
	// Since teVarTable holds raw const char* into other map keys, we need
	// these alias names to persist somewhere stable too. We park them in
	// the implicitZero map (which won't grow further after Compile).
	for (auto& kv : im.dimsVars) {
		std::string nx = kv.first + "_x";
		std::string ny = kv.first + "_y";
		// Stash names into implicitZero to keep them alive with stable keys.
		auto& sx = im.implicitZero[nx];
		if (!sx) sx.reset(new double(0));
		*sx = (double)kv.second.dims->x;        // not used directly; alias only
		auto& sy = im.implicitZero[ny];
		if (!sy) sy.reset(new double(0));
		*sy = (double)kv.second.dims->y;
		// Push aliases pointing at the mirror doubles (which we refresh per frame).
		im.teVarTable.push_back({ im.implicitZero.find(nx)->first.c_str(),
								  kv.second.xMirror.get(), 0, nullptr });
		im.teVarTable.push_back({ im.implicitZero.find(ny)->first.c_str(),
								  kv.second.yMirror.get(), 0, nullptr });
	}

	// ----- Compile every arithmetic expression.
	auto compile = [&](CExpr& e, const char* where) -> bool {
		if (e.source.empty()) return true;
		int err = 0;
		te_expr* compiled = te_compile(e.source.c_str(),
									   im.teVarTable.data(),
									   (int)im.teVarTable.size(),
									   &err);
		if (!compiled) {
			char buf[256];
			std::snprintf(buf, sizeof(buf),
						  "compile error in %s at offset %d: %s",
						  where, err, e.source.c_str());
			im.lastError = buf;
			return false;
		}
		e.compiled = compiled;
		im.ownedExprs.push_back(compiled);
		return true;
	};

	// Compile a single FormatArg subtree. The same helper is used for
	// top-level config Format args and for branches of ternaries.
	std::function<bool(FormatArg*, const std::string&, bool)> compileFmtArg =
		[&](FormatArg* a, const std::string& ctx, bool isStrSlot) -> bool {
			if (!a) return true;
			switch (a->kind) {
				case FormatArgKind::Numeric:
					if (!isStrSlot) {
						int e = 0;
						te_expr* p = te_compile(a->numericSource.c_str(),
												im.teVarTable.data(),
												(int)im.teVarTable.size(),
												&e);
						if (!p) {
							char buf[256];
							std::snprintf(buf, sizeof(buf),
								"compile error in %s arg '%s' (offset %d)",
								ctx.c_str(), a->numericSource.c_str(), e);
							im.lastError = buf;
							return false;
						}
						a->numericCompiled = p;
						im.ownedExprs.push_back(p);
					}
					// For a string slot, the arg should be a string-shaped
					// FormatArg (StringExpr / NestedFormat / Ternary). A
					// bare Numeric here is unusual but tolerable -- it will
					// be stringified at materialise time. We compile it so
					// the value is available.
					else {
						int e = 0;
						te_expr* p = te_compile(a->numericSource.c_str(),
												im.teVarTable.data(),
												(int)im.teVarTable.size(),
												&e);
						if (p) {
							a->numericCompiled = p;
							im.ownedExprs.push_back(p);
						}
					}
					return true;
				case FormatArgKind::StringExpr_:
					return true;
				case FormatArgKind::NestedFormat: {
					if (!a->nested) return true;
					FormatSpec& nf = *a->nested;
					for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
						bool nIsStr = i < nf.argIsString.size() && nf.argIsString[i];
						if (!compileFmtArg((FormatArg*)nf.argTernaries[i],
										   ctx + ".nested", nIsStr))
							return false;
					}
					return true;
				}
				case FormatArgKind::Ternary: {
					// Don't te_compile the cond -- it may contain
					// comparison operators (==, >=, <) which tinyexpr can't
					// handle. We evaluate it through EvalCondition at
					// materialise time instead. The cond source stays in
					// a->condSource as-is.
					if (!compileFmtArg(a->thenArg, ctx + ".then", isStrSlot))
						return false;
					if (!compileFmtArg(a->elseArg, ctx + ".else", isStrSlot))
						return false;
					return true;
				}
			}
			return true;
		};

	// Compile format-spec args via the FormatArg tree built by
	// ResolveFormatArgs. The older `compiledArgs` path is kept zero-filled
	// so anything that still looks at it doesn't dereference garbage; the
	// materialise routine prefers argTernaries.
	for (auto& kv : im.config) {
		if (kv.second.kind != ConfigKind::Format) continue;
		FormatSpec& f = kv.second.fmtVal;
		f.compiledArgs.assign(f.argExprs.size(), nullptr);
		for (size_t i = 0; i < f.argTernaries.size(); ++i) {
			bool isStr = i < f.argIsString.size() && f.argIsString[i];
			FormatArg* arg = (FormatArg*)f.argTernaries[i];
			if (!compileFmtArg(arg, "config '" + kv.first + "'", isStr))
				return false;
		}
	}

	// Compile GRAPH_CONDITIONS rows. Each row has its own per-row scratch
	// double for `x`, registered as an additional te_variable so the row's
	// line/fill colour expressions can reference it. The cond expression
	// is NOT compiled here -- it may contain comparison ops which tinyexpr
	// can't parse; we evaluate it through EvalCondition at frame time
	// (with `x` bound via CondCache::extraVars).
	for (auto& gkv : im.graphConds) {
		for (auto& row : gkv.second.rows) {
			std::vector<te_variable> rowVars;
			rowVars.reserve(im.teVarTable.size() + 1);
			rowVars.push_back({ "x", &row.xScratch, 0, nullptr });
			for (auto& v : im.teVarTable) rowVars.push_back(v);

			auto compileOne = [&](const std::string& src, te_expr*& out_,
								  const char* what) -> bool {
				int e = 0;
				te_expr* p = te_compile(src.c_str(), rowVars.data(),
										(int)rowVars.size(), &e);
				if (!p) {
					char buf[256];
					std::snprintf(buf, sizeof(buf),
						"GRAPH_CONDITIONS '%s' %s '%s' (offset %d)",
						gkv.first.c_str(), what, src.c_str(), e);
					im.lastError = buf;
					return false;
				}
				out_ = p;
				im.ownedExprs.push_back(p);
				return true;
			};
			// Cond is NOT compiled (see above) -- left as raw source.
			row.condCompiled = nullptr;
			if (!compileOne(row.lineSource, row.lineCompiled, "lineCol")) return false;
			if (!compileOne(row.fillSource, row.fillCompiled, "fillCol")) return false;
		}
	}

	// Compile script expressions.
	std::function<bool(std::vector<AstNode>&)> compileBlock = [&](std::vector<AstNode>& v) -> bool {
		for (auto& n : v) {
			switch (n.kind) {
				case NodeKind::If:
					// Condition is NOT a tinyexpr expression -- we evaluate
					// it ourselves (see EvalCondition).
					if (!compileBlock(n.thenBranch)) return false;
					if (!compileBlock(n.elseBranch)) return false;
					break;
				case NodeKind::For:
					// Loop var lives in the existing varStore / stringVars
					// (registered in pass 1). No expression on the For node
					// itself to compile -- the body compiles normally.
					if (!compileBlock(n.body)) return false;
					break;
				case NodeKind::Text:
					if (!compile(n.eX,"TEXT.x"))        return false;
					if (!compile(n.eY,"TEXT.y"))        return false;
					if (!compile(n.eFontSize,"TEXT.fs"))return false;
					if (!compile(n.eColor,"TEXT.col"))  return false;
					if (n.textIsInlineFormat) {
						for (auto& seg : n.textInlineExpr.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								if (!compileFmtArg((FormatArg*)nf.argTernaries[i],
												   "TEXT inline fmt", nIsStr))
									return false;
							}
						}
					}
					break;
				case NodeKind::Box:
					if (!compile(n.eX,"BOX.x"))         return false;
					if (!compile(n.eY,"BOX.y"))         return false;
					if (!compile(n.eWidth,"BOX.w"))     return false;
					if (!compile(n.eHeight,"BOX.h"))    return false;
					if (!compile(n.eColorBox,"BOX.col"))return false;
					break;
				case NodeKind::RoundedBox:
					if (!compile(n.eX,"RBOX.x"))              return false;
					if (!compile(n.eY,"RBOX.y"))              return false;
					if (!compile(n.eWidth,"RBOX.w"))          return false;
					if (!compile(n.eHeight,"RBOX.h"))         return false;
					if (!compile(n.eRoundnessTL,"RBOX.rndtl"))return false;
					if (!compile(n.eRoundnessTR,"RBOX.rndtr"))return false;
					if (!compile(n.eRoundnessBL,"RBOX.rndbl"))return false;
					if (!compile(n.eRoundnessBR,"RBOX.rndbr"))return false;
					if (!compile(n.eColorBox,"RBOX.col"))     return false;
					break;
				case NodeKind::EmptyBox:
					if (!compile(n.eX,"EBOX.x"))         return false;
					if (!compile(n.eY,"EBOX.y"))         return false;
					if (!compile(n.eWidth,"EBOX.w"))     return false;
					if (!compile(n.eHeight,"EBOX.h"))    return false;
					if (!compile(n.eColorBox,"EBOX.col"))return false;
					break;
				case NodeKind::DashedLine:
					if (!compile(n.eX,"DL.x"))           return false;
					if (!compile(n.eY,"DL.y"))           return false;
					if (!compile(n.eX2,"DL.x2"))         return false;
					if (!compile(n.eY2,"DL.y2"))         return false;
					if (!compile(n.eDashOn,"DL.on"))     return false;
					if (!compile(n.eDashOff,"DL.off"))   return false;
					if (!compile(n.eColorBox,"DL.col"))  return false;
					break;
				case NodeKind::HistoryUpdate:
					if (!compile(n.eHistoryValue,"HU.val")) return false;
					break;
				case NodeKind::HistoryClean:
					break;
				case NodeKind::GraphLineChart:
					if (!compile(n.eX,"GLC.x"))           return false;
					if (!compile(n.eY,"GLC.y"))           return false;
					if (!compile(n.eWidth,"GLC.w"))       return false;
					if (!compile(n.eHeight,"GLC.h"))      return false;
					if (!compile(n.eMin,"GLC.min"))       return false;
					if (!compile(n.eMax,"GLC.max"))       return false;
					if (!compile(n.eLineColor,"GLC.lcol"))return false;
					if (!compile(n.eFillColor,"GLC.fcol"))return false;
					break;
				case NodeKind::Var:
					// Struct-copy VARs have no expression to compile.
					if (n.varIsStruct) break;
					// String VARs are evaluated by name resolution at frame
					// time. The identifier segments need no compilation, but
					// any `{...}` nested-format segments still own a tree of
					// numeric expressions that must be te_compile'd here.
					if (n.varIsString) {
						for (auto& seg : n.stringRhs.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							std::string ctx = "VAR '" + n.varName + "' nested fmt";
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								if (!compileFmtArg((FormatArg*)nf.argTernaries[i],
												   ctx, nIsStr))
									return false;
							}
						}
						break;
					}
					if (n.varIsTernary) {
						// The cond goes through EvalCondition; only compile
						// the then/else branches as straight tinyexpr.
						if (!compile(n.eVarThen, "VAR.then")) return false;
						if (!compile(n.eVarElse, "VAR.else")) return false;
					} else {
						if (!compile(n.eVarValue,"VAR.val")) return false;
					}
					break;
				case NodeKind::GetDimensions:
					if (!compile(n.eFontSize,"GD.fs"))  return false;
					if (n.textIsInlineFormat) {
						for (auto& seg : n.textInlineExpr.segments) {
							if (!seg.nestedFmt) continue;
							FormatSpec& nf = *(FormatSpec*)seg.nestedFmt;
							for (size_t i = 0; i < nf.argTernaries.size(); ++i) {
								bool nIsStr = i < nf.argIsString.size()
											  && nf.argIsString[i];
								if (!compileFmtArg((FormatArg*)nf.argTernaries[i],
												   "GET_DIMENSIONS inline fmt",
												   nIsStr))
									return false;
							}
						}
					}
					break;
			}
		}
		return true;
	};
	if (!compileBlock(im.script)) return false;

	// One-time seed pass for ALL config values that have a matching
	// privateVar / stringVar. Both `:` (default-kind) and `=` (constant-
	// kind) are now compile-time-once seeds; the difference between the
	// two kinds is whether VAR can rebind the name later (constant-kind
	// forbids it; default-kind allows it). Reset() doesn't re-seed any of
	// these -- it only zeros un-config-backed VARs and re-materialises
	// Format-kind string configs (because Format values are explicitly
	// declared as "re-evaluate against live host data each frame").
	for (auto& kv : im.config) {
		auto pit = im.privateVars.find(kv.first);
		if (pit != im.privateVars.end() && pit->second) {
			if (kv.second.kind == ConfigKind::Integer
			 || kv.second.kind == ConfigKind::Bool) {
				*pit->second = (double)kv.second.intVal;
				continue;
			}
		}
		auto sit = im.stringVars.find(kv.first);
		if (sit != im.stringVars.end()) {
			if (kv.second.kind == ConfigKind::String) {
				sit->second = kv.second.stringVal;
			} else if (kv.second.kind == ConfigKind::Format) {
				sit->second = MaterializeText(im, /*isLit=*/false, kv.first);
			}
		}
	}

	// First Reset(): zeroes un-config-backed VARs and re-materialises the
	// Format-kind strings (which are intentionally "always live").
	Reset();
	return true;
}

// ===========================================================================
// Per-frame value refresh
// ===========================================================================

static void RefreshScratches(Document::Impl& im) {
	// Pull each host-bound source into its scratch double.
	for (auto& kv : im.hostBinds) {
		HostBinding& b = kv.second;
		switch (b.type) {
			case BindType::Int64:  b.scratch = (double)*(const int64_t*)b.src; break;
			case BindType::Float:  b.scratch = (double)*(const float*  )b.src; break;
			case BindType::Double: b.scratch = (double)*(const double* )b.src; break;
			case BindType::Bool:   b.scratch = (*(const bool*)b.src) ? 1.0 : 0.0; break;
		}
	}
	// Project host-bound resolution arrays into the 24 scratch doubles per
	// array (8 entries x 3 fields). The compile pass has already pushed
	// te_variable entries pointing at these slots.
	for (auto& kv : im.hostResArrays) {
		HostResolutionArrayBinding& rb = *kv.second;
		if (!rb.src) {
			for (size_t i = 0; i < 24; ++i) rb.scratch[i] = 0.0;
			continue;
		}
		for (size_t i = 0; i < 8; ++i) {
			rb.scratch[i * 3 + 0] = (double)rb.src[i].width;
			rb.scratch[i * 3 + 1] = (double)rb.src[i].height;
			rb.scratch[i * 3 + 2] = (double)rb.src[i].calls;
		}
	}
	// Dims mirrors reflect last value written by callback.
	for (auto& kv : im.dimsVars) {
		*kv.second.xMirror = (double)kv.second.dims->x;
		*kv.second.yMirror = (double)kv.second.dims->y;
	}
}

// ===========================================================================
// Condition evaluator: handles or / and / comparison, delegating arithmetic
// pieces to a tiny re-compile path. To keep things cheap we compile each
// sub-arithmetic the first time it's seen and cache it.
//
// Grammar (low to high precedence):
//   E   ::= A ( ('or')  A )*
//   A   ::= C ( ('and') C )*
//   C   ::= arith ( ('==' | '!=' | '>=' | '<=' | '>' | '<') arith )?
//
// arith is "everything up to the next top-level or/and/cmp boundary".
// ===========================================================================

struct CondCache {
	std::unordered_map<std::string, te_expr*>* arithMap;
	Document::Impl* im;
	// Optional extra variables that should be visible *only* for this
	// evaluation, e.g. the `x` sample-value variable for a single row of
	// a GRAPH_CONDITIONS list. When set, arith compilation goes through a
	// non-cached path because the same source could mean different things
	// depending on caller `x` binding -- and the resulting te_expr* would
	// be invalidated as soon as the caller's stack frame returns.
	const te_variable* extraVars  = nullptr;
	int                extraCount = 0;
	// When extras are present, ad-hoc compiled exprs are appended here
	// and freed by the caller right after EvalCondition returns.
	std::vector<te_expr*>* extraOwned = nullptr;
};

// Compile an arithmetic sub-expression, caching by source. Caller still
// owns the te_expr* via Impl.ownedExprs.
static te_expr* ArithCompile(CondCache& cc, const std::string& src) {
	if (cc.extraVars && cc.extraCount > 0) {
		// Non-cached path: build a private var table = extras + globals.
		std::vector<te_variable> vt;
		vt.reserve((size_t)cc.extraCount + cc.im->teVarTable.size());
		for (int i = 0; i < cc.extraCount; ++i) vt.push_back(cc.extraVars[i]);
		for (auto& v : cc.im->teVarTable) vt.push_back(v);
		int err = 0;
		te_expr* e = te_compile(src.c_str(), vt.data(), (int)vt.size(), &err);
		if (e && cc.extraOwned) cc.extraOwned->push_back(e);
		return e;
	}
	auto it = cc.arithMap->find(src);
	if (it != cc.arithMap->end()) return it->second;
	int err = 0;
	te_expr* e = te_compile(src.c_str(),
							cc.im->teVarTable.data(),
							(int)cc.im->teVarTable.size(),
							&err);
	(*cc.arithMap)[src] = e;
	if (e) cc.im->ownedExprs.push_back(e);
	return e;
}

// Tokenise a condition into top-level tokens: arithmetic chunks and keywords.
// Returns a flat list. Each element is one of:
//   "or", "and", "==", "!=", ">=", "<=", ">", "<", or an arithmetic chunk.
// We respect "(", "[", "{" nesting and "..." strings (though conditions
// shouldn't contain strings).
static std::vector<std::string> TokenizeCondition(const std::string& s) {
	std::vector<std::string> out;
	size_t i = 0, n = s.size();
	auto flush = [&](std::string& cur) {
		std::string t = Trim(cur);
		if (!t.empty()) out.push_back(std::move(t));
		cur.clear();
	};
	std::string cur;
	int depth = 0;
	bool inStr = false;
	while (i < n) {
		char c = s[i];
		if (inStr) {
			cur.push_back(c);
			if (c == '\\' && i + 1 < n) { cur.push_back(s[++i]); ++i; continue; }
			if (c == '"') inStr = false;
			++i; continue;
		}
		if (c == '"') { inStr = true; cur.push_back(c); ++i; continue; }
		if (c == '(' || c == '[' || c == '{') { ++depth; cur.push_back(c); ++i; continue; }
		if (c == ')' || c == ']' || c == '}') { --depth; cur.push_back(c); ++i; continue; }
		if (depth == 0) {
			// 2-char comparison ops
			if (i + 1 < n) {
				std::string two = s.substr(i, 2);
				if (two == "==" || two == "!=" || two == ">=" || two == "<=") {
					flush(cur); out.push_back(two); i += 2; continue;
				}
			}
			// 1-char comparison ops (but not '<' that's part of '<<' -- not used here)
			if (c == '<' || c == '>') {
				flush(cur); out.push_back(std::string(1, c)); ++i; continue;
			}
			// Keywords "and" / "or" -- match as whole words.
			auto isWord = [](char ch){
				return std::isalnum((unsigned char)ch) || ch == '_';
			};
			if ((c == 'a' || c == 'A') && i + 3 <= n) {
				if (std::tolower((unsigned char)s[i]) == 'a'
				 && std::tolower((unsigned char)s[i+1]) == 'n'
				 && std::tolower((unsigned char)s[i+2]) == 'd'
				 && (i == 0 || !isWord(s[i-1]))
				 && (i+3 == n || !isWord(s[i+3]))) {
					flush(cur); out.push_back("and"); i += 3; continue;
				}
			}
			if ((c == 'o' || c == 'O') && i + 2 <= n) {
				if (std::tolower((unsigned char)s[i]) == 'o'
				 && std::tolower((unsigned char)s[i+1]) == 'r'
				 && (i == 0 || !isWord(s[i-1]))
				 && (i+2 == n || !isWord(s[i+2]))) {
					flush(cur); out.push_back("or"); i += 2; continue;
				}
			}
		}
		cur.push_back(c);
		++i;
	}
	flush(cur);
	return out;
}

static double EvalArith(CondCache& cc, const std::string& src,
						Document::Impl& im) {
	if (Trim(src).empty()) return 0.0;
	// Literal-only fast path: pure number.
	char* endp = nullptr;
	double v = std::strtod(src.c_str(), &endp);
	if (endp && endp != src.c_str()) {
		// Make sure nothing trails (allow trailing whitespace).
		while (std::isspace((unsigned char)*endp)) ++endp;
		if (*endp == '\0') return v;
	}
	te_expr* e = ArithCompile(cc, src);
	if (!e) {
		im.lastError = "condition arith compile failed: " + src;
		return 0.0;
	}
	return te_eval(e);
}

static bool IsTruthy(double v) {
	// Anything not exactly zero (and not NaN). NaN -> false.
	return v == v && v != 0.0;
}

static bool EvalCondition(CondCache& cc, const std::string& cond,
						  Document::Impl& im) {
	if (Trim(cond).empty()) return true;
	auto toks = TokenizeCondition(cond);

	// Split by top-level "or" -> ORs of ANDs of comparisons.
	std::vector<std::vector<std::string>> orGroups;
	orGroups.emplace_back();
	for (auto& t : toks) {
		if (t == "or") orGroups.emplace_back();
		else           orGroups.back().push_back(t);
	}

	for (auto& orGrp : orGroups) {
		// Split this group by "and"
		std::vector<std::vector<std::string>> andGroups;
		andGroups.emplace_back();
		for (auto& t : orGrp) {
			if (t == "and") andGroups.emplace_back();
			else            andGroups.back().push_back(t);
		}
		bool andResult = true;
		for (auto& cmp : andGroups) {
			if (cmp.empty()) { andResult = false; break; }
			// cmp may contain a single comparison op or none.
			// Look for the comparison operator at any position.
			size_t opPos = (size_t)-1;
			std::string op;
			for (size_t i = 0; i < cmp.size(); ++i) {
				const auto& t = cmp[i];
				if (t=="==" || t=="!=" || t==">=" || t=="<=" || t==">" || t=="<") {
					opPos = i; op = t; break;
				}
			}
			bool oneResult;
			if (opPos == (size_t)-1) {
				// Pure arith truthiness.
				std::string joined;
				for (size_t i = 0; i < cmp.size(); ++i) {
					if (i) joined.push_back(' ');
					joined += cmp[i];
				}
				oneResult = IsTruthy(EvalArith(cc, joined, im));
			} else {
				std::string lhs, rhs;
				for (size_t i = 0; i < opPos; ++i)
					{ if (i) lhs.push_back(' '); lhs += cmp[i]; }
				for (size_t i = opPos+1; i < cmp.size(); ++i)
					{ if (i != opPos+1) rhs.push_back(' '); rhs += cmp[i]; }
				// String-equality path. We take it only for `==` / `!=`
				// and only when at least one side resolves to a string
				// (literal in quotes or known string-typed identifier).
				// Anything mixed string-vs-numeric is treated as not-equal
				// (so `$stringvar == 5` is false, `!= 5` is true) -- this
				// matches the forgiving behaviour of templates that work
				// across heterogeneous list types.
				auto resolveAsString = [&](const std::string& sideRaw,
										   std::string& outStr) -> bool {
					std::string side = Trim(sideRaw);
					if (side.size() >= 2
					 && side.front() == '"' && side.back() == '"') {
						outStr = Unescape(side.substr(1, side.size() - 2));
						return true;
					}
					// Bare identifier referencing a string-valued name?
					// Allowed sources, in priority: stringVars (script-side
					// VAR string + active loop var string), then config
					// String. Host-bound strings also land in stringVars
					// (handled by ReseedConfigAndHostBindings each frame).
					if (!side.empty()
					 && (std::isalpha((unsigned char)side.front())
						 || side.front() == '_')) {
						bool allId = true;
						for (char ch : side) {
							if (!(std::isalnum((unsigned char)ch)
								  || ch == '_' || ch == '.')) {
								allId = false; break;
							}
						}
						if (allId) {
							auto sit = im.stringVars.find(side);
							if (sit != im.stringVars.end()) {
								outStr = sit->second;
								return true;
							}
							auto cit = im.config.find(side);
							if (cit != im.config.end()
							 && cit->second.kind == ConfigKind::String) {
								outStr = cit->second.stringVal;
								return true;
							}
						}
					}
					return false;
				};
				if (op == "==" || op == "!=") {
					std::string sL, sR;
					bool isL = resolveAsString(lhs, sL);
					bool isR = resolveAsString(rhs, sR);
					if (isL && isR) {
						bool eq = (sL == sR);
						oneResult = (op == "==") ? eq : !eq;
						if (!oneResult) { andResult = false; break; }
						continue;
					}
					if (isL != isR) {
						// One string, one not -- always unequal.
						oneResult = (op == "!=");
						if (!oneResult) { andResult = false; break; }
						continue;
					}
					// Both numeric: fall through to arith path.
				}
				double L = EvalArith(cc, lhs, im);
				double R = EvalArith(cc, rhs, im);
				if      (op=="==") oneResult = (L == R);
				else if (op=="!=") oneResult = (L != R);
				else if (op==">=") oneResult = (L >= R);
				else if (op=="<=") oneResult = (L <= R);
				else if (op==">")  oneResult = (L >  R);
				else               oneResult = (L <  R); // "<"
			}
			if (!oneResult) { andResult = false; break; }
		}
		if (andResult) return true;
	}
	return false;
}

// ===========================================================================
// printf-style formatter that takes doubles + strings (parallel arrays
// indexed by argument position) and converts per spec.
// ===========================================================================
//
// We walk the format string. On each '%', we read the spec (flags, width,
// precision, length, conversion) and call snprintf with one of:
//   - long long          (for d, i)
//   - unsigned long long (for u, o, x, X)
//   - double             (for f, F, e, E, g, G, a, A)
//   - int                (for c)
//   - const char*        (for s, S) -- pulled from `strs[i]`
// %% emits a literal %. Arg index is shared between `vals` and `strs`;
// only one of the two is meaningful at each position depending on whether
// the spec is numeric or %s.

static std::string FormatPrintf(const std::string& fmt,
								const std::vector<double>& vals,
								const std::vector<std::string>& strs) {
	std::string out;
	out.reserve(fmt.size() + 32);
	size_t ai = 0;
	for (size_t i = 0; i < fmt.size(); ) {
		char c = fmt[i];
		if (c != '%') { out.push_back(c); ++i; continue; }

		// Parse a single specifier into a local buffer.
		char spec[64];
		size_t sp = 0;
		spec[sp++] = '%';
		++i;
		if (i < fmt.size() && fmt[i] == '%') {
			out.push_back('%'); ++i; continue;
		}
		// flags
		while (i < fmt.size()
			&& (fmt[i]=='-'||fmt[i]=='+'||fmt[i]==' '
				||fmt[i]=='#'||fmt[i]=='0'))
			{ if (sp+1<sizeof(spec)) spec[sp++]=fmt[i]; ++i; }
		// width
		while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
			{ if (sp+1<sizeof(spec)) spec[sp++]=fmt[i]; ++i; }
		// precision
		if (i < fmt.size() && fmt[i] == '.') {
			if (sp+1<sizeof(spec)) spec[sp++]='.';
			++i;
			while (i < fmt.size() && std::isdigit((unsigned char)fmt[i]))
				{ if (sp+1<sizeof(spec)) spec[sp++]=fmt[i]; ++i; }
		}
		// length (h, hh, l, ll, z, j, t, L) -- we'll override these below.
		while (i < fmt.size()
			&& (fmt[i]=='h'||fmt[i]=='l'||fmt[i]=='z'
				||fmt[i]=='j'||fmt[i]=='t'||fmt[i]=='L'))
			{ ++i; }   // skip; we re-pick the length to match our value type
		if (i >= fmt.size()) { out.append(spec, sp); break; }
		char conv = fmt[i++];

		// Append correct length modifier and the conversion.
		// For %s we need a heap buffer because the formatted result can
		// exceed any small stack buffer (long strings + width modifiers).
		switch (conv) {
			case 'd': case 'i': {
				char buf[128];
				if (sp + 4 < sizeof(spec)) {
					spec[sp++] = 'l'; spec[sp++] = 'l'; spec[sp++] = conv; spec[sp] = 0;
				}
				long long v = (ai < vals.size()) ? (long long)vals[ai] : 0;
				++ai;
				int n = std::snprintf(buf, sizeof(buf), spec, v);
				if (n > 0) out.append(buf, (size_t)n);
				break;
			}
			case 'u': case 'o': case 'x': case 'X': {
				char buf[128];
				if (sp + 4 < sizeof(spec)) {
					spec[sp++] = 'l'; spec[sp++] = 'l'; spec[sp++] = conv; spec[sp] = 0;
				}
				unsigned long long v = (ai < vals.size())
									   ? (unsigned long long)(long long)vals[ai]
									   : 0ULL;
				++ai;
				int n = std::snprintf(buf, sizeof(buf), spec, v);
				if (n > 0) out.append(buf, (size_t)n);
				break;
			}
			case 'f': case 'F': case 'e': case 'E':
			case 'g': case 'G': case 'a': case 'A': {
				char buf[128];
				if (sp + 1 < sizeof(spec)) { spec[sp++] = conv; spec[sp] = 0; }
				double v = (ai < vals.size()) ? vals[ai] : 0.0;
				++ai;
				int n = std::snprintf(buf, sizeof(buf), spec, v);
				if (n > 0) out.append(buf, (size_t)n);
				break;
			}
			case 'c': {
				char buf[128];
				if (sp + 1 < sizeof(spec)) { spec[sp++] = conv; spec[sp] = 0; }
				int v = (ai < vals.size()) ? (int)vals[ai] : 0;
				++ai;
				int n = std::snprintf(buf, sizeof(buf), spec, v);
				if (n > 0) out.append(buf, (size_t)n);
				break;
			}
			case 's': case 'S': {
				if (sp + 1 < sizeof(spec)) { spec[sp++] = 's'; spec[sp] = 0; }
				const std::string& v = (ai < strs.size()) ? strs[ai] : std::string();
				++ai;
				// Two-step snprintf to size the output exactly.
				int n = std::snprintf(nullptr, 0, spec, v.c_str());
				if (n > 0) {
					size_t base = out.size();
					out.resize(base + (size_t)n + 1);
					std::snprintf(&out[base], (size_t)n + 1, spec, v.c_str());
					out.resize(base + (size_t)n);
				}
				break;
			}
			default: {
				// Unknown; emit raw specifier text.
				char buf[128];
				if (sp + 1 < sizeof(spec)) { spec[sp++] = conv; spec[sp] = 0; }
				int n = std::snprintf(buf, sizeof(buf), "%s", spec);
				if (n > 0) out.append(buf, (size_t)n);
				break;
			}
		}
	}
	return out;
}

// Forward decl: needed because ResolveStringArg recurses through MaterializeText
// when a %s slot resolves to a config Format value.
static std::string MaterializeText(Document::Impl& im,
								   bool isLit,
								   const std::string& strOrKey,
								   int depth);

// Forward decl: EvaluateFormatArg uses EvalStringExpr which is defined
// further below (it itself recurses through MaterializeText).
static std::string EvalStringExpr(Document::Impl& im, const StringExpr& se);

// Materialise a "text source" -- either an inline literal or a bare
// identifier reference -- into the final UTF-8 string using current
// variable values. The identifier resolves in the same priority order
// as ResolveStringArg: private string VAR > config String > config
// Format > host string binding > echo the identifier verbatim.
// Forward decl for the condition evaluator -- defined further below.
struct CondCache;
static bool EvalCondition(CondCache& cc, const std::string& cond,
						  Document::Impl& im);

// Helper: render a single positional FormatArg slot into (out_d, out_s).
// `isStrSlot` says whether the %-specifier consumes a string or a number;
// for string slots we fill out_s and out_d is 0, and vice-versa.
//
// For ternary args, we evaluate the cond and recurse into the chosen
// branch. For nested-format args, we materialise the nested format with
// the current `depth` propagated. For string-expression args, we evaluate
// the StringExpr (it might reference private string VARs, config strings,
// or host string bindings).
//
// `cc` is an optional thread-local CondCache for the calling Evaluate().
// If null, ternary conds get a one-off cache (slower but always correct).
static void EvaluateFormatArg(Document::Impl& im, FormatArg* a, bool isStrSlot,
							  int depth,
							  double& out_d, std::string& out_s,
							  CondCache* cc = nullptr);

static void EvaluateFormatArg(Document::Impl& im, FormatArg* a, bool isStrSlot,
							  int depth,
							  double& out_d, std::string& out_s,
							  CondCache* cc) {
	out_d = 0.0;
	out_s.clear();
	if (!a) return;
	if (depth > 8) return;
	switch (a->kind) {
		case FormatArgKind::Numeric: {
			double v = a->numericCompiled ? te_eval(a->numericCompiled) : 0.0;
			if (isStrSlot) {
				char tmp[32];
				std::snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
				out_s = tmp;
			} else {
				out_d = v;
			}
			return;
		}
		case FormatArgKind::StringExpr_: {
			if (isStrSlot) {
				out_s = EvalStringExpr(im, a->strExpr);
			} else {
				std::string s = EvalStringExpr(im, a->strExpr);
				out_d = std::strtod(s.c_str(), nullptr);
			}
			return;
		}
		case FormatArgKind::NestedFormat: {
			if (!a->nested) return;
			const FormatSpec& f = *a->nested;
			std::vector<double>      vals;
			std::vector<std::string> strs;
			vals.reserve(f.argTernaries.size());
			strs.reserve(f.argTernaries.size());
			for (size_t i = 0; i < f.argTernaries.size(); ++i) {
				bool nIsStr = i < f.argIsString.size() && f.argIsString[i];
				double dv; std::string sv;
				EvaluateFormatArg(im, (FormatArg*)f.argTernaries[i],
								  nIsStr, depth + 1, dv, sv, cc);
				vals.push_back(dv);
				strs.push_back(sv);
			}
			std::string rendered = FormatPrintf(f.fmt, vals, strs);
			if (isStrSlot) out_s = std::move(rendered);
			else           out_d = std::strtod(rendered.c_str(), nullptr);
			return;
		}
		case FormatArgKind::Ternary: {
			bool truth = false;
			if (cc) {
				truth = EvalCondition(*cc, a->condSource, im);
			} else {
				// Caller didn't supply a CondCache. Use the Document's
				// persistent arithCache so condition te_expr trees are
				// compiled once and reused across frames -- a fresh
				// per-call cache would push a NEW te_expr into ownedExprs
				// every time MaterializeText / EvalStringExpr enters a
				// ternary, leaking memory at a steady rate (~2 te_exprs
				// per ternary per frame).
				CondCache local;
				local.arithMap = &im.arithCache;
				local.im       = &im;
				truth = EvalCondition(local, a->condSource, im);
			}
			FormatArg* chosen = truth ? a->thenArg : a->elseArg;
			EvaluateFormatArg(im, chosen, isStrSlot, depth + 1, out_d, out_s, cc);
			return;
		}
	}
}

static std::string MaterializeText(Document::Impl& im,
								   bool isLit,
								   const std::string& strOrKey,
								   int depth) {
	if (isLit) return strOrKey;
	if (depth > 8) return std::string();

	auto vit = im.stringVars.find(strOrKey);
	if (vit != im.stringVars.end()) return vit->second;

	auto it = im.config.find(strOrKey);
	if (it != im.config.end()) {
		const ConfigValue& cv = it->second;
		if (cv.kind == ConfigKind::String) return cv.stringVal;
		if (cv.kind == ConfigKind::Format) {
			const FormatSpec& f = cv.fmtVal;
			std::vector<double>      vals;
			std::vector<std::string> strs;
			vals.reserve(f.argTernaries.size());
			strs.reserve(f.argTernaries.size());
			for (size_t i = 0; i < f.argTernaries.size(); ++i) {
				bool isStr = i < f.argIsString.size() && f.argIsString[i];
				double dv; std::string sv;
				EvaluateFormatArg(im, (FormatArg*)f.argTernaries[i],
								  isStr, depth + 1, dv, sv);
				vals.push_back(dv);
				strs.push_back(sv);
			}
			return FormatPrintf(f.fmt, vals, strs);
		}
		// Integer/Bool config used in place of a string? Stringify.
		char tmp[32];
		std::snprintf(tmp, sizeof(tmp), "%lld", (long long)cv.intVal);
		return tmp;
	}

	auto hit = im.hostStringBinds.find(strOrKey);
	if (hit != im.hostStringBinds.end())
		return hit->second.src ? *hit->second.src : std::string();

	return strOrKey;  // unknown identifier -> echo verbatim
}

// Convenience overload for callers that don't need a depth hint.
static inline std::string MaterializeText(Document::Impl& im,
										  bool isLit,
										  const std::string& strOrKey) {
	return MaterializeText(im, isLit, strOrKey, 0);
}

// Evaluate a string expression (sequence of literal and identifier segments
// joined by `+`) to a single string. Identifier segments resolve via the
// same chain as MaterializeText:
//   private string VAR > config String > config Format >
//   host string binding > numeric VAR / binding (stringified) > echo.
// Numeric identifiers used in a string expression are stringified as
// integers; this lets `"x=" + COMMON_MARGIN` produce something sensible.
static std::string EvalStringExpr(Document::Impl& im, const StringExpr& se) {
	std::string out;
	for (const auto& seg : se.segments) {
		if (seg.isLiteral) {
			out += seg.text;
			continue;
		}
		if (seg.nestedFmt) {
			// Materialise the inline `{"fmt", args...}` segment using the
			// same machinery as a standalone Format-typed config value.
			const FormatSpec& f = *(const FormatSpec*)seg.nestedFmt;
			std::vector<double>      vals;
			std::vector<std::string> strs;
			vals.reserve(f.argTernaries.size());
			strs.reserve(f.argTernaries.size());
			for (size_t i = 0; i < f.argTernaries.size(); ++i) {
				bool nIsStr = i < f.argIsString.size() && f.argIsString[i];
				double dv; std::string sv;
				EvaluateFormatArg(im, (FormatArg*)f.argTernaries[i],
								  nIsStr, /*depth=*/0, dv, sv, /*cc=*/nullptr);
				vals.push_back(dv);
				strs.push_back(sv);
			}
			out += FormatPrintf(f.fmt, vals, strs);
			continue;
		}
		// 1. private string VAR
		if (auto vit = im.stringVars.find(seg.text); vit != im.stringVars.end()) {
			out += vit->second; continue;
		}
		// 2/3. config (String / Format / Integer / Bool)
		if (auto cit = im.config.find(seg.text); cit != im.config.end()) {
			const ConfigValue& cv = cit->second;
			if (cv.kind == ConfigKind::String) { out += cv.stringVal; continue; }
			if (cv.kind == ConfigKind::Format) {
				out += MaterializeText(im, /*isLit=*/false, seg.text);
				continue;
			}
			// Integer / Bool: stringify.
			char tmp[32];
			std::snprintf(tmp, sizeof(tmp), "%lld", (long long)cv.intVal);
			out += tmp;
			continue;
		}
		// 4. host string binding
		if (auto hit = im.hostStringBinds.find(seg.text); hit != im.hostStringBinds.end()) {
			if (hit->second.src) out += *hit->second.src;
			continue;
		}
		// 5. numeric private VAR (stringify)
		if (auto pit = im.privateVars.find(seg.text); pit != im.privateVars.end()) {
			char tmp[32];
			std::snprintf(tmp, sizeof(tmp), "%lld", (long long)*pit->second);
			out += tmp;
			continue;
		}
		// 6. numeric host binding (stringify via scratch)
		if (auto hb = im.hostBinds.find(seg.text); hb != im.hostBinds.end()) {
			char tmp[32];
			std::snprintf(tmp, sizeof(tmp), "%lld", (long long)hb->second.scratch);
			out += tmp;
			continue;
		}
		// Last resort: emit the name verbatim so the user notices.
		out += seg.text;
	}
	return out;
}

// ===========================================================================
// Evaluate
// ===========================================================================

// Helper exposed via GraphConditionView::matches. The host calls this to
// ask "does this row's condition fire for sample value X?". We stash the
// value into the row's per-row scratch double (which `x` resolves to in
// the row's source) and evaluate the cond through EvalCondition (because
// the cond may contain comparison operators that tinyexpr can't handle
// on its own).
struct GraphMatchState {
	Document::Impl*  im;
	GraphCondParsed* row;
};
static bool GraphConditionMatches(void* state, double sampleValue) {
	auto* gs = (GraphMatchState*)state;
	if (!gs || !gs->row || !gs->im) return false;
	gs->row->xScratch = sampleValue;
	// Build a one-off CondCache with `x` exposed via extraVars.
	std::unordered_map<std::string, te_expr*> arithCache;
	std::vector<te_expr*> extraOwned;
	te_variable xVar = { "x", &gs->row->xScratch, 0, nullptr };
	CondCache cc;
	cc.arithMap   = &arithCache;
	cc.im         = gs->im;
	cc.extraVars  = &xVar;
	cc.extraCount = 1;
	cc.extraOwned = &extraOwned;
	bool truth = EvalCondition(cc, gs->row->condSource, *gs->im);
	// Free any ad-hoc arith exprs compiled for this single eval.
	for (te_expr* e : extraOwned) if (e) te_free(e);
	return truth;
}

bool Document::Evaluate(Callback cb, void* user) {
	if (!cb) { m_impl->lastError = "no callback"; return false; }
	// When frozen, replay the script using the exact same host values and
	// cached string state as the previous frame. This is used for a DryRun
	// that should produce identical geometry to the real pass without
	// re-pulling live data (e.g. for clamping before RecordCallback).
	if (!m_impl->m_frozen)
		RefreshScratches(*m_impl);
	m_impl->m_frozen = false;

	// Per-Document persistent cache for ad-hoc arithmetic chunks built by
	// EvalCondition. The cache is owned by Impl so the te_expr trees
	// survive across frames; otherwise EvalCondition would recompile (and
	// push another te_expr into ownedExprs) every frame for every
	// condition, growing memory without bound.
	CondCache cc{ &m_impl->arithCache, m_impl };

	auto evalExpr = [](const CExpr& e) -> double {
		return e.compiled ? te_eval(e.compiled) : 0.0;
	};

	std::function<bool(std::vector<AstNode>&)> walk = [&](std::vector<AstNode>& v) -> bool {
		for (auto& n : v) {
			switch (n.kind) {
				case NodeKind::If: {
					bool truth = EvalCondition(cc, n.condSource, *m_impl);
					if (!walk(truth ? n.thenBranch : n.elseBranch)) return false;
					break;
				}
				case NodeKind::For: {
					// Iterate the referenced list, writing the current
					// element into the loop var's storage each iteration.
					auto cit = m_impl->config.find(n.listName);
					if (cit == m_impl->config.end()
					 || cit->second.kind != ConfigKind::List) {
						m_impl->lastError = "#for: '" + n.listName
							+ "' is not a LIST at evaluate time";
						return false;
					}
					const ListValue& lv = cit->second.listVal;
					// Pre-resolve the storage slot once -- it's stable across
					// iterations (the privateVars / stringVars maps are not
					// mutated during evaluation).
					if (n.loopVarType == ListElemType::String) {
						auto sit = m_impl->stringVars.find(n.loopVarName);
						if (sit == m_impl->stringVars.end()) {
							// Defensive: classify should have created it.
							m_impl->stringVars[n.loopVarName];
							sit = m_impl->stringVars.find(n.loopVarName);
						}
						for (const auto& s : lv.strings) {
							sit->second = s;
							if (!walk(n.body)) return false;
						}
					} else {
						auto pit = m_impl->privateVars.find(n.loopVarName);
						if (pit == m_impl->privateVars.end()) {
							m_impl->privateVars[n.loopVarName]
								.reset(new double(0));
							pit = m_impl->privateVars.find(n.loopVarName);
						}
						double* slot = pit->second.get();
						if (n.loopVarType == ListElemType::Int) {
							for (int64_t v : lv.ints) {
								*slot = (double)v;
								if (!walk(n.body)) return false;
							}
						} else {
							// Float / Double both stored as doubles.
							for (double v : lv.doubles) {
								*slot = v;
								if (!walk(n.body)) return false;
							}
						}
					}
					break;
				}
				case NodeKind::Var: {
					if (n.varIsStruct) {
						// Copy width/height/calls from the source resolution
						// slot into the three private destination doubles.
						// The te_variable table already has all six names
						// bound to stable double pointers, so EnsureVariable
						// hands them back without allocating.
						double* sw = EnsureVariable(*m_impl, n.varSrcWidthName);
						double* sh = EnsureVariable(*m_impl, n.varSrcHeightName);
						double* sc = EnsureVariable(*m_impl, n.varSrcCallsName);
						auto dw = m_impl->privateVars.find(n.varName + "_width");
						auto dh = m_impl->privateVars.find(n.varName + "_height");
						auto dc = m_impl->privateVars.find(n.varName + "_calls");
						if (sw && dw != m_impl->privateVars.end()) *dw->second = *sw;
						if (sh && dh != m_impl->privateVars.end()) *dh->second = *sh;
						if (sc && dc != m_impl->privateVars.end()) *dc->second = *sc;
						break;
					}
					if (n.varIsString) {
						auto it = m_impl->stringVars.find(n.varName);
						if (it != m_impl->stringVars.end())
							it->second = EvalStringExpr(*m_impl, n.stringRhs);
					} else {
						auto it = m_impl->privateVars.find(n.varName);
						if (it != m_impl->privateVars.end()) {
							if (n.varIsTernary) {
								bool truth = EvalCondition(cc, n.varCondSource,
														   *m_impl);
								*it->second = evalExpr(truth ? n.eVarThen
															  : n.eVarElse);
							} else {
								*it->second = evalExpr(n.eVarValue);
							}
						}
					}
					break;
				}
				case NodeKind::Text: {
					RenderCommand cmd{};
					cmd.type     = RenderCmdType::Text;
					cmd.x        = (int64_t)evalExpr(n.eX);
					cmd.y        = (int64_t)evalExpr(n.eY);
					cmd.fontSize = (int64_t)evalExpr(n.eFontSize);
					cmd.color    = (uint16_t)(int64_t)evalExpr(n.eColor);
					cmd.text     = n.textIsInlineFormat
						? EvalStringExpr(*m_impl, n.textInlineExpr)
						: MaterializeText(*m_impl, n.textIsLiteral, n.textStrOrKey);
					// Strip a trailing newline so the renderer doesn't measure
					// a phantom empty line below the last visible line of text.
					// This matters for bottom/right clamping (e.g. Mini.smd),
					// where an extra \n inflates the reported height and causes
					// the widget to land a few pixels short of the edge.
					if (!cmd.text.empty() && cmd.text.back() == '\n')
						cmd.text.pop_back();
					if (!cmd.text.empty() && cmd.text.back() == '\r')
						cmd.text.pop_back();
					cb(cmd, user);
					break;
				}
				case NodeKind::Box: {
					RenderCommand cmd{};
					cmd.type   = RenderCmdType::Box;
					cmd.x      = (int64_t)evalExpr(n.eX);
					cmd.y      = (int64_t)evalExpr(n.eY);
					cmd.width  = (int64_t)evalExpr(n.eWidth);
					cmd.height = (int64_t)evalExpr(n.eHeight);
					cmd.color  = (uint16_t)(int64_t)evalExpr(n.eColorBox);
					cb(cmd, user);
					break;
				}
				case NodeKind::RoundedBox: {
					RenderCommand cmd{};
					cmd.type   = RenderCmdType::RoundedBox;
					cmd.x      = (int64_t)evalExpr(n.eX);
					cmd.y      = (int64_t)evalExpr(n.eY);
					cmd.width  = (int64_t)evalExpr(n.eWidth);
					cmd.height = (int64_t)evalExpr(n.eHeight);
					cmd.roundnessTl = evalExpr(n.eRoundnessTL);
					cmd.roundnessTr = evalExpr(n.eRoundnessTR);
					cmd.roundnessBl = evalExpr(n.eRoundnessBL);
					cmd.roundnessBr = evalExpr(n.eRoundnessBR);
					cmd.color  = (uint16_t)(int64_t)evalExpr(n.eColorBox);
					cb(cmd, user);
					break;
				}
				case NodeKind::EmptyBox: {
					RenderCommand cmd{};
					cmd.type   = RenderCmdType::EmptyBox;
					cmd.x      = (int64_t)evalExpr(n.eX);
					cmd.y      = (int64_t)evalExpr(n.eY);
					cmd.width  = (int64_t)evalExpr(n.eWidth);
					cmd.height = (int64_t)evalExpr(n.eHeight);
					cmd.color  = (uint16_t)(int64_t)evalExpr(n.eColorBox);
					cb(cmd, user);
					break;
				}
				case NodeKind::DashedLine: {
					RenderCommand cmd{};
					cmd.type       = RenderCmdType::DashedLine;
					cmd.x          = (int64_t)evalExpr(n.eX);
					cmd.y          = (int64_t)evalExpr(n.eY);
					cmd.x2         = (int64_t)evalExpr(n.eX2);
					cmd.y2         = (int64_t)evalExpr(n.eY2);
					cmd.dashOn     = (int64_t)evalExpr(n.eDashOn);
					cmd.dashOff    = (int64_t)evalExpr(n.eDashOff);
					cmd.color      = (uint16_t)(int64_t)evalExpr(n.eColorBox);
					cb(cmd, user);
					break;
				}
				case NodeKind::HistoryUpdate: {
					auto it = m_impl->histories.find(n.historyTarget);
					if (it != m_impl->histories.end()) {
						HistoryDecl& h = it->second;
						// Evaluate the expression once in double precision;
						// every history kind takes its sample value from here.
						double dv = evalExpr(n.eHistoryValue);
						// Drop oldest if at capacity. erase-begin is O(n) but
						// capacities are small (typically <= a few hundred)
						// and the host wants stable sample[0] -> sample[n-1]
						// semantics. Behaviour is identical for all types.
						switch (h.type) {
							case HistoryType::Int:
								if (h.samplesI.size() >= h.capacity && h.capacity > 0)
									h.samplesI.erase(h.samplesI.begin());
								h.samplesI.push_back((int64_t)dv);
								break;
							case HistoryType::Float:
								if (h.samplesF.size() >= h.capacity && h.capacity > 0)
									h.samplesF.erase(h.samplesF.begin());
								h.samplesF.push_back((float)dv);
								break;
							case HistoryType::Double:
								if (h.samplesD.size() >= h.capacity && h.capacity > 0)
									h.samplesD.erase(h.samplesD.begin());
								h.samplesD.push_back(dv);
								break;
						}
#ifdef DEBUG
						// Surface as a render command so the host knows --
						// useful for instrumentation/debug; release builds
						// never see this. Header gates RenderCmdType::HistoryUpdate
						// and the historyValue field on the same macro.
						RenderCommand cmd{};
						cmd.type         = RenderCmdType::HistoryUpdate;
						cmd.historyName  = it->first.c_str();
						cmd.historyValue = dv;
						cb(cmd, user);
#else
						(void)cb; (void)user;
#endif
					}
					break;
				}
				case NodeKind::HistoryClean: {
					auto it = m_impl->histories.find(n.historyTarget);
					if (it != m_impl->histories.end()) {
						// Cheapest to clear all three; the inactive ones
						// are already empty so .clear() is a no-op on them.
						it->second.samplesI.clear();
						it->second.samplesF.clear();
						it->second.samplesD.clear();
#ifdef DEBUG
						RenderCommand cmd{};
						cmd.type        = RenderCmdType::HistoryClean;
						cmd.historyName = it->first.c_str();
						cb(cmd, user);
#else
						(void)cb; (void)user;
#endif
					}
					break;
				}
				case NodeKind::GraphLineChart: {
					auto hit = m_impl->histories.find(n.graphHistoryName);
					if (hit == m_impl->histories.end()) break;
					HistoryDecl& h = hit->second;

					std::vector<GraphConditionView> viewVec;
					std::vector<GraphMatchState>    stateVec;
					GraphCondDecl* gcd = nullptr;
					if (!n.graphCondsName.empty()) {
						auto git = m_impl->graphConds.find(n.graphCondsName);
						if (git != m_impl->graphConds.end()) gcd = &git->second;
					}
					if (gcd) {
						// Reserve up-front so addresses stay stable while
						// we hand state pointers out via views.
						stateVec.reserve(gcd->rows.size());
						viewVec.reserve(gcd->rows.size());
						for (auto& row : gcd->rows) {
							stateVec.push_back({ m_impl, &row });
						}
						for (size_t i = 0; i < gcd->rows.size(); ++i) {
							auto& row = gcd->rows[i];
							GraphConditionView v;
							v.lineColor = (int64_t)(row.lineCompiled
													? te_eval(row.lineCompiled) : 0);
							v.fillColor = (int64_t)(row.fillCompiled
													? te_eval(row.fillCompiled) : 0);
							v.matches   = &GraphConditionMatches;
							v.state     = &stateVec[i];
							viewVec.push_back(v);
						}
					}

					RenderCommand cmd{};
					cmd.type      = RenderCmdType::GraphLineChart;
					cmd.x         = (int64_t)evalExpr(n.eX);
					cmd.y         = (int64_t)evalExpr(n.eY);
					cmd.width     = (int64_t)evalExpr(n.eWidth);
					cmd.height    = (int64_t)evalExpr(n.eHeight);
					cmd.minClamp  = (int64_t)evalExpr(n.eMin);
					cmd.maxClamp  = (int64_t)evalExpr(n.eMax);
					cmd.direction = n.graphDir;
					cmd.color     = (uint16_t)(int64_t)evalExpr(n.eLineColor);
					cmd.fillColor = (uint16_t)(int64_t)evalExpr(n.eFillColor);
					// Sample pointers + count + type. Exactly one of
					// (samples, samplesD) is non-null; the other is nullptr.
					// The host reads via the sampleType discriminator.
					switch (h.type) {
						case HistoryType::Int:
							cmd.sampleType  = HistorySampleType::Int;
							cmd.samples     = h.samplesI.empty() ? nullptr : h.samplesI.data();
							cmd.samplesD    = nullptr;
							cmd.sampleCount = h.samplesI.size();
							break;
						case HistoryType::Float:
							// We promote each float to double on the fly into
							// a scratch buffer owned by the document so the
							// host receives a uniform `const double*` view
							// for both float and double histories. The
							// scratch survives until the next GraphLineChart
							// for this same history (or until Free()).
							{
								auto& scratch = m_impl->floatSampleScratch[hit->first];
								scratch.assign(h.samplesF.begin(), h.samplesF.end());
								cmd.sampleType  = HistorySampleType::Float;
								cmd.samples     = nullptr;
								cmd.samplesD    = scratch.empty() ? nullptr : scratch.data();
								cmd.sampleCount = scratch.size();
							}
							break;
						case HistoryType::Double:
							cmd.sampleType  = HistorySampleType::Double;
							cmd.samples     = nullptr;
							cmd.samplesD    = h.samplesD.empty() ? nullptr : h.samplesD.data();
							cmd.sampleCount = h.samplesD.size();
							break;
					}
					cmd.sampleCapacity = h.capacity;
					cmd.conditions     = viewVec.empty() ? nullptr : viewVec.data();
					cmd.condCount      = viewVec.size();
					cmd.historyName    = hit->first.c_str();
					cb(cmd, user);
					break;
				}
				case NodeKind::GetDimensions: {
					int64_t fs = (int64_t)evalExpr(n.eFontSize);
					std::string txt = n.textIsInlineFormat
						? EvalStringExpr(*m_impl, n.textInlineExpr)
						: MaterializeText(*m_impl, n.textIsLiteral, n.textStrOrKey);

					auto it = m_impl->dimsVars.find(n.dimsName);
					if (it == m_impl->dimsVars.end()) break;   // shouldn't happen

					// Cache key: text + 0x01 + fontSize. 0x01 is unlikely to
					// appear in a real font-renderable string, so the join is
					// unambiguous.
					std::string key;
					key.reserve(txt.size() + 16);
					key.append(txt);
					key.push_back('\x01');
					{
						char numBuf[32];
						std::snprintf(numBuf, sizeof(numBuf), "%lld", (long long)fs);
						key.append(numBuf);
					}

					auto cacheIt = m_impl->dimsMeasureCache.find(key);
					if (cacheIt != m_impl->dimsMeasureCache.end()) {
						// Cache hit: reuse measurement, skip callback.
						*it->second.dims = cacheIt->second;
					} else {
						// Cache miss: invoke host callback to measure.
						RenderCommand cmd{};
						cmd.type     = RenderCmdType::GetDimensions;
						cmd.fontSize = fs;
						cmd.text     = txt;
						cmd.outDims  = it->second.dims.get();
						cmd.dimsName = it->first.c_str();
						cb(cmd, user);
						// Store measurement for next time. Bounded: dynamic
						// text (e.g. live FPS strings) generates a unique
						// key every frame, so an unbounded cache grows
						// linearly with playtime. When we exceed a soft cap,
						// drop the entire cache rather than inserting; the
						// host pays a re-measure cost across the next
						// few frames but RSS stays flat. Static text
						// (labels) just gets re-inserted next frame.
						constexpr size_t kDimsCacheCap = 256;
						if (m_impl->dimsMeasureCache.size() >= kDimsCacheCap)
							m_impl->dimsMeasureCache.clear();
						m_impl->dimsMeasureCache[std::move(key)] = *it->second.dims;
					}
					// Refresh mirror doubles so subsequent expressions read
					// the freshly written .x / .y values.
					*it->second.xMirror = (double)it->second.dims->x;
					*it->second.yMirror = (double)it->second.dims->y;
					break;
				}
			}
		}
		return true;
	};
	bool ok = walk(m_impl->script);
	return ok;
}

void Document::Reset(bool freeze) {
	if (!m_impl) return;
	Impl& im = *m_impl;
	im.m_frozen = freeze;
	if (freeze) return;  // caller wants Evaluate() to replay with cached state
	// Pull host-bound values into scratch doubles BEFORE we re-seed string
	// VARs below. Otherwise MaterializeText on config Format defaults
	// (line 3741) evaluates format args against scratch state left over
	// from the previous frame, so any string VAR not reassigned inside an
	// active `#if` branch carries last frame's data. Evaluate() will
	// refresh again at its own entry; the second refresh is essentially
	// free (one pass over ~50 doubles).
	RefreshScratches(im);
	// For each private numeric VAR: if a config Integer/Bool with the same
	// name exists, re-seed the VAR with that value. State-kind entries
	// ('key = value') are intentionally skipped here -- they were seeded
	// once at Compile() and are owned by the script as cross-frame state.
	// Numeric VARs: only zero those that have NO matching config key at
	// all. Config-backed VARs (both `:` default-kind and `=` constant-
	// kind) are seeded once at Compile() and from then on carry whatever
	// value the script most recently wrote -- Reset doesn't clobber them.
	// This is what lets `lastFrame: 0` + `VAR{lastFrame, Game_LastFrameNumber_int}`
	// carry state across frames without the parser undoing it.
	for (auto& kv : im.privateVars) {
		if (!kv.second) continue;
		auto cit = im.config.find(kv.first);
		bool configBacked = cit != im.config.end()
			&& (cit->second.kind == ConfigKind::Integer
			 || cit->second.kind == ConfigKind::Bool);
		if (!configBacked) *kv.second = 0.0;
	}
	// String VARs: same policy. Config-backed entries carry their
	// last-written value; un-backed entries get cleared each frame.
	// Format-kind configs are special: they're treated as "always
	// re-evaluate" by design (the whole point of a Format is that it
	// pulls live host data), so we re-materialise them each Reset.
	for (auto& kv : im.stringVars) {
		auto cit = im.config.find(kv.first);
		if (cit == im.config.end()) {
			kv.second.clear();
			continue;
		}
		if (cit->second.kind == ConfigKind::Format) {
			kv.second = MaterializeText(im, /*isLit=*/false, kv.first);
		}
		// String / Integer / Bool configs: leave whatever the script (or
		// initial seed) most recently wrote.
	}
	// Zero every GET_DIMENSIONS struct + its mirror doubles, so expressions
	// that reference dims.x / dims.y see fresh state before the matching
	// GET_DIMENSIONS command in the next Evaluate() repopulates them.
	for (auto& kv : im.dimsVars) {
		if (kv.second.dims)    { kv.second.dims->x = 0; kv.second.dims->y = 0; }
		if (kv.second.xMirror) *kv.second.xMirror = 0.0;
		if (kv.second.yMirror) *kv.second.yMirror = 0.0;
	}
	// Note: hostBinds scratch, configMirror, implicitZero, and dimsMeasureCache
	// are intentionally NOT cleared. The first three either get refreshed at
	// the top of Evaluate or are immutable; the last is meant to persist.
}

void Document::ClearDimsMeasureCache() {
	if (m_impl) m_impl->dimsMeasureCache.clear();
}

// ===========================================================================
// Config accessors
// ===========================================================================

const ConfigValue* Document::GetConfig(const char* key) const {
	auto it = m_impl->config.find(key);
	return (it == m_impl->config.end()) ? nullptr : &it->second;
}
int64_t Document::GetConfigInt(const char* key, int64_t def) const {
	auto* c = GetConfig(key);
	return c ? c->intVal : def;
}
bool Document::GetConfigBool(const char* key, bool def) const {
	auto* c = GetConfig(key);
	if (!c) return def;
	if (c->kind == ConfigKind::Bool)    return c->boolVal;
	if (c->kind == ConfigKind::Integer) return c->intVal != 0;
	return def;
}
const char* Document::GetConfigString(const char* key, const char* def) const {
	auto* c = GetConfig(key);
	if (!c) return def;
	if (c->kind == ConfigKind::String) return c->stringVal.c_str();
	return def;
}
bool Document::FormatConfigString(const char* key, std::string& out) {
	auto it = m_impl->config.find(key);
	if (it == m_impl->config.end()) return false;
	RefreshScratches(*m_impl);
	out = MaterializeText(*m_impl, false, key);
	return true;
}

} // namespace smd

#pragma GCC pop_options