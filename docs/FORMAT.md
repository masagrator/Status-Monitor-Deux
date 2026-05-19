# SMD Format Reference

Status Monitor Design (`.smd`) format.

Each file defines a small program: a set of configuration values up top, then a render script that the host runs once per frame.

This document is for **authors writing `.smd` files**. For details on how the parser is implemented internally, see `PARSER_INTERNALS.md`.

---

## 1. File structure

Every `.smd` file is two halves separated by a `Start:` line:

```
;; -- header: configuration --
Name = My Overlay
LayerWidth = 448
LayerHeight = 720
User_RefreshRate = 30

Color = COLOR{0xF77F}
FpsLine: {"FPS=%d", Game_FPS_int}

Start:
;; -- body: render script --
BOX{0, 0, 100, 20, BackgroundColor}
TEXT{5, 0, 18, Color, FpsLine}
```

**Above `Start:`**: configuration. One `key value` pair per line.
**Below `Start:`**: render commands. One command per line. Conditional blocks with `#if`/`#elif`/`#else`/`#endif`. For loops with `#for` / `endfor`.

### Comments and whitespace

- Comments start with `;` and run to end of line. Anywhere outside a string literal.
- Blank lines are ignored.
- CRLF and trailing whitespace are tolerated.
- Indentation is ignored
- Identifier characters are `[A-Za-z_][A-Za-z0-9_]*`.
- A leading `$` on an identifier is purely visual: `$Game_FPS_int` and `Game_FPS_int` mean the same thing. The `$` form is conventional inside `#if` conditions to make variable references visually distinct from operators.

---

## 2. Configuration section

### Separators: `:` vs `=`

The configuration section uses **two different separators**, and the choice matters:

```
User_RefreshRate = 30              ; constant: cannot be changed by VAR
lastFrame: 0                       ; default: VAR may rewrite it
```

| Separator | Meaning                                                    |
|-----------|------------------------------------------------------------|
| `=`       | **Constant.** The name is immutable. `VAR{key, ...}` is a compile-time error. Use for things that are decided once and never change (overlay name, dimensions, color constants, HISTORY declarations, user-tunable knobs the script never rewrites). |
| `:`       | **Default.** The script may reassign with `VAR{key, ...}` and the new value persists across frames. Use for state variables, frame counters, or any value the script needs to update over time. |

Three keys **must** use `=`: `Name`, `LayerWidth`, `LayerHeight`. Writing them with `:` is rejected at load time with a clear error.

Both kinds are seeded once when the file is loaded; neither is re-applied frame-by-frame. The difference is purely about mutability. The single exception: **Format-kind** values (the `{"%d", ...}` form, see below) re-evaluate every frame regardless of separator — that's the whole point of a format.

### Value kinds

What you put on the right-hand side determines how the value is treated:

#### Integer

Decimal or `0x`-prefixed hex.

```
COMMON_MARGIN = 20
LayerHeight = 720
WindowColor = 0xF00F
```

Negative values work too: `Offset = -5`.

#### Bool

`true` or `false`, case sensitive.

```
EnableCPU = true
HeaderText = false
```

#### String

Double-quoted. Standard escapes: `\n`, `\t`, `\\`, `\"`. Unicode bytes can be embedded literally.

```
Name = "FPS Counter"
FPSnoDataText = "n/d"
```

#### Format spec — `{"fmt", arg, ...}`

A printf-style template followed by argument expressions. **Re-evaluated every frame** against live host data.

```
FpsLine: {"FPS=%d", Game_FPS_int}
BatteryText: {"Battery: %.1f%%", Board_BatteryChargePercentage_float}
ComboFooter: {"Hold %s to exit", formattedKeyCombo}
```

Supported printf conversions:

| Conversion                              | Argument is read as          |
|-----------------------------------------|------------------------------|
| `%d` `%i`                               | signed integer (64-bit)      |
| `%u` `%o` `%x` `%X`                     | unsigned integer (64-bit)    |
| `%f` `%F` `%e` `%E` `%g` `%G` `%a` `%A` | double-precision float       |
| `%c`                                    | character (int casted)       |
| `%s` `%S`                               | string                       |
| `%%`                                    | literal `%`                  |

Flags (`-`, `+`, ` `, `#`, `0`), width, and precision are honoured. **Length modifiers** (`h`, `hh`, `l`, `ll`, `lf`, etc.) are accepted but ignored — the parser always picks the right length for its internal representation. So `%hhu`, `%u`, `%lu`, `%llu` all behave identically.

A `%s` slot pulls a value from one of: a string-typed host binding, a `String`-kind config value, a string-typed `VAR`, or a string-valued ternary. A `{"...","..."}` format may be **nested** inside another:

```
TimeOrDash: {"%s", Board_BatteryTimeEstimateInMinutes_int >= 0 ? {"%d:%02d", Board_BatteryTimeEstimateInMinutes_int / 60, Board_BatteryTimeEstimateInMinutes_int % 60} : "-:--"}
```

#### History buffer — `HISTORY{type, capacity}`

Declares a ring buffer of samples used by `GRAPH_LINE_CHART`.

```
FPSreading = HISTORY{float, 180}
CPUreading = HISTORY{int, 60}
PowerReading = HISTORY{double, 120}
```

Types: `int` (64-bit signed integers), `float` (single-precision), `double` (double-precision). Capacity is the number of samples retained; new samples push out the oldest. **Always declared with `=`** — the buffer's *contents* mutate via `HISTORY_UPDATE`, but the *declaration* (type + capacity) is fixed.

#### Graph conditions — `GRAPH_CONDITIONS{{cond, lineColor, fillColor}, ...}`

Per-sample style overrides for `GRAPH_LINE_CHART`. Each row is a triple: a condition that compares the sample (named `x` inside the expression) plus a line color and a fill color. First matching row wins per sample; un-matched samples use the chart's default line/fill colors.

`fillColor` is currently ignored.

```
GraphConditions: GRAPH_CONDITIONS{{round(x) == System_DisplayRefreshRate_int, perfectColor, 0x0000}, {round(x * 2) == System_DisplayRefreshRate_int, roundedColor, 0x0000}}
```

#### Color shorthand — `COLOR{0xRGBA}`

`COLOR{16-bit value}` does the nibble swap so you can use RGBA order for RGBA4444 framebuffer instead of BARG. `COLOR{0xABCD}` (RGBA) becomes `0xDCBA` (BARG).

```
Color = COLOR{0xF77F}        ; literal: rewritten at load time
TintedColor = COLOR{base + 4} ; expression: evaluated each frame
```

Two forms: a **literal-body** `COLOR{0xHHHH}` is rewritten at load time (free at runtime), and an **expression-body** `COLOR{expr}` is evaluated each frame.

#### List — `LIST{type, {elem, elem, ...}}`

A typed array. The element type is `int`, `float`, `double`, or `str`. Used as the iterand of a `#for` loop (see §5b).

```
Categories = LIST{str, {"CPU", "GPU", "RAM"}}
Sizes      = LIST{int, {16, 24, 32}}
Ratios     = LIST{float, {0.5, 1.0, 1.5}}
```

Element literals are parsed at config time and never re-evaluated; expressions and identifier references are not allowed inside the element list.

It supports special command inside `LIST`: `RANGE{start, end, step}`

Python-style integer sequence generator. Used in place of the element list inside `LIST{int, ...}`:

```
Indices = LIST{int, RANGE{0, 8, 2}}    ; -> 0, 2, 4, 6
Down    = LIST{int, RANGE{10, 0, -2}}  ; -> 10, 8, 6, 4, 2
```

The sequence includes `start` and continues while strictly below `end` (or strictly above, when `step` is negative). Equal `start` and `end`, or a step pointing the wrong way, produces an empty list — no error. `step` of `0` is a parse error. RANGE only generates integers; `LIST{float, RANGE{...}}` is rejected.

#### IETF - `IETF{string}`

A special kind of string storage used for translations. string represents a text that is used when system/overriden language is not detected.

```
WarningText = IETF{"Game is not running or it's incompatible."}
```

#### IETF_LOCALE - `IETF_LOCALE{name, IETF code string, string}`

Assign string to local variable initiated with `IETF{string}` only if provided IETF code matches what program expects.

IETF code is determined by system language or can be overriden by editing `config.ini` in `config/system-monitor-deux/`, enabling `override_language` by writing:
```ini
override_language=true
```
and changing `override_language_ietf_code` to language supported by SMD and locale files. If it's not correct, program will fallback to default string.

List of codes supported natively by system:
- `JP-JP` (Japanese)
- `EN-US` (American English)
- `FR-FR` (French)
- `DE-DE` (German)
- `IT-IT` (Italian)
- `ES-ES` (Spanish)
- `ZH-CN` (Chinese Simplified)
- `ZH-TW` (Chinese Traditional)
- `KO-KR` (Korean)
- `NL-NL` (Dutch)
- `PT-PT` (Portuguese)
- `RU-RU` (Russian)
- `EN-GB` (British English)
- `FR-CA` (Canadian French)
- `ES-419` (Latin Spanish)
- `PT-BR` (Brazilian Portuguese)

Any other language must be forced via `config.ini`.

```
IETF_LOCALE{WarningText, "PL-PL", "Gra jest nieuruchomiona lub niekompatybilna."}
```

#### IETF_GET - `IETF_GET{name}`

Get string from local variable initiated with `IETF{string}`.

```
TEXT{0, 20, 18, User_WarningColor, IETF_GET{WarningText}}
```

#### NAME_LOCALE - `NAME_LOCALE{ietf code string, string}`

Assign string to `Name` variable if provided ietf code string matches what program expects. Read more about it in `IETF_LOCALE` section.

---

## 3. Render script

Below `Start:`, the file is a sequence of render commands. They execute top-to-bottom, every frame.

### `TEXT{x, y, fontSize, color, contentExpr}`

Draws text at pixel position `(x, y)`, with `fontSize` in points and a 16-bit RGBA color. `contentExpr` is one of:

- A bare string literal: `TEXT{0, 0, 18, 0xFFFF, "Hello"}`
- A name of a config String or Format: `TEXT{0, 0, 18, 0xFFFF, FpsLine}`
- A name of a string `VAR`: `TEXT{0, 0, 18, 0xFFFF, Output}`

Inline `{"fmt", args...}` is also accepted — handy for one-off values that don't deserve a config key of their own:

```
TEXT{0, 13, 10, 0xFFFF, {"%d", RefreshRate}}
GET_DIMENSIONS{labelDims, 18, {"FPS: %d", Game_FPS_int}}
```

The inline form re-evaluates every frame, same as a named config Format. For values used in many places, declaring a named Format up top and referencing it keeps the script tidier.

### `BOX{x, y, width, height, color}`

Filled rectangle.

```
BOX{0, 0, LayerWidth, 50, BackgroundColor}
```

### `ROUNDED_BOX{x, y, width, height, roundness top left corner, roundness top right corner, roundness bottom left corner, roundness top right corner, color}`

Filled rectangle with rounded corners. Roundness range is [0.0, 1.0]. 0.0 for all roundness inputs is equal to using BOX{x, y, width, height, color}.

```
ROUNDED_BOX{0, 0, LayerWidth, 50, 0.3, 0.0, 0.3, 0.0, BackgroundColor}
```

### `EMPTY_BOX{x, y, width, height, color}`

Stroked (outline-only) rectangle.

### `DASHED_LINE{x, y, x2, y2, dashOn, dashOff, color}`

Dashed line from `(x, y)` to `(x2, y2)`. `dashOn` is the length of each drawn segment, `dashOff` is the gap. Both in pixels.

### `GET_DIMENSIONS{name, fontSize, contentExpr}`

Asks the host to measure how big `contentExpr` will render at `fontSize`. The result lands in two dotted variables, `name.x` (width) and `name.y` (height), which any *following* commands can reference.

```
GET_DIMENSIONS{labelDims, 18, "Battery: 100%"}
TEXT{(LayerWidth - labelDims.x) / 2, 0, 18, 0xFFFF, "Battery: 100%"}
```

The measurement is cached per `(text, fontSize)` pair, so repeated calls with the same arguments are free. Dynamic text gets re-measured.

### `VAR{name, expression}`

Declare or assign a script-local variable.

```
VAR{boxWidth, System_DisplayRefreshRate_int >= 100 ? 212 : 202}
VAR{label, "Frame: " + {"%d", Game_LastFrameNumber_int}}
```

Three RHS forms:

1. **Numeric expression** → stored as a double. Available in subsequent expressions.
2. **String expression** → stored as a string. Built from `"literal" + identifier + "more"` segments and `{"fmt", ...}` Format pieces.
3. **Resolution-array struct** → `VAR{R, Game_ResolutionRenderCalls_int[0]}` copies all three fields (`width`, `height`, `calls`). Access as `R.width`, `R.height`, `R.calls`.

VAR cannot overwrite:
- Predefined host names (`Game_FPS_int`, `System_KeysHeld_int`, etc.)
- `System_Key_*` button constants
- Config keys declared with `=` (constant-kind)

VAR can freely reassign config keys declared with `:` (default-kind). The new value persists across frames until the next assignment. A VAR with no matching config key starts at `0`/`""` each frame.

### `HISTORY_UPDATE{historyName, valueExpression}`

Pushes one sample onto a history buffer. The value is evaluated as a double and stored with the precision the buffer was declared with.

```
HISTORY_UPDATE{FPSreading, Game_FpsAvg_float}
```

If the buffer is at capacity, the oldest sample is dropped.

### `HISTORY_CLEAN{historyName}`

Clears every sample from the buffer, maintaining declared capacity.

```
HISTORY_CLEAN{FPSreading}
```

### `GRAPH_LINE_CHART{x, y, width, height, min, max, direction, lineColor, fillColor, conditions, history}`

Draws a graph of a history buffer.

```
GRAPH_LINE_CHART{20, 5, 180, 60, 0, 60, LEFT_TO_RIGHT, mainColor, 0x0000, GraphConditions, FPSreading}
```

Arguments:

| Field           | Meaning                                                                           |
|-----------------|-----------------------------------------------------------------------------------|
| `x, y`          | Left top start position of graph                                                  |
| `width, height` | Graph dimensions                                                                  |
| `min, max`      | Y-axis bounds. Samples clamp to `[min, max]`.                                     |
| `direction`     | `LEFT_TO_RIGHT` or `RIGHT_TO_LEFT`. Which screen edge the newest sample lands at. |
| `lineColor`     | Default line color.                                                               |
| `fillColor`     | Default fill color (under the line). Currently ignored.                           |
| `conditions`    | A `GRAPH_CONDITIONS` name, or empty to use only the defaults.                     |
| `history`       | A `HISTORY` name.                                                                 |

If you don't want per-sample style overrides, leave the conditions slot empty:

```
GRAPH_LINE_CHART{0, 0, 100, 60, 0, 60, LEFT_TO_RIGHT, 0xFFFF, 0x0000, , myHistory}
```

---

## 4. Expressions

Numeric expressions use **tinyexpr-style arithmetic** plus a small set of additions for SMD use. They appear inside `VAR` RHS, `#if`  conditions, format args, and most command arguments.

### Arithmetic

- `+ - * /` — standard
- `%` — modulo (also works on doubles, returns `a - floor(a/b)*b` style)
- `^` or `**` — exponent
- Parentheses for grouping
- Unary `-`

### Comparisons (only inside conditions and ternaries)

- `==`, `!=`
- `<`, `<=`, `>`, `>=`

`==` and `!=` also work on strings: each side may be a quoted string literal or an identifier whose value is a string (config String, string-VAR, or a string-typed `#for` loop variable). When one side is a string and the other is numeric, the result is `!=` true / `==` false. Other comparisons (`<`, `<=`, `>`, `>=`) are numeric-only and not meaningful on strings.

### Logical (only inside conditions and ternaries)

- `and`
- `or`

There is **no `not` operator**. Use `== false` or `== 0` for negation.

### Ternary

```
VAR{x, condition ? thenValue : elseValue}
TEXT{0, 0, 18, gameRunning ? 0xFFFF : 0x8888, "..."}
```

Works in any numeric expression slot. Inside Format-spec args:

```
FpsLine: {"FPS: %d (%s)", Game_FPS_int, Game_FPS_int >= 60 ? "OK" : "Low"}
```

### Built-in functions

| Name          | Args           | Result                                              |
|---------------|----------------|-----------------------------------------------------|
| `round(x)`    | 1              | round to nearest, ties away from zero               |
| `trunc(x)`    | 1              | truncate toward zero (cast to integer)              |
| `floor(x)`    | 1              | round down (tinyexpr standard)                      |
| `ceil(x)`     | 1              | round up (tinyexpr standard)                        |
| `abs(x)`      | 1              | absolute value                                      |
| `sqrt(x)`     | 1              | square root                                         |
| `min(a, b)`   | 2              | smaller of two                                      |
| `max(a, b)`   | 2              | larger of two                                       |
| `color(x)`    | 1              | RGBA-to-ABGR nibble swap (same as `COLOR{x}`)       |

Standard tinyexpr math (`log`, `exp`, `sin`, `cos`, `pow`, `atan2`, ...) is also available.

### Boolean literals in expressions

Bare `true` and `false` work as numeric `1` and `0` in expressions. They only match at word boundaries — a name like `truesz` is left alone.

```
VAR{flag, true}           ; same as VAR{flag, 1}
#if $cond == false        ; same as #if $cond == 0
```

### Dotted access

A struct VAR or a resolution-array slot is read with `.field`:

```
GET_DIMENSIONS{D, 18, "x"}
TEXT{D.x, D.y, 18, 0xFFFF, "x"}
```

For resolution arrays:

```
TEXT{0, 0, 18, 0xFFFF, {"%dx%d, %d", Game_ResolutionRenderCalls_int[0].width, Game_ResolutionRenderCalls_int[0].height, Game_ResolutionRenderCalls_int[0].calls}}
```

---

## 5. Conditionals

Conditional rendering uses `#if` / `#elif` / `#else` / `#endif`. Conditions are full expressions including comparisons and logical operators.

```
#if $Game_IsGameRunning
    TEXT{0, 0, 18, 0xFFFF, "Game running"}
#elif $System_IsDocked
    TEXT{0, 0, 18, 0xFFFF, "Docked, no game"}
#else
    TEXT{0, 0, 18, 0xFFFF, "Handheld, no game"}
#endif
```

Conditions are evaluated every frame, so any `#if` can depend on live host data, VARs assigned earlier in the same frame, or both. Nested `#if` blocks are fine.

Stray `#endif`, `#else`, `#elif` outside an open `#if` are a load-time error. Double `#else` or `#elif` after `#else` are also rejected.

---

## 5b. Loops

```
#for $loopVar in $listName
    ...body...
#endfor
```

The body runs once per element of `$listName`, in FIFO (First In First Out) order. Each iteration `$loopVar` holds the current element. The loop variable's type matches the list's element type (int, float, double, or str) and is readable in expressions throughout the body.

`$listName` must reference a `LIST{...}` config key. The list is evaluated once at load time; the loop body is interpreted every frame and may use the loop variable in any expression.

```
Order = LIST{str, {"CPU", "GPU", "RAM"}}
;;;
#for $cat in $Order
    #if $User_ShowCPU and $cat == "CPU"
        TEXT{X_OFFSET, 0, 18, 0xFFFF, "CPU"}
    #endif
    #if $User_ShowGPU and $cat == "GPU"
        TEXT{X_OFFSET, 0, 18, 0xFFFF, "GPU"}
    #endif
    ...
#endfor
```

Loops can nest. `#if` inside a `#for` (and vice-versa) compose normally. The loop variable's storage persists after the loop ends (matching Python semantics) — referencing `$cat` after `#endfor` reads the last value assigned by the loop.

Empty lists run the body zero times. Stray `#endfor` outside any open `#for`, or missing `#endfor`, are load-time errors.

---

## 6. Common idioms

### Cross-frame state

Use a `:` config key as the initial value, then `VAR` to update it:

```
;; header
lastFrame: 0

;; script
#if $Game_LastFrameNumber_int > $lastFrame
    HISTORY_UPDATE{FPSreading, Game_FpsAvg_float}
    VAR{lastFrame, Game_LastFrameNumber_int}
#endif
```

`lastFrame` is seeded to 0 at load, and each frame retains whatever the last `VAR{lastFrame, ...}` wrote. `Reset()` does not wipe it.

### Edge detection (action on state change)

Track the previous state and react when it differs:

```
;; header
gameWasActive: false

;; script
#if $Game_IsGameRunning
    VAR{gameWasActive, true}
#elif $gameWasActive
    HISTORY_CLEAN{FPSreading}    ; game just stopped
    VAR{gameWasActive, false}
#endif
```

### Per-mode value

If you want a value that differs between docked and handheld but the rest of the script just reads "the value":

```
;; header
User_DockedFontSize = 40
User_HandheldFontSize = 30

;; script
#if $System_IsDocked
    VAR{fontsize, User_DockedFontSize}
#else
    VAR{fontsize, User_HandheldFontSize}
#endif
TEXT{0, 0, fontsize, 0xFFFF, "Hello"}
```

The two `=` lines are user-tunable knobs (constants); `fontsize` is a fresh-per-frame VAR built from one of them.

### Center-aligned text

Use `GET_DIMENSIONS` to measure, then offset:

```
VAR{label, {"%.1f FPS", Game_FpsAvg_float}}
GET_DIMENSIONS{labelDims, 18, label}
TEXT{(LayerWidth - labelDims.x) / 2, 10, 18, 0xFFFF, label}
```

### Conditional color

```
TEXT{0, 0, 18, Game_FPS_int < 30 ? 0x00FF : 0xFFFF, FpsLine}
```

(Remember: in `COLOR{}` form the nibble order outputs proper RGBA; with a literal hex like `0x00FF` you're writing it directly in BARG order. Pick one convention and stick with it.)

### Hold-to-exit combo

```
;; header
UseCustomExitCombo = true
ComboButtonFooter = {"Hold %s to exit", formattedKeyCombo}
```

The `formattedKeyCombo` host binding holds the human-readable button names ("L+R+ZL" etc.); the host wires up the actual button-press logic.

---

## 7. Predefined names (host bindings)

Names the host fills in every frame. You read them in any expression; you cannot assign to them with `VAR`.

### CPU
`CPU_Hz_int`, `CPU_Core0Load_double`, `CPU_Core1Load_double`, `CPU_Core2Load_double`, `CPU_Core3Load_double`.
Available only with sys-clk or hoc-clk: `CPU_RealHz_int`, `CPU_DeltaHz_int`

### GPU
`GPU_Hz_int`, `GPU_Load_int`.
Available only with sys-clk or hoc-clk: `GPU_RealHz_int`, `GPU_DeltaHz_int`

### RAM
`RAM_Hz_int`, `RAM_UsedAllMB_float`, `RAM_TotalAllMB_float`, `RAM_UsedApplicationMB_float`, `RAM_TotalApplicationMB_float`, `RAM_UsedAppletMB_float`, `RAM_TotalAppletMB_float`, `RAM_UsedSystemMB_float`, `RAM_TotalSystemMB_float`, `RAM_UsedSystemUnsafeMB_float`, `RAM_TotalSystemUnsafeMB_float`.
Available only with sys-clk or hoc-clk:  `RAM_RealHz_int`, `RAM_DeltaHz_int`, `RAM_LoadAll_int`, `RAM_LoadCPU_int`
Avaialble only with hoc-clk: `RAM_HocClkRamBWAll_int`, `RAM_HocClkRamBWCpu_int`, `RAM_HocClkRamBWGpu_int`, `RAM_HocClkRamBWPeak_int`

### Board
`Board_ChargerCurrentLimit_int`, `Board_ChargerVoltageLimit_int`, `Board_ChargerConnected_int`, `Board_BatteryCurrentAvg_float`, `Board_BatteryVoltageAvg_float`, `Board_IsBatteryFiltered`, `Board_BatteryAgePercentage_float`, `Board_BatteryChargePercentage_float`, `Board_BatteryTemperatureCelcius_float`, `Board_DesignedFullBatteryCapacity_float`, `Board_ActualFullBatteryCapacity_float`, `Board_PowerConsumption_float`, `Board_BatteryTimeEstimateInMinutes_int`, `Board_SocTemperatureCelsius_float`, `Board_PcbTemperatureCelsius_float`, `Board_SkinTemperatureMiliCelsius_int`, `Board_FanRotationPercentageLevel_float`.
Available only with hoc-clk: `Board_HocClkThermalSensorCPU_int`, `Board_HocClkThermalSensorGPU_int`, `Board_HocClkThermalSensorMEM_int`, `Board_HocClkThermalSensorPLLX_int`, `Board_HocClkThermalSensorAO_int`, `Board_HocClkThermalSensorBQ24193_int`

### Game
Available only with SaltyNX
`Game_LastFrameNumber_int`, `Game_IsGameRunning`, `Game_FPS_int`, `Game_FpsAvgOld_float`, `Game_FpsAvg_float`, `Game_ReadSpeedPerSecond_float`, `Game_ResolutionRenderCalls_int` (struct array of 8: `[N].width`/`.height`/`.calls`), `Game_ResolutionViewportCalls_int` (same shape).

### System
`System_IsDocked`, `System_KeysDown_int`, `System_KeysHeld_int`, `formattedKeyCombo` (string).
Available only with SaltyNX: `System_DisplayRefreshRate_int`

### Misc
`Misc_IsWiFiPassphrase`, `Misc_NvDecHz_int`, `Misc_NvEncHz_int`, `Misc_NvJpgHz_int`, `Misc_NetworkConnectionType_int`, `Misc_WiFiPassphrase_str`.

### Button constants

These are integer constants you can compare against `System_KeysDown_int` / `System_KeysHeld_int`. Bit values match libnx's `HidNpadButton`.

`System_Key_A`, `System_Key_B`, `System_Key_X`, `System_Key_Y`, `System_Key_StickL`, `System_Key_StickR`, `System_Key_L`, `System_Key_R`, `System_Key_ZL`, `System_Key_ZR`, `System_Key_Plus`, `System_Key_Minus`, `System_Key_Left`, `System_Key_Up`, `System_Key_Right`, `System_Key_Down`.

Example:

```
#if $System_KeysHeld_int == System_Key_Y
    TEXT{0, 0, 18, 0xFFFF, Misc_WiFiPassphrase_str}
#endif
```

### Writable host-readable defaults

These are read by the host but can also be overridden in your `.smd` config section:

| Key                  | Default                         | Notes                                                 |
|----------------------|---------------------------------|-------------------------------------------------------|
| `COMMON_MARGIN`      | `20`                            | Pixel margin used by the host's frame chrome.         |
| `BackgroundColor`    | `COLOR{0x000D}`                 | Background fill color (use 0x0000 for no background). |
| `ComboButtonFooter`  | `"\uE0E1  Back     \uE0E0  OK"` | Footer hint text.                                     |
| `Movable`            | `false`                         | Can the user drag the overlay?                        |
| `User_RefreshRate`   | `60`                            | Target FPS for the overlay.                           |
| `EnableCPU`          | `false`                         | Enable refreshing CPU_* variables.                    |
| `EnableGPU`          | `false`                         | Enable refreshing GPU_* variables.                    |
| `EnableRAM`          | `false`                         | Enable refreshing RAM_* variables.                    |
| `EnableBoard`        | `false`                         | Enable refreshing Board_* variables.                  |
| `EnableMisc`         | `false`                         | Enable refreshing Misc_* variables.                   |
| `EnableGame`         | `false`                         | Enable refreshing Game_* variables.                   |
| `LayerWidth`         | `448`                           | Overlay layer width.                                  |
| `LayerHeight`        | `720`                           | Overlay layer height.                                 |
| `HeaderText`         | `true`                          | Draw the host's title bar?                            |
| `FooterText`         | `true`                          | Draw the host's footer hint?                          |
| `UseCustomExitCombo` | `false`                         | Honour a custom exit combo from `formattedKeyCombo`?  |
| `EnableControls`     | `true`                          | Accept input events?                                  |

---

## 8. Error messages

The parser surfaces errors via `Document::LastError()`. All errors are word-wrapped to 40 columns per line.

Common ones:

- `config 'Name' must use '=' (constant), not ':'` — pick the right separator.
- `VAR target 'X' is declared with '=' (constant); use ':' in the config section if it should be mutable, or pick a different VAR name` — you wrote `key = ...` in the header, then tried to `VAR{key, ...}` in the script.
- `VAR target 'Y' is a predefined read-only name` — you tried to VAR over a host binding like `Game_FPS_int`.
- `HISTORY: unsupported type 'X' (expected int, float, or double)` — only those three types are accepted.
- `config 'K': HISTORY: malformed braces` — check braces match in your `HISTORY{...}` declaration.
- `stray #endif at top level` (and friends) — a directive imbalance.

When you hit an error, the line and column information isn't always pinpoint — start by checking the named key and the surrounding context.
