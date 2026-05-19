# `smd_parser` — Internal Documentation

A C++23 parser for `.smd` (Status Monitor Design) overlay files used by SaltyNX/Tesla Switch overlays. This document is written for a future Claude instance who needs to maintain, extend, or debug the parser. It assumes C++ fluency and skips Switch/Tesla background where irrelevant.

The parser is a single translation unit (`smd_parser.cpp`) plus a public header (`smd_parser.hpp`). It depends on a vendored, modified copy of `tinyexpr` for arithmetic-only expressions. The host uses it like this:

```cpp
smd::Document doc;
doc.LoadFromFile("Design.smd");        // parse to AST + config
doc.BindInt64("CPU_Hz_int", &cpu);     // attach live host pointers
// ... bind every predefined name referenced by the file ...
doc.Compile();                          // resolve names, compile expressions
for (;;) {                              // once per frame
    doc.Reset();                        // zero un-backed VARs, refresh Format strings
    doc.Evaluate(MyCallback, this);     // emit RenderCommands to the host
}
```

`Free()` (or the destructor) releases everything, including all `te_expr*` trees.

---

## 1. The `.smd` file format

A `.smd` file has two parts, separated by a `Start:` line:

```
;; -- top half: configuration --
Name = My Overlay
User_RefreshRate = 30
EnableCPU = true
Color = COLOR{0xF77F}
FpsLine: {"FPS=%d", Game_FPS_int}
hist = HISTORY{int, 64}
conds: GRAPH_CONDITIONS{{x < 30, 0xF005, 0xF00F}, {x >= 30, 0x0F0F, 0x0F05}}

Start:
;; -- bottom half: render script --
#if $Game_IsGameRunning
    BOX{0, 0, 100, 20, BackgroundColor}
    TEXT{5, 0, 18, Color, FpsLine}
    GRAPH_LINE_CHART{0, 25, 100, 40, 0, 120,
                     LEFT_TO_RIGHT, 0xF00F, 0xF005, conds, hist}
#endif
```

### Lexical layer

- **Comments** begin with `;` and run to end of line. The semicolon is recognized everywhere except inside string literals.
- **Strings** are double-quoted with the usual `\n`, `\t`, `\\`, `\"` escapes.
- **Trailing whitespace + CRLF** is normalized away; the parser is robust to Windows-style line endings.
- **Blank lines** are ignored.
- **Identifier characters**: `[A-Za-z_][A-Za-z0-9_]*`. Dots in identifiers are translated to underscores at parse time (`Game_Resolution[0].calls` → `Game_Resolution_0_calls`), so dotted access is purely syntactic sugar.
- `$name` is identical to `name` — the leading `$` is dropped during preprocessing. It exists for visual clarity, especially in conditions.

### Config (top half) — supported value kinds

The top half is `key SEP value` pairs, one per line, where `SEP` is either `:` (default-kind) or `=` (constant-kind). The value's syntax determines its `ConfigKind`:

| Syntax                              | `ConfigKind`      | Example                                   |
|-------------------------------------|-------------------|-------------------------------------------|
| Decimal/hex integer                 | `Integer`         | `User_RefreshRate = 60`, `Color = 0xF00F` |
| `true` / `false`                    | `Bool`            | `EnableCPU = true`                        |
| `"..."`                             | `String`          | `Name = "Bottom Bar"`                     |
| `{ "fmt", arg, ... }`               | `Format`          | `FpsLine: {"FPS=%d", Game_FPS_int}`       |
| `HISTORY{int\|float\|double, N}`    | `History`         | `cpuHist = HISTORY{int, 64}`              |
| `GRAPH_CONDITIONS{{cond, lc, fc}…}` | `GraphConditions` | see GRAPH_CONDITIONS section              |
| `COLOR{0xRGBA}`                     | `Integer` (swapped)| `Color = COLOR{0xF77F}` → `0xF77F` reversed|

**Separator semantics (`:` vs `=`)**:

- **`key = value`** — *constant-kind*. The name is **immutable**: `VAR{key, ...}` is a compile-time error. Use for things the script and the host both treat as fixed: the overlay's `Name`, layer dimensions, color constants, HISTORY declarations, user-configurable knobs that no script branch will ever rewrite.
- **`key: value`** — *default-kind*. The name is **mutable**: the script may rebind via `VAR{key, ...}` and the assignment persists across frames. Use for state variables (`lastFrame: 0`, `gameWasActive: false`) or any value the script needs to track over time. For values that differ between modes (docked vs handheld, etc.), use an `#if`/`#else` pair to write the right value each frame.

Both kinds are **seeded once at `Compile()`** and never re-seeded by `Reset()` — so a `:` value the script assigns persists across frames until the script next writes to it. (Format-kind values are the one exception: they're re-materialised against live host data every `Reset()` because the whole point of a Format is to follow live state.)

**Three keys require `=`**: `Name`, `LayerWidth`, `LayerHeight`. Writing them with `:` produces a clear error at Load time. Conceptually these are properties of the overlay layer itself, not of the rendering script, so they can't reasonably mutate.

**Peek behaviour**: `Document::Peek` / `PeekFromMemory` honour **only `=` lines** for these three keys. A file that uses `Name:` will fail Peek (won't find a name) even though Peek doesn't otherwise validate the file.

### Render script (bottom half) — supported commands

| Command                                                                              | Meaning                                                |
|--------------------------------------------------------------------------------------|--------------------------------------------------------|
| `TEXT{x, y, fontSize, color, "lit" or VAR}`                                          | Draw text                                              |
| `BOX{x, y, w, h, color}`                                                             | Filled rectangle                                       |
| `ROUNDED_BOX{x, y, w, h, roundnessTl, roundnessTr, roundnessBl, roundnessBr, color}` | Stroked rectangle with rounded edges                   |
| `EMPTY_BOX{x, y, w, h, color}`                                                       | Stroked rectangle                                      |
| `DASHED_LINE{x, y, x2, y2, lineLength, gap, color}`                                  | Dashed line; geometry args are pixels                  |
| `GET_DIMENSIONS{dimsName, fontSize, "lit" or VAR}`                                   | Asks host to measure; result stored as `dimsName.x/.y` |
| `VAR{name, expr or "string" or string+expr}`                                         | Define / update a script-level variable                |
| `HISTORY_UPDATE{historyName, expr}`                                                  | Push a sample onto a ring buffer                       |
| `HISTORY_CLEAN{historyName}`                                                         | Clear a ring buffer                                    |
| `GRAPH_LINE_CHART{x,y,w,h, min,max, dir, line, fill, conds, hist}`                   | Draw a graph                                           |
| `#if expr` / `#elif expr` / `#else` / `#endif`                                       | Conditional block                                      |

All commands use `{}` as their argument-list delimiter. Args are split on commas at depth 0 (i.e. `,` inside nested `{}` doesn't terminate an arg).

### Conditional directives (`#if` / `#elif` / `#else` / `#endif`)

These are **render-script lines**, not preprocessor passes. The condition is evaluated each frame and gates whether the block executes. Conditions are full expressions including `==`, `!=`, `<`, `<=`, `>`, `>=`, `and`/`or`/`not`, plus all the regular arithmetic. **The expression engine for conditions is built on top of tinyexpr** (which only handles pure arithmetic) — see `EvalCondition` in §6.

The parser rejects stray `#endif` / `#else` / `#elif` at the top level (anywhere outside an `#if` block) with a clear error. It also rejects double `#else` and `#elif` after `#else`.

### `COLOR{}` pseudo-function

`COLOR{0x1234}` evaluates to `0x4321` — the nibble order is reversed. The reason: file authors think in RGBA, the display hardware wants ABGR. Authors write the natural ordering and the parser pre-swaps it.

Two forms:

- **Literal**: `COLOR{0xHHHH}` (or decimal) — rewritten at line-ingest time by `RewriteLiteralColors()` before anything else sees the source. After this step the parser is looking at an ordinary integer literal.
- **Expression**: `COLOR{expr}` — rewritten by `PreprocessExpr()` to `color(expr)`, which dispatches to a `TE_FUNCTION1|TE_FLAG_PURE` tinyexpr builtin. Evaluated each frame.

The implementation lives at:
- Helper: `static inline uint16_t SmdNibbleSwap16(uint16_t v)` (one bit-twiddle).
- Literal rewrite: `static void RewriteLiteralColors(std::string& line)` — called once per line after comment-strip + trim.
- Expression rewrite: inside `PreprocessExpr`, where it recognizes `COLOR{` as a token boundary and converts braces to parens for tinyexpr.
- tinyexpr registration: `te_color` in the builtins block alongside `round`/`trunc`/`min`/`max`.

---

## 2. The public API (`smd::Document`)

Public surface, in order of typical use:

```cpp
class Document {
public:
    using Callback = void(*)(RenderCommand& cmd, void* user);

    Document();
    ~Document();                    // calls Free()
    Document(Document&&) noexcept;  // move-only
    Document& operator=(Document&&) noexcept;
    // Copy is deleted.

    bool LoadFromFile  (const char* path);
    bool LoadFromMemory(const char* data, size_t size);

    // Set the callback used to query the host for the active IETF locale key.
    // Call before LoadFromFile / LoadFromMemory. The callback and user pointer
    // survive Free() so they do not need to be re-set between loads.
    void SetRecordCallback(RecordCallback cb, void* user);

    // Cheap one-shot inspectors. Don't allocate a full Document::Impl.
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

    // Bind a host pointer for a predefined name. The pointer must outlive
    // the Document; the parser reads it every frame inside Evaluate().
    void BindInt64 (const char* name, const int64_t*     ptr);
    void BindFloat (const char* name, const float*       ptr);
    void BindDouble(const char* name, const double*      ptr);
    void BindBool  (const char* name, const bool*        ptr);
    void BindString(const char* name, const std::string* ptr);
    void BindResolutionArray(const char* name, const ResolutionEntry* entries);

    bool Compile();                          // call once after Load + all Bind*
    bool Evaluate(Callback cb, void* user);  // call once per frame

    void Reset();                            // resets mutable state for next frame
    void ClearDimsMeasureCache();            // GET_DIMENSIONS cache
    void Free();                             // release everything

    const ConfigValue* GetConfig(const char* key) const;
    int64_t            GetConfigInt   (const char* key, int64_t def = 0)      const;
    bool               GetConfigBool  (const char* key, bool def = false)     const;
    const char*        GetConfigString(const char* key, const char* def = "") const;
    bool               FormatConfigString(const char* key, std::string& out);

    const char* LastError() const;           // wrapped to 40 columns per line

private:
    struct Impl;
    Impl* m_impl;
};
```

### Lifecycle

```
Document doc;                               // empty, defaults seeded
doc.LoadFromMemory(buf.data(), buf.size()); // parse to AST + config
doc.BindInt64(...);                         // ...one Bind per predefined name in use...
doc.Compile();                              // resolve names, compile exprs
for each frame:
    doc.Reset();                            // refresh Format strings, zero un-backed VARs
    doc.Evaluate(MyCallback, this);
doc.Free();                                 // or let the destructor handle it
```

Calling `Load*` again on a live Document is fine — it does an internal `Free()` first.

### Error handling

Every fallible method returns `bool` and sets a single `lastError` string on the `Impl`. `LastError()` returns that string **wrapped to ≤40 columns per line** (wraps on whitespace where possible; hard-breaks tokens longer than 40 chars). The wrap is computed lazily inside `LastError()` so all the `lastError = "..."` assignment sites stay simple.

When `LastError()` is empty, you get `""` (never `nullptr`).

---

## 3. The render-command callback

The callback signature is:

```cpp
void MyCallback(smd::RenderCommand& cmd, void* user);
```

You inspect `cmd.type` and read the relevant fields. The `RenderCommand` struct is a single discriminated union (every kind reuses the same struct; unused fields are zero). Fields per kind:

### `RenderCmdType::Text`
- `text` — the string to draw
- `x`, `y`, `fontSize`, `color` — pixel position, point size, RGBA color (already nibble-swapped if `COLOR{}` was used)

### `RenderCmdType::Box` and `RenderCmdType::EmptyBox`
- `x`, `y`, `width`, `height`, `color`

### `RenderCmdType::RoundedBox`
- `x`, `y`, `width`, `height`, 
- `roundnessTl`, `roundnessTr`, `roundnessBl`, `roundnessBr` - level of roundness for each top/bottom right/left corner for range [0.0, 1.0]
- `color`

### `RenderCmdType::DashedLine`
- `x`, `y` — start
- `x2`, `y2` — end
- `dashOn`, `dashOff` — length of drawn segment / gap segment
- `color`

### `RenderCmdType::GetDimensions`
- `text`, `fontSize` — what to measure
- `outDims` — pointer the host **must write to** with measured `{x, y}` (width, height)
- `dimsName` — the name the script knows this measurement by

This is the only callback kind where the host *writes back*. The host's font/renderer measures the string, fills `*outDims`, and returns. The parser caches the result keyed on `(text, fontSize)` so subsequent GET_DIMENSIONS in the same frame are free. The cache has a soft cap of 256 entries (drops the whole cache when exceeded — see `ClearDimsMeasureCache`).

### `RenderCmdType::GraphLineChart`
- `x`, `y`, `width`, `height` — drawing rect
- `minClamp`, `maxClamp` — y-axis bounds (the script's `min` and `max` args)
- `direction` — `LeftToRight` or `RightToLeft` (rendering hint; the sample buffer order does NOT change with direction)
- `color`, `fillColor` — default colors for the line + fill
- `sampleType` — `HistorySampleType::Int`, `Float`, or `Double`. Tells the host which sample-pointer field to read.
- `samples` — `const int64_t*`, non-null only when `sampleType == Int`.
- `samplesD` — `const double*`, non-null when `sampleType == Float` or `Double`. (Float histories are promoted to double on the way out so the host has a single read path for both.)
- Both pointers are `nullptr` when `sampleCount == 0`. **`samples[0]` / `samplesD[0]` is oldest, index `sampleCount - 1` is newest.** True regardless of `direction`.
- `sampleCount` — currently valid samples
- `sampleCapacity` — declared ring capacity (from `HISTORY{int|float|double, capacity}`)
- `conditions` — optional `const GraphConditionView*`, length `condCount`. Per-sample override colors; see §4.
- `historyName` — which HISTORY buffer this chart is reading from

### `RenderCmdType::HistoryUpdate` and `HistoryClean` (DEBUG only)

These exist **only when the parser is compiled with `-DDEBUG`**. In release builds, the parser still maintains the underlying ring buffer (because `GraphLineChart` needs it) but suppresses the callback. The host typically doesn't need to see these; they're useful for instrumentation/tracing. When enabled:

- `HistoryUpdate`: `historyName`, `historyValue`
- `HistoryClean`: `historyName`

If your render code uses a `switch` over `RenderCmdType`, you'll get unreachable-case warnings if you handle these without the macro. Either wrap your cases in `#ifdef DEBUG` or compile with `-DDEBUG` in the same translation unit as the renderer.

---

## 4. Reading `GraphLineChart` samples and per-sample conditions

```cpp
case smd::RenderCmdType::GraphLineChart: {
    // 1) Iterate samples oldest -> newest. The fanout on sampleType decides
    //    which pointer to read; everything else (matches, drawing) operates
    //    on a uniform double.
    for (size_t i = 0; i < cmd.sampleCount; ++i) {
        double v;
        switch (cmd.sampleType) {
            case smd::HistorySampleType::Int:
                v = (double)cmd.samples[i];   // cmd.samples is const int64_t*
                break;
            case smd::HistorySampleType::Float:
            case smd::HistorySampleType::Double:
                v = cmd.samplesD[i];          // cmd.samplesD is const double*
                break;
        }
        // Decide per-sample colors. `cond.matches` takes a double; for an
        // int history the (double)cast above is the only adapter you need.
        uint16_t lineCol = cmd.color;
        uint16_t fillCol = cmd.fillColor;
        for (size_t c = 0; c < cmd.condCount; ++c) {
            const auto& cond = cmd.conditions[c];
            if (cond.matches(cond.state, v)) {
                lineCol = (uint16_t)cond.lineColor;
                fillCol = (uint16_t)cond.fillColor;
                break;  // first match wins
            }
        }
        // ... draw segment i with lineCol/fillCol, mapping v into
        // [minClamp .. maxClamp] -> [y+height .. y] (or use your own mapping).
    }
    // 2) `direction` decides which screen edge the newest sample sits at.
    //    The buffer order is always oldest-first regardless.
    break;
}
```

The `matches` function pointer in `GraphConditionView` is filled in by the parser. It takes a `double` regardless of the underlying history type (int samples are cast on the way in). It evaluates each row's compiled `cond` expression (e.g. `x < 30`) with the sample bound to `x`. The condition is the same kind of expression you can write in `#if` (full comparison/logical support via `EvalCondition`). `state` is opaque — pass it straight through.

If `condCount == 0`, fall back to `cmd.color` / `cmd.fillColor` for every sample.

---

## 5. Predefined names

These are the names the SMD format recognizes as belonging to the host. They split into three groups.

### Group A: live host data (bound via `Bind*`)

The host **must** call the appropriate `Bind*` for any predefined name the SMD file references. The parser only reads through these pointers — it never writes. Each name has an implicit type that determines which `Bind*` to use (encoded in the suffix: `_int`, `_float`, `_double`, `_str`, plus bare `Is*` for bools, and `Game_Resolution*_int` for `BindResolutionArray`).

Note: there's no enforcement that the `Bind*` you call matches the suffix; you're trusted. But the script will type-error or coerce odd things if you cross the streams.

**CPU**: `CPU_Hz_int`, `CPU_RealHz_int`, `CPU_DeltaHz_int`, `CPU_Core0Load_double`...`CPU_Core3Load_double`.

**GPU**: `GPU_Hz_int`, `GPU_RealHz_int`, `GPU_DeltaHz_int`, `GPU_Load_int`.

**RAM**: `RAM_Hz_int`, `RAM_RealHz_int`, `RAM_DeltaHz_int`, `RAM_LoadAll_int`, `RAM_LoadCPU_int`, `RAM_UsedAllMB_float`, `RAM_TotalAllMB_float`, plus `_UsedApplicationMB_float` / `_TotalApplicationMB_float` / `_UsedAppletMB_float` / `_TotalAppletMB_float` / `_UsedSystemMB_float` / `_TotalSystemMB_float` / `_UsedSystemUnsafeMB_float` / `_TotalSystemUnsafeMB_float`.

**Board**: `Board_ChargerCurrentLimit_int`, `Board_ChargerVoltageLimit_int`, `Board_ChargerConnected_int`, `Board_BatteryCurrentAvg_float`, `Board_BatteryVoltageAvg_float`, `Board_IsBatteryFiltered`, `Board_BatteryAgePercentage_float`, `Board_BatteryChargePercentage_float`, `Board_BatteryTemperatureCelcius_float`, `Board_DesignedFullBatteryCapacity_float`, `Board_ActualFullBatteryCapacity_float`, `Board_PowerConsumption_float`, `Board_BatteryTimeEstimateInMinutes_int`, `Board_SocTemperatureCelsius_float`, `Board_PcbTemperatureCelsius_float`, `Board_SkinTemperatureMiliCelsius_int`, `Board_FanRotationPercentageLevel_float`.

**Game**: `Game_LastFrameNumber_int`, `Game_IsGameRunning`, `Game_FPS_int`, `Game_FpsAvgOld_float`, `Game_FpsAvg_float`, `Game_ReadSpeedPerSecond_float`, `Game_ResolutionRenderCalls_int` (BindResolutionArray, 8 entries), `Game_ResolutionViewportCalls_int` (BindResolutionArray, 8 entries).

**System**: `System_DisplayRefreshRate_int`, `System_IsDocked`, `System_KeysDown_int`, `System_KeysHeld_int`, `formattedKeyCombo` (string).

**Misc**: `Misc_IsWiFiPassphrase`, `Misc_NvDecHz_int`, `Misc_NvEncHz_int`, `Misc_NvJpgHz_int`, `Misc_NetworkConnectionType_int`, `Misc_WiFiPassphrase_str`.

All of the above are **read-only from the script's perspective** — `VAR{Game_FPS_int, 100}` is a compile error.

### Group B: `System_Key_*` button constants

Pure integer constants seeded into the symbol table. Bit values match libnx's `HidNpadButton`. Use in conditions like `#if $System_KeysHeld_int == System_Key_Y`.

`System_Key_A` (1<<0), `B` (1<<1), `X` (1<<2), `Y` (1<<3), `StickL` (1<<4), `StickR` (1<<5), `L` (1<<6), `R` (1<<7), `ZL` (1<<8), `ZR` (1<<9), `Plus` (1<<10), `Minus` (1<<11), `Left` (1<<12), `Up` (1<<13), `Right` (1<<14), `Down` (1<<15), `LeftSL` (1<<24), `LeftSR` (1<<25), `RightSL` (1<<26), `RightSR` (1<<27).

### Group C: writable config defaults

These are seeded into the config map on construction (and re-seeded after `Free()`) so a freed Document still answers `GetConfigInt("LayerWidth") == 448`. The SMD file may override them in its top half.

| Key                  | Kind    | Default value |
|----------------------|---------|---------------|
| `COMMON_MARGIN`      | Integer | 20            |
| `BackgroundColor`    | Integer | `0xD000`      |
| `ComboButtonFooter`  | String  | "\uE0E1  Back     \uE0E0  OK" |
| `Movable`            | Bool    | false         |
| `User_RefreshRate`   | Integer | 60            |
| `EnableGame`         | Bool    | false         |
| `EnableCPU`          | Bool    | false         |
| `EnableGPU`          | Bool    | false         |
| `EnableRAM`          | Bool    | false         |
| `EnableBoard`        | Bool    | false         |
| `EnableMisc`         | Bool    | false         |
| `LayerWidth`         | Integer | 448           |
| `LayerHeight`        | Integer | 720           |
| `LayerPos_x`         | Integer | 0             |
| `LayerPos_y`         | Integer | 0             |
| `HeaderText`         | Bool    | true          |
| `FooterText`         | Bool    | true          |
| `UseCustomExitCombo` | Bool    | false         |
| `EnableControls`     | Bool    | true          |

Reading these values: `GetConfigInt("LayerWidth")`, `GetConfigBool("HeaderText")`, etc. Writing them isn't supported — they're host-readable, file-overridable, but not script-mutable.

---

## 6. Internal architecture (`Document::Impl`)

The implementation hides behind a single pimpl, `Document::Impl`. Key members:

### Static parsed data (computed once during Load + Compile)

- `script` — `std::vector<AstNode>` representing the render block. Each `AstNode` is one command (or `If` with then/else branches).
- `config` — `std::unordered_map<std::string, ConfigValue>` keyed by config name.
- `histories` — `std::unordered_map<std::string, HistoryDecl>`. Each `HistoryDecl` holds the declared capacity plus a `std::vector<int64_t> samples` ring buffer.
- `graphConds` — `std::unordered_map<std::string, GraphCondDecl>` (parsed `GRAPH_CONDITIONS` lists).
- `ownedFormatArgs` — owning `std::vector<std::unique_ptr<FormatArg>>` for any compiled FormatArg trees (ternaries, nested formats, etc.).
- `ownedNestedFormats` — owning storage for nested `FormatSpec` instances.

### Per-Document mutable state (live across frames)

- `hostBinds` — `std::unordered_map<std::string, HostBinding>`. Each `HostBinding` has the host pointer, a type tag, and a `double scratch` slot. **Pointer stability matters here**: the `teVarTable` below holds pointers into these scratch slots, so `unordered_map`'s guarantee that pointers to mapped values survive rehashes is what makes this safe. (Iterators do get invalidated, but the parser only ever holds pointers, not iterators.)
- `hostStringBinds` — same idea for `std::string*` host pointers.
- `hostResArrays` — `std::unordered_map<std::string, std::unique_ptr<HostResolutionArrayBinding>>`. Each binding owns 24 `double` scratch slots (8 entries × 3 fields: width, height, calls), refreshed per frame.
- `privateVars` — `std::unordered_map<std::string, std::unique_ptr<double>>`. Backing storage for VARs that are *numeric* (the bool/int kinds). `unique_ptr` for pointer stability when the map rehashes.
- `stringVars` — `std::unordered_map<std::string, std::string>`. Backing storage for VARs that are *string-valued*.
- `dimsVars` — names declared by `GET_DIMENSIONS`. Each has a `Dimensions* dims` + two scratch doubles for `.x` and `.y` mirrors so expressions like `myText.x + 5` work.
- `configMirror` — for Integer/Bool config values, a mirror `double` so they can be referenced in tinyexpr expressions.
- `implicitZero` — `std::unordered_map<std::string, std::unique_ptr<double>>`. Catch-all for unbound identifiers found during prescan; they evaluate to 0. Created on demand so the prescan never fails for typos.
- `teVarTable` — `std::vector<te_variable>`. The merged symbol table handed to `te_compile`. **All pointers in here must be stable across the Document's lifetime**, hence all the `unique_ptr<double>`s.
- `ownedExprs` — `std::vector<te_expr*>`. Every compiled tinyexpr tree we own; freed in `Free()`.
- `arithCache` — `std::unordered_map<std::string, te_expr*>`. **Persistent across frames.** Caches the compiled form of arithmetic source strings that `EvalCondition` builds on-the-fly. Critical for memory stability: see §9.
- `dimsMeasureCache` — `std::unordered_map<std::string, Dimensions>`. Caches GET_DIMENSIONS results keyed on `(text, fontSize)`. Soft cap of 256 entries.
- `lastError`, `lastErrorWrapped`, `lastErrorWrappedOf` — error string plus the lazy 40-col wrap.

### Two-pass build

`Load*` does **parse only** — comments stripped, lines tokenized, `RewriteLiteralColors` applied, config table populated, render script built into an AST. No te_expr trees yet, no name resolution. This is cheap.

`Compile()` does **resolve + compile**:
1. Wipes `ownedExprs` / `arithCache` (recompile invalidates any old trees).
2. Walks the script registering every `VAR` and `GET_DIMENSIONS` declaration. Crucial that this happens *before* the prescan, otherwise `myText_x` (a dims alias) would look like an unknown identifier.
3. **Prescan** — walks every expression source and calls `EnsureVariable` for each identifier reference. Order: privateVars → configMirror → hostBinds → resolution-array slot pattern → implicitZero. Misses get auto-added to `implicitZero` so `te_compile` never fails on an unknown name.
4. Builds `teVarTable` from the union of all visible names, plus the `System_Key_*` constants and the custom function builtins (`round`, `trunc`, `min`, `max`, `color`).
5. Compiles every numeric expression source we found, parking the `te_expr*` in `ownedExprs` and remembering the handle in the AST node (`Expr::compiled`).

A recompile (calling `Compile()` again) tears everything down before rebuilding. The persistent `arithCache` is invalidated at the same time.

### Per-frame `Reset` + `Evaluate`

`Reset(bool freeze = false)`:
1. **Calls `RefreshScratches()`** to pull current host pointer values into all scratch doubles. This guarantees that any format-string materialisation done below sees current data, not last frame's.
2. Zeroes private numeric VARs that have **no matching config key**. Config-backed VARs (both `:` and `=` kinds) are left alone — they were seeded once at `Compile()` and carry whatever value the script most recently wrote.
3. Re-materialises **Format-kind** config strings (the live `{"...", host_var}` forms) into the matching `stringVars` slot. Other string-VAR kinds (`String`-kind configs, plain `:` strings) are not touched — Format is the only kind explicitly designed to "always refresh."
4. Resets `dimsVars` (the GET_DIMENSIONS result mirrors) to zero.

If `freeze == true` only position is restored back to Start while everything else is not zeroed nor updated.

This behavior is what makes the cross-frame state idiom (`lastFrame: 0` + `VAR{lastFrame, ...}`) work: the VAR's assignment from the previous frame is still live when the next frame starts.

`Evaluate()`:
1. Refreshes scratches again (cheap — same data flowing).
2. Walks the AST. For each node:
   - `If`: evaluate condition, recurse into chosen branch.
   - `Var`: evaluate RHS (numeric → te_eval, string → MaterializeText/EvalStringExpr), store into the target.
   - `Text` / `Box` / etc.: evaluate every numeric arg via `te_eval`, materialize text args, emit a `RenderCommand`, call the callback.
   - `HistoryUpdate`: push value onto ring buffer; emit callback only in DEBUG.
   - `HistoryClean`: clear ring buffer; emit callback only in DEBUG.
   - `GraphLineChart`: evaluate geometry args, build `GraphConditionView[]` from the named GraphCondDecl rows, fill `cmd.samples` / `sampleCount` / `sampleCapacity` from the live `HistoryDecl`, emit callback.
   - `GetDimensions`: look up cache, on miss invoke callback (host fills `outDims`), store result.

### `RefreshScratches`

A single tight loop that copies every bound host pointer's value into the corresponding scratch double in the `te_var` table. ~50 doubles in a typical SMD file. Called at the top of both `Reset()` and `Evaluate()` — the double-refresh is essentially free and guarantees consistency.

---

## 7. Expression engine

`tinyexpr` (vendored, in `third_party/tinyexpr/`) handles pure arithmetic only. The parser builds two layers on top:

### `PreprocessExpr(source)` — text-level transform

Runs on every expression source string before it ever reaches `te_compile`. Does:
- Drop `$` prefixes (`$Game_FPS_int` → `Game_FPS_int`).
- Collapse `ident.field` into `ident_field` (only when both sides look like identifier chars). Used for `dimsVar.x`, `Game_Resolution[0].calls`, struct VARs (`R.width` → `R_width`), etc.
- Rewrite `Name[N]` (N a digit) as `Name_N`, so `Game_Resolution[0].calls` ends up as `Game_Resolution_0_calls` after both passes.
- Translate `COLOR{expr}` → `color(expr)` (brace → paren), recursing into the body for the same dot/$ rules.

This is purely textual — it doesn't know if a name is bound. The bound-or-not question is answered later by `EnsureVariable`.

### `EvalCondition` — comparisons + logical ops

tinyexpr can't handle `==`, `!=`, `<`, `<=`, `>`, `>=`, `and`, `or`, `not`. So `EvalCondition` is a tiny recursive-descent layer that:
1. Splits on top-level `or` / `and` / `not`.
2. For each leaf comparison `lhs OP rhs`, evaluates `lhs` and `rhs` independently via `EvalArith` (which calls tinyexpr through the `arithCache`), then applies the comparator.
3. For bare leaves (no comparator), evaluates the leaf via `EvalArith` and checks truthiness (nonzero is true; NaN is false).

`EvalArith` interns each arithmetic source string. On cache hit it returns the cached `te_expr*` and calls `te_eval`. On cache miss it compiles, inserts into `arithCache`, and pushes to `ownedExprs`. **This is why the cache must persist across frames** — if it were per-Evaluate-call, the parser would recompile every condition every frame and silently leak `te_expr` trees into `ownedExprs`. We hit exactly this bug; see §9.

### `EvaluateFormatArg` — format-spec arguments

`{"FPS=%d", expr}` has its `expr` slots compiled to a tree of `FormatArg` nodes. Kinds:
- `Numeric` — a pre-compiled `te_expr*`, evaluated each frame.
- `String` — a `StringExpr` (literal + identifier segments joined by `+`).
- `Ternary` — `cond ? thenArg : elseArg`, where `thenArg`/`elseArg` are themselves `FormatArg*`. Evaluated via `EvalCondition` on the cond, then recurse into the chosen arm.
- `NestedFormat` — a nested `FormatSpec` (e.g. `{"%s", {"%d:%02d", a, b}}`), evaluated by recursing into MaterializeText.

The Ternary case takes a `CondCache*` argument. When the caller (notably `MaterializeText` / `EvalStringExpr` for inline brace-wrapped ternaries) doesn't have one, the function constructs a local `CondCache` pointing at the **persistent** `im.arithCache` — not a fresh map. Same memory-stability requirement as `EvalCondition`.

### Custom builtins

Registered with `TE_FUNCTION1|TE_FLAG_PURE` (or `TE_FUNCTION2`):
- `round(x)` — banker-style rounding away from zero.
- `trunc(x)` — truncate toward zero (cast to `long long` and back).
- `min(a, b)`, `max(a, b)`.
- `color(x)` — `SmdNibbleSwap16` on the lower 16 bits.

---

## 8. VAR semantics

`VAR{name, value}` is both declaration and assignment. The classifier looks at the RHS to decide:

- RHS is a string expression (starts with `"`, or contains a top-level `+` joining strings) → goes into `stringVars` as a `std::string`. The parser owns the StringExpr structure; evaluation calls `EvalStringExpr(...)` which materializes the concatenation each frame.
- RHS is a numeric expression → goes into `privateVars` as a `unique_ptr<double>`. The parser parks the compiled `te_expr*` on the AST node.
- RHS is `Name[N]` where `Name` is a bound resolution array → **struct-copy mode**: parser registers three private doubles `Name_width`, `Name_height`, `Name_calls` and copies all three each frame.

**VAR target rules**:

- **Group A names** (live host data, e.g. `Game_FPS_int`): never. Compile-time error.
- **Group B names** (`System_Key_*` constants): never. Compile-time error.
- **`=` config keys**: never. Compile-time error with message naming the key and suggesting `:` instead.
- **`:` config keys**: allowed. The first write seeds the matching `privateVars`/`stringVars` slot with the new value; the slot was already initialised once at `Compile()` from the config default.
- **Names not in config at all**: allowed. The slot starts at 0 (numeric) or `""` (string) and Reset re-zeroes it every frame for the numeric case (since there's no config backing to preserve).

**Empty RHS** (`VAR{x, }`) or empty target name (`VAR{, 5}`) are rejected at parse time.

**Cross-frame state**: a VAR whose target maps to a `:` config key persists its last-written value across frames — Reset doesn't clobber it. This is the supported idiom for "remember something between frames":

```
;; config:
lastFrame: 0
gameWasActive: false
;; script:
VAR{lastFrame, Game_LastFrameNumber_int}  ; carries across frames
```

A VAR with no matching config key (`VAR{tmp, 5}` with no `tmp` in config) gets zeroed by Reset, so it's "fresh per frame." Authors choosing between the two pick the config-backed version when they want cross-frame memory, the bare version when they want a scratch variable.

---

## 9. Memory model — what to watch out for

The most common way to introduce a slow leak in this parser is to compile `te_expr` trees into a local `std::unordered_map<std::string, te_expr*>` cache. The map dies at end of call, but the `te_expr*` values it held survive in the Document-lifetime `ownedExprs`. Every call then compiles fresh trees, all surviving in `ownedExprs` until Document destruction. AddressSanitizer **does not** catch this (the trees are eventually freed at destruction), but `mallinfo2` will show steady growth — and on a real Switch overlay running 30–60fps it adds up fast.

The parser avoids this by having a single `arithCache` member on `Impl`, persistent across frames. Both `Document::Evaluate` and `EvaluateFormatArg` route through `&im.arithCache`. `Free()` and `Compile()` clear it whenever they wipe `ownedExprs` (because clearing `ownedExprs` invalidates everything `arithCache` points to).

**Rule for new code**: if you add a path that compiles a `te_expr` per call, do not introduce a local cache. Either reuse `im.arithCache` or carefully `te_free` the trees you allocate. The regression test `test_memory_growth` (`mallinfo2`-based) catches this; run it whenever you touch `Evaluate`, `EvaluateFormatArg`, `EvalCondition`, or `EvalArith`.

Other lifetime invariants to preserve:
- Every `unique_ptr<double>` in `privateVars` / `implicitZero` is referenced from `teVarTable` via a raw pointer. Don't replace `unique_ptr` with a value — `unordered_map` rehashes invalidate value addresses for values stored by value, but pointers retrieved via `.get()` from a `unique_ptr` member remain stable.
- Same for `HostResolutionArrayBinding` (held by `unique_ptr` in `hostResArrays`).
- `dimsMeasureCache` has a soft cap (256). Dynamic text generates a fresh key every frame; without the cap it would grow unboundedly. The cap fires by **clearing the entire cache**, not by eviction — simpler and the cost is one re-measure per text per frame for a few frames until things refill.

---

## 10. Build and test layout

Top-level `CMakeLists.txt` exposes options:
- `SMD_SANITIZE` — adds `-fsanitize=address,undefined`.
- `SMD_DEBUG_HISTORY` — defines `DEBUG`, exposing the HistoryUpdate/HistoryClean callbacks.
- `SMD_BUILD_TESTS` — builds and registers the `tests/` binaries with CTest.

Default standard is C++23 (matches devkitPro's GCC 15). Each test binary is standalone and runs with `WORKING_DIRECTORY` set to the `fixtures/` dir, so tests can refer to `.smd` files by bare filename.

CI lanes (`.github/workflows/ci.yml`):
- x86_64 Release, ASan+UBSan, ASan+UBSan+DEBUG
- arm64 Release, ASan+UBSan, ASan+UBSan+DEBUG (native `ubuntu-24.04-arm` runners — actual aarch64, no qemu)

All lanes install GCC 15 from `ubuntu-toolchain-r/test` PPA. Test logs are uploaded on every run (success or failure) as artifacts.

### Test inventory

Quick reference for what each test covers — useful for picking the right starting point when reproducing a bug:

| Test                          | Focus                                                       |
|-------------------------------|-------------------------------------------------------------|
| `test_all_smd_files`          | Per-fixture: required RenderCmdTypes + required TEXT substrings |
| `test_fixture_discovery`      | Runtime-enumerated `.smd` files (no hardcoded list)         |
| `test_render_output`          | Hand-checked output across 3 fixtures                       |
| `test_array_index_mangling`   | `Name[N].field` → `Name_N_field` preprocessing              |
| `test_brace_ternary`          | `{cond ? a : b}` in config Format args                      |
| `test_edge_cases`             | Empty file, no Start, unclosed/extra directives             |
| `test_history`                | HISTORY ring + GRAPH_LINE_CHART, 100+ frames                |
| `test_lifecycle`              | Multi-frame eval, Free()/Reset(), defaults reseed           |
| `test_many_formats`           | 30 × 50 nested-format reps (alloc/free stress)              |
| `test_predefined_names`       | RW defaults + `System_Key_*` constants                      |
| `test_stress`                 | 11 lifecycle/error-path scenarios                           |
| `test_strict_directives`      | Stray #endif/#else/#elif, malformed VARs rejected           |
| `test_struct_var`             | `VAR{R, Game_Resolution...[N]}` struct-copy                 |
| `test_memory_growth`          | Per-frame heap growth via `mallinfo2` (skipped in sanitizer lanes) |
| `test_color_swap`             | COLOR{} pseudo-function, both literal and expression forms  |
| `test_reset_refresh`          | Format-kind config strings re-evaluate against current host data on each Reset+Evaluate |
| `test_error_wrap`             | LastError() ≤40 columns per line                            |
| `test_history_types`          | HISTORY{int\|float\|double} routing through `cmd.samples` vs `cmd.samplesD`, ring wrap-around, HISTORY_CLEAN, bad type-name rejection |
| `test_config_kinds`           | `=` (constant) vs `:` (default) syntax: VAR rejects `=` keys, Name/Layer\* require `=`, Peek honours only `=` for those, cross-frame `:` state idiom, Format-kind always re-evaluates |
| `test_inline_format`          | Inline `{"fmt", args...}` form as TEXT / GET_DIMENSIONS trailing arg: int/hex/float specs, multiple inline commands, multi-arg formats, arithmetic args, live host data, backward compat for named refs / quoted literals / bare identifiers |
| `test_list_for`               | `LIST{type, ...}` config kind (int/float/double/str), `RANGE{start, end, step}` integer generator, `#for $var in $list` iteration with FIFO order, string `==` / `!=` comparison, nested loops, mixed-type comparison falls to unequal, empty list runs body zero times, all error paths for malformed LIST / RANGE / #for |

---

## 11. Cheat sheet for common edits

The line-number hints below are approximate ("around line N") and drift as code is added. When in doubt, grep for the relevant identifier rather than trusting the number.

**Adding a new render command** (e.g. `CIRCLE`):
1. Add `Circle` to `NodeKind` (smd_parser.cpp near the top).
2. Add `Circle` to `RenderCmdType` in smd_parser.hpp.
3. Add parser case in `ParseCommandBody` (grep for `if (name == "TEXT")` — the dispatch lives around line 1400): name match, arg count, store sources into AST node.
4. Add a case in the **registerDecls** walk (catches new VAR/dims declarations the command might introduce — only needed if the command declares any). Grep `registerDecls = ` or `case NodeKind::Text:` in that function.
5. Add a case in the **prescan** walk (calls `EnsureVariable` on every identifier the new command references). Grep `prescanRefs` or look at the second `case NodeKind::Text:` site.
6. Add a case in the **evaluate** walk (the per-frame emission, around the third `case NodeKind::Text:` site, ~line 3890): evaluate args, fill a `RenderCommand`, invoke the callback.

**Adding a new predefined host name**:
1. Append the literal string to `kFixed[]` in `IsPredefinedReadOnlyName()` (~line 264).
2. Document it in §5 here and in `SMD_FORMAT.md` §7.

**Adding a new writable config default**:
1. Append to `kConfigDefaults[]` (~line 362).
2. Document it in §5 here and in `SMD_FORMAT.md` §7.

**Adding a new tinyexpr builtin function**:
1. Define the static `+[]` lambda in the builtins block (grep for `te_round` to find the cluster).
2. Add `im.teVarTable.push_back({...})` with `TE_FN1` or `TE_FN2` flag.
3. Bump the `+ N` slop in the `teVarTable` reserve calculation.

**Touching expression evaluation**: re-run `test_memory_growth` in the Release lane (it's disabled when sanitizers are on). The test catches any new path that compiles `te_expr` trees without using `im.arithCache`.

---

## 12. Known sharp edges

- **`:` vs `=` is load-bearing.** `key = value` makes the name immutable (VAR will refuse it at compile time). `key: value` keeps it mutable. `Name`, `LayerWidth`, `LayerHeight` *must* use `=`. The rest is convention but commonly: `=` for constants (colors, dimensions, HISTORY declarations, user-tunable knobs), `:` for things the script will write to (state flags, frame counters, anything that needs to carry across frames). Get this wrong and the script silently produces wrong output, since the parser can't tell intent from syntax alone.
- **Reset does not re-seed config-backed VARs.** Both kinds are seeded once at `Compile()` and from then on hold whatever the script most recently wrote (or the initial config value if nothing's been written yet). The one exception: Format-kind config strings (`{"%d", live_var}`) are re-materialised every Reset because their whole point is to follow live data.
- **`true` / `false` work in expressions** as numeric literals. PreprocessExpr maps them to `1` / `0` at identifier boundaries, so `VAR{flag, true}` and `#if $cond == false` both work. A name like `truesz` is left alone (only matches at word boundaries).
- **`UseCustomExitCombo`** defaults to `false`.
- **`EnableControls`** defaults to `true`.
- **`historyName` is always-available**, even outside DEBUG, because `GraphLineChart` also uses it. Only `historyValue` (HistoryUpdate-specific) is DEBUG-gated.
- **`direction` in GraphLineChart is render-only**. The sample buffer order is always oldest-first; `direction` tells you which screen edge the newest sample maps to.
- **`samples` and `samplesD` can both be `nullptr`** when `sampleCount == 0`. Always guard. When non-empty, exactly one of the two is non-null — the one matching `sampleType`.
- **`Reset()` calls `RefreshScratches()` at its entry**, so the order `Reset(); Evaluate();` is what you want every frame. Skipping Reset leaves Format-kind strings stale.
- **String VARs survive across frames within their `#if` branch** — if the branch is false on frame N+1, the VAR's last-firing-frame value is what reads back. This is by design (lets you carry state) but can surprise authors who expect "uninitialized."
- **`arithCache` must be persistent**. If you add a code path that compiles tinyexpr trees per call, route through `im.arithCache` or free the trees yourself. `test_memory_growth` is the safety net.
- **Inline `{"fmt", arg, ...}` as a TEXT / GET_DIMENSIONS arg** is parsed as a single-segment `StringExpr` (same data shape used by VAR's string-mode RHS) and upgraded into a real FormatSpec by the classify pass. It re-evaluates every frame, just like a named config Format. Use named Formats up top when the same template is reused across commands; reach for inline when it's a one-off.

