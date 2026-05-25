# SMD Syntax Highlighting for VS Code

Syntax highlighting for Status Monitor Design (`.smd`) files — the overlay format used by SaltyNX/Tesla Switch overlays.

Highlights:

- **Commands** (`TEXT`, `BOX`, `ROUNDED_BOX`, `VAR`, `GRAPH_LINE_CHART`, etc.) styled as built-in functions
- **`:` vs `=` separators** styled differently — `=` (constant-kind) gets a distinct color from `:` (default-kind) so the immutability difference is visible at a glance
- **Predefined host names** (`Game_FPS_int`, `CPU_Hz_int`, `RAM_HocClkRamBWAll_int`, etc.) styled as built-in variables — typos like `Game_Fps_int` will fall back to the generic identifier color
- **`System_Key_*` constants** styled as language constants
- **`#if` / `#elif` / `#else` / `#endif`** as control keywords
- **`#for` / `#endfor`** as loop keywords
- **Comments** starting with `;`
- **String literals** with `\n` / `\t` / `\\` / `\"` escapes
- **printf format specifiers** (`%d`, `%.1f`, `%s`, etc.) inside Format spec strings
- **`COLOR{}` / `HISTORY{}` / `GRAPH_CONDITIONS{}`** pseudo-functions
- **`HISTORY_AVERAGE{name}`** — numeric expression returning the mean of a ring buffer; styled as a built-in function
- **`IETF{...}` / `IETF_LOCALE{...}` / `IETF_GET{...}` / `NAME_LOCALE{...}`** — localisation directives
- **Built-in math functions** (`round`, `trunc`, `min`, `max`, `floor`, `ceil`, `abs`, `sqrt`, `color`, `log`, `exp`, `sin`, etc.)
- **Direction keywords** `LEFT_TO_RIGHT` / `RIGHT_TO_LEFT`
- **Operators** including `and`/`or` and ternary `?:`

---

## Install (development / personal use)

The fastest way to use it, no publishing or packaging required:

1. Copy the entire `smd-language` folder to your VS Code extensions directory:
   - **Windows**: `%USERPROFILE%\.vscode\extensions\smd-language`
   - **macOS / Linux**: `~/.vscode/extensions/smd-language`
2. Reload VS Code (Ctrl+Shift+P → "Developer: Reload Window", or just quit and reopen).
3. Open any `.smd` file. Highlighting should activate automatically based on the file extension.

If highlighting doesn't appear:

- Ctrl+Shift+P → "Change Language Mode" → pick **"Status Monitor Design"**. If it's in the list, the extension is loaded but file-extension association didn't kick in. Reload again and it'll stick.
- Check the bottom-right of the status bar — it should say `Status Monitor Design` when an `.smd` file is open.

## Install (as a `.vsix` package)

Useful if you want to share the extension or install it cleanly:

1. Install the packaging tool (one-time): `npm install -g @vscode/vsce`
2. From the `smd-language` folder, run: `vsce package`
3. That produces a `smd-language-0.1.0.vsix` file.
4. In VS Code: Ctrl+Shift+P → "Extensions: Install from VSIX..." → pick the file.

---

## What colors will I actually see?

VS Code themes map scope names (like `keyword.control.smd`) to colors. Most modern themes already handle the common scopes well; you should see reasonable colors out of the box on themes like **Dark+**, **Monokai**, **One Dark Pro**, **Material**, **GitHub Theme**, etc.

If you want to fine-tune specific scopes, add a `editor.tokenColorCustomizations` block to your `settings.json`. For example, to make `=` config separators stand out in orange and `:` separators stay quiet in gray:

```json
{
  "editor.tokenColorCustomizations": {
    "textMateRules": [
      {
        "scope": "keyword.operator.assignment.constant.smd",
        "settings": { "foreground": "#FFA500", "fontStyle": "bold" }
      },
      {
        "scope": "keyword.operator.assignment.default.smd",
        "settings": { "foreground": "#7F8C8D" }
      },
      {
        "scope": "support.variable.predefined.host.smd",
        "settings": { "foreground": "#4FC1FF" }
      },
      {
        "scope": "constant.language.system-key.smd",
        "settings": { "foreground": "#C586C0" }
      },
      {
        "scope": "support.function.command.smd",
        "settings": { "foreground": "#DCDCAA", "fontStyle": "bold" }
      },
      {
        "scope": "support.function.color.smd",
        "settings": { "foreground": "#CE9178" }
      },
      {
        "scope": "support.function.history-average.smd",
        "settings": { "foreground": "#CE9178" }
      },
      {
        "scope": "support.type.ietf.smd",
        "settings": { "foreground": "#9CDCFE" }
      },
      {
        "scope": "support.function.ietf.smd",
        "settings": { "foreground": "#9CDCFE" }
      },
      {
        "scope": "support.function.ietf-get.smd",
        "settings": { "foreground": "#9CDCFE" }
      }
    ]
  }
}
```

You don't need to set all of these — pick what you find useful.

---

## Full scope reference

| Scope name                                        | What it highlights                                                                                        |
|---------------------------------------------------|-----------------------------------------------------------------------------------------------------------|
| `comment.line.semicolon.smd`                      | `; line comment` until end of line                                                                        |
| `keyword.control.start.smd`                       | The `Start:` marker between header and script                                                             |
| `keyword.control.conditional.smd`                 | `#if`, `#elif`, `#else`, `#endif`                                                                         |
| `keyword.control.loop.smd`                        | `#for`, `in`, `#endfor`                                                                                   |
| `variable.other.constant.smd`                     | Identifier on the left of `=` (constant-kind config key)                                                  |
| `variable.other.smd`                              | Identifier on the left of `:` (default-kind config key), or any unrecognized identifier                   |
| `variable.other.config-default.smd`               | Built-in config defaults (`LayerWidth`, `BackgroundColor`, etc.)                                          |
| `keyword.operator.assignment.constant.smd`        | `=` separator                                                                                             |
| `keyword.operator.assignment.default.smd`         | `:` separator                                                                                             |
| `support.type.history.smd`                        | `HISTORY` keyword in `HISTORY{...}`                                                                       |
| `storage.type.history.smd`                        | `int` / `float` / `double` inside `HISTORY{}`                                                             |
| `support.type.list.smd`                           | `LIST` keyword in `LIST{...}`                                                                             |
| `storage.type.list.smd`                           | `int` / `float` / `double` / `str` inside `LIST{}`                                                       |
| `support.function.range.smd`                      | `RANGE` keyword inside `LIST{int, RANGE{...}}`                                                            |
| `support.type.graph-conditions.smd`               | `GRAPH_CONDITIONS` keyword                                                                                |
| `support.function.color.smd`                      | `COLOR{...}` pseudo-function                                                                              |
| `support.function.history-average.smd`            | `HISTORY_AVERAGE{name}` — numeric mean of a ring buffer                                                   |
| `support.type.ietf.smd`                           | `IETF` keyword in `IETF{...}` config values                                                               |
| `support.function.ietf.smd`                       | `IETF_LOCALE{...}` and `NAME_LOCALE{...}` directive lines                                                 |
| `support.function.ietf-get.smd`                   | `IETF_GET{name}` expression (resolves localised string at render time)                                    |
| `support.function.command.smd`                    | Render commands: `TEXT`, `BOX`, `ROUNDED_BOX`, `EMPTY_BOX`, `DASHED_LINE`, `GET_DIMENSIONS`, `VAR`, `HISTORY_UPDATE`, `HISTORY_CLEAN`, `GRAPH_LINE_CHART` |
| `support.function.builtin.smd`                    | Math builtins (`round`, `trunc`, `min`, `max`, etc.)                                                      |
| `support.variable.predefined.host.smd`            | Host-bound names (`Game_FPS_int`, `CPU_Hz_int`, `RAM_HocClkRamBWAll_int`, ...)                            |
| `constant.language.system-key.smd`                | `System_Key_*` button constants                                                                           |
| `constant.language.direction.smd`                 | `LEFT_TO_RIGHT` / `RIGHT_TO_LEFT`                                                                         |
| `constant.language.boolean.smd`                   | `true` / `false`                                                                                          |
| `string.quoted.double.smd`                        | `"text"` literals                                                                                         |
| `string.unquoted.name.smd`                        | The right-hand side of a `Name =` line when written without quotes                                        |
| `string.quoted.double.format.smd`                 | Format string `"%d ..."` inside `{"...", ...}`                                                            |
| `constant.other.placeholder.smd`                  | `%d`, `%.1f`, `%s`, etc. inside format strings                                                            |
| `constant.character.escape.smd`                   | `\n`, `\t`, `\\`, `\"`                                                                                    |
| `constant.numeric.hex.smd`                        | `0x1234`                                                                                                  |
| `constant.numeric.decimal.smd`                    | `42`, `3.14`, `1.5e3`                                                                                     |
| `keyword.operator.comparison.smd`                 | `==`, `!=`, `<`, `>`, `<=`, `>=`, `and`, `or`                                                             |
| `keyword.operator.arithmetic.smd`                 | `+`, `-`, `*`, `/`, `%`, `^`, `**`                                                                        |
| `keyword.operator.ternary.smd`                    | `?`, `:` (ternary)                                                                                        |
| `keyword.operator.variable-marker.smd`            | Leading `$` on variable references                                                                        |
| `punctuation.section.*.smd`                       | `{`, `}`, `[`, `]`, `(`, `)`                                                                              |
| `punctuation.separator.smd`                       | `,`                                                                                                       |
| `punctuation.accessor.smd`                        | `.` (for `dims.x`, `R.width`, etc.)                                                                       |

---

## Known limitations

- **Multi-line conditions** aren't supported by the grammar (each line is parsed independently). The parser doesn't support them either, so this isn't a regression.

## Authoring the grammar

The grammar follows VS Code's standard TextMate JSON format. To tweak it:

1. Edit `syntaxes/smd.tmLanguage.json`.
2. Reload your VS Code window — no rebuild needed.
3. Useful debugging command: Ctrl+Shift+P → "Developer: Inspect Editor Tokens and Scopes" — shows you the scope hierarchy under the cursor, so you know what color the current theme applies.

The grammar's `repository` is organized so each named pattern can be included independently. The `expression` rule is the central one used everywhere a value is expected (config RHS, command args, condition bodies, format args). Adding a new command means appending to the alternation in `command`; adding a new builtin function means appending to `builtin-function`; adding a new pseudo-function (like `HISTORY_AVERAGE` or `IETF_GET`) means adding a new `begin/end` rule to the repository and including it in `expression`.