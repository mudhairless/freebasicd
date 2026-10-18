# M17 - FreeBASIC Formatter: Detailed Implementation Plan

This document contains the detailed design and implementation plan for M17 (formatter), split into the requested two-fold deliverable:
- A separate executable (`fbformat`) for standalone formatting
- LSP support (`textDocument/formatting`, `textDocument/rangeFormatting`) in the language server

The plan is written for implementation against the current codebase state (AGENTS.md, FreeBASIC.md, existing architecture).

## Design principles

- **Conservative by default**: Prefer not changing text when safety is ambiguous. Return no edits rather than corrupt.
- **Reuse existing infrastructure**: Leverage `tomlplusplus` for config (like `settings.{h,cpp}`), reuse `language.cpp` catalogs (keywords, intrinsics, types), reuse parser for structure-aware decisions (`SplitSafe` needs block ranges).
- **Layering**: Formatter core is LSP-agnostic and byte-offset aware. LSP layer converts between LSP positions/URIs and byte offsets using `utf16.{h,cpp}` and existing patterns. CLI is separate.
- **Round-trip fidelity**: Preserve comments, whitespace where appropriate, metacommands, macro bodies, `#define` bodies. Block comments (`/'...'/`) must be handled losslessly.
- **Idempotent**: Formatting an already-formatted file produces no changes on the test corpus.
- **No silent overrides**: Config file presence is authoritative. Client LSP `FormattingOptions` only apply when no config file is present at all.

## 1. Config schema and discovery

### Schema
Two files share the same schema:
- `freebasicd.toml` (project file, existing discovery via roots)
- `.fbformat` (standalone, walk up tree like `.clang-format`)

Both contain:
```toml
[format-style]
style = "FBdocs"  # preset name (no .toml)
inherit = "base" # optional single-parent inheritance (added per question 5)

[format]
# ... all keys from FB_FORMAT.md catalog ...
```

Preset files (`<name>.toml`) contain only `[format]` keys (and may also have `inherit` under `[format-style]` if we want presets to chain; start with single-parent inheritance).

### Defaults and precedence

**Default preset**: `FBdocs` (question 2). The factory "Leave everywhere" values are the base, but the effective default when nothing is specified is the `FBdocs` preset values, overridden by explicit `[format]` keys.

**Resolution order (highest → lowest), with inheritance**:
1. `[format]` keys from the chosen config file (`.fbformat` if exists, else `freebasicd.toml` if exists). If neither exists, skip to step 3's client-options mapping path.
2. For each key not set in (1), take from the effective preset chain: start from `style` named in that config file's `[format-style]`; if `inherit` is present, walk up the single-parent chain (cycle detection required). Apply in order from root ancestor down to `style` (child overrides parent). If no `style` named, use `FBdocs` as default preset.
3. For keys still unset, built-in defaults (factory `Leave` values as specified in FB_FORMAT.md).
4. **Client LSP `FormattingOptions`**: These apply **only if no config file exists at all** (neither `.fbformat` nor `freebasicd.toml` found for the document). When they apply, map: `insertSpaces=false` → `UseTab=ForIndentation`; `insertSpaces=true` → `UseTab=Never`; `tabSize` → `TabWidth` and `IndentWidth` (unless explicitly set by other means? but no config file exists). Also `trimTrailingWhitespace`, `insertFinalNewline`, `trimFinalNewlines` map to their equivalents. This is a separate layer applied after (3).

**Inheritance**: Single parent via `[format-style].inherit` (question 5). Cycle detection on load. A preset that inherits another loads the ancestor first, then overlays its own `[format]` keys.

### Discovery paths
- **Config files**:
  - `.fbformat`: search up from the file's directory to repo root (like `.clang-format`). First found wins.
  - `freebasicd.toml`: existing root discovery via `Settings`/index roots (reuse). If `.fbformat` not found, check for `freebasicd.toml` at the document's index root.
- **Preset files (`<name>.toml`)**: search in order (highest priority first):
  1. local: `<config-file-dir>/.styles/` (relative to the config file that was found)
  2. user: `~/.config/fbformat/styles/` (Linux/macOS), `%USERPROFILE%\fbformat\styles\` (Windows)
  3. global: `${prefix}/share/fbformat/styles/` on Unix (`/usr` or Homebrew prefix), or `styles\` beside installed binaries on Windows

First match wins. Built-in presets (`FBdocs.toml`, `fbc.toml`, and base defaults) are also installed to global location and/or embedded? Better to ship as files in install (and also have them in repo under `styles/` or generated). Commit the preset TOML files.

### Config loader module
New module: `src/format/config.{h,cpp}` (or in `src/format/` subdir).
- Parse TOML with tomlplusplus (vendored). Follow same error handling as `settings.{cpp}`: unknown keys ignored; malformed values should not crash; log to stderr/client log but be conservative.
- `FormatConfig` struct holds resolved values for all options (concrete values, no "unset" after resolution).
- Functions: `loadConfigForPath(filePath)` returns resolved config (or default FBdocs-based config if none). Handles file discovery, preset loading, inheritance, client options mapping.

## 2. Build scaffolding and CLI

### CMake changes
Add new static library `freebasicd_format`:
```cmake
add_library(freebasicd_format STATIC
    src/format/config.cpp
    src/format/tokens.cpp
    src/format/engine.cpp
    src/format/format_lsp.cpp  # LSP seam
)
target_link_libraries(freebasicd_format
    PUBLIC freebasicd_lang  # for catalogs
    PRIVATE tomlplusplus::tomlplusplus)
```

Link into:
- `freebasicd_core` (add to its sources or link `freebasicd_format`? LSP handlers live in session; better link `freebasicd_format` into `freebasicd_core` or `freebasicd` as needed)
- New executable `fbformat`:
```cmake
add_executable(fbformat
    src/format/fbformat_main.cpp
)
target_link_libraries(fbformat PRIVATE freebasicd_format freebasicd_lang)
```

Install:
- `install(TARGETS fbformat RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})`
- Install preset files: `styles/*.toml` to `${CMAKE_INSTALL_DATADIR}/fbformat/styles/` (or follow global path above)

Add preset files to repo (e.g. `styles/FBdocs.toml`, `styles/fbc.toml`).

### CLI (`fbformat_main.cpp`)
Flags:
- `--in-place` / `-i`: write changes back to file(s)
- `--check`: exit 1 if any file would be changed, 0 if unchanged
- `--stdin-filepath <path>`: path to use for config discovery when reading from stdin
- `--config <path>`: explicit config file override (skip discovery)
- `--style <preset>`: override style name
- `<files...>` or read stdin if no files
(replaced manual parsing with https://github.com/p-ranav/argparse, vendored at `third_party/argparse` — done for `fbformat`; `freebasicd` currently takes no argv at all, so when the server grows a command line it should use argparse too rather than hand-rolling)

Behavior:
- Read file content (or stdin). For stdin, require `--stdin-filepath` for config discovery.
- Load resolved config for that path.
- Format document.
- If changes: print diff? or just write. For `--check`, print nothing required but exit 1. For `--in-place`, overwrite file. For stdout (default), write formatted content.
- Exit codes: 0 = formatted or unchanged (success), 1 = changes needed (`--check`), 2 = error (config/IO/unsafe state).

## 3. Token stream and round-trip foundation

### Block comments (question 1)
Audit `src/lexer.{h,cpp}` for `/'` … `'/`. FreeBASIC block comments can be multi-line. They must be preserved verbatim as a single token/range (or we must track their exact byte spans including newlines). If lexer doesn't support them, extend it:
- Add token kind (e.g. `TK_BLOCK_COMMENT`) or treat as comment token that carries raw text.
- Lex `/'` followed by content until closing `'/` (nesting? FB block comments in practice: check FreeBASIC.md; typically non-nesting in FB, but be conservative). The research just says "block comments are used" - preserve them as-is.
- Ensure positions are correct.

Also handle line comments `'` and `REM`.

### Token model for formatting
Create lossless token stream:
- `FormatToken`: kind, start offset, end offset (byte offsets in original content), text slice (or just ranges). Also flags: isComment, isBlockComment, isDirective, isMetacommand, isString, isWhitespace.
- Tokenize entire document with existing lexer extended for block comments. Also need to identify:
  - `#macro` / `#define` bodies: directive lines that start macro/define - their body must be emitted verbatim (research says macro/define bodies are opaque).
  - Metacommand lines: lines containing `$` in comments that are metacommands? Or lines starting with metacommands. Default `CommentPragmas = "^[[:space:]]*\$"` protects lines matching; also `$lang`, `$dynamic` etc in comment form.
  - Format-off/on pragmas: `' fbformat: off'` / `' fbformat: on'` (case-sensitive as written). Also support `OneLineFormatOffRegex`.
  - Strings: to avoid breaking inside strings (unless `BreakStringLiterals` true, default false). Track string tokens.
- Whitespace is significant in some places? But formatter controls it. Need to be able to reconstruct layout.

### Format-off regions
Parse pragmas in comments. Track regions: when inside off-region, copy original text ranges verbatim. Regions can be nested? Unusual; treat as state.

## 4. Format engine (core)

All formatting operates on byte offsets (LSP-agnostic). Use `ParseResult` from parser for structure-aware decisions.

### Document representation
- Input: original content (string/bytes), file path (for config/lang)
- Parse once: `ParseResult pr = parser.parseDocument(content)`; also get token stream.
- Config: resolved `FormatConfig`.
- Output: new content string.

### Indentation
Track block nesting stack based on keywords and `blockRanges`. Keys that open blocks: `Sub`, `Function`, `Property`, `Type`, `Union`, `Enum`, `Namespace`, `If`, `ElseIf`, `Else`, `Do`, `While`, `For`, `Select Case`, `With`, `Extern`. Close keywords pop.

Options:
- `IndentWidth`, `TabWidth`, `UseTab` (Never/ForIndentation/ForContinuationAndIndentation/Always)
- `ContinuationIndentWidth` for wrapped lines when `AlignEscapedNewlines` off
- `IndentCaseLabels`, `NamespaceIndentation` (None/All/Inner), `IndentAccessModifierSections`, `IndentPPDirectives`, `IndentGotoLabels`, `IndentExternBlock`, `IndentWrappedFunctionNames`
- `BracedInitializerIndentWidth`

### Line breaking and continuations (`_`)
- `ColumnLimit = 0` means off (default in FBdocs? FBdocs sets 80 per research; but default is 0 - need to follow resolved config). Research says "Default off because every break needs a `_`". But FBdocs/fbc presets set 80.
- `ContinuationStyle`: `Leave` (don't re-wrap), `WrapOnly` (re-wrap over limit but don't rejoin author's breaks), `WrapAndJoin` (rejoin breaks that would fit). Start with `WrapOnly` behavior? Or follow strictly.
- `BreakBinaryOperators`: `None` means operator at end of first line (`x or _` - compiler convention), `NonAssignment`/`All` put at start of continuation.
- `AlignEscapedNewlines`: align `_` column.
- Never insert `_` inside strings unless `BreakStringLiterals` true (default false).
- Never break across coordinate-pair `-` (graphics). Coordinate pairs are `(x1,y1)-(x2,y2)`.
- Respect `Penalty*` weights for wrapping decisions.

### Spacing
Implement families F,G from FB_FORMAT.md:
- Assignment operators spacing
- Binary/unary operators
- Parens: `SpaceBeforeParens`, per-context options; `SpacesInParens` split by context (param lists vs call parens vs subscripts vs casts)
- Commas: `SpaceAfterComma`, `SpaceBeforeComma`
- Colons: `SpaceBeforeColon`, `SpaceAfterStatementSeparator`
- Print `#`: `SpaceAfterPrintChannelHash`
- Braced initializers
- Member access `.`/`->`: `SpaceAroundMemberAccess`

### Case transforms
Use catalogs from `src/language.cpp` (read-only, authoritative). 
- `KeywordCase`: Leave/lower/UPPER/Title/Canonical. `Canonical` uses catalog's spelling (important for `ElseIf`, `AndAlso`, `LongInt` etc).
- `TypeNameCase`: builtin types
- `IntrinsicCase`: builtins
- `BooleanLiteralCase`: True/False
- `IntrinsicDollarSuffix`: only safe in `fb` dialect. In qb/fblite/deprecated suffix selects distinct builtin - must ignore `Drop`. In `fb`, `Left`/`Left$` alias.
- `IdentifierCase`: Leave only (invariant).
- Directive/metacommand case: `DirectiveCase`, `MetacommandCase`
- Numeric literal case: prefixes/digits/suffixes/exponents
- `FloatSuffixStyle`: `!` <-> `F`, `#` <-> `D`
- `GraphicsModeWordCase`: for B/BF/F and Put method words; reserved words also follow KeywordCase (more specific wins).

### Statement separator `:` and one-line blocks
- `StatementSeparatorStyle`: `Leave`, `SplitSafe`, `SplitAll`, `Join`. `SplitSafe` splits top-level `:` except those inside one-line `If` or one-line blocks (detected via `blockRanges` and structure). `SplitAll` splits those too - needs `AllowShort... Never`. Start conservative.
- `AllowShortIfStatementsOnASingleLine`: `Leave`/`Never`. One-way: `Never` expands to block; never merges block to one-line.
- `AllowShortBlocksOnASingleLine`: same for For/Next, While/Wend, Do/Loop, Select Case/End Select, With/End With, Type/End Type, Enum/End Enum, Namespace/End Namespace. One-way.

### Calls and parentheses (graphics-aware)
- `CallParentheses`: `Leave`/`Add`/`Remove` for statement-position `Sub` calls only. Never touch Function in expression, niladic calls (see `NiladicCallParentheses`), or names in `CallParenthesesExclude`.
- `CallParenthesesExclude` defaults to graphics statements: Circle, Draw, Get, Line, Paint, Preset, PReset, Pset, Put, View, Window. Their coordinate-pair grammar has no parenthesized form in the statement-call position as specified.
- Graphics: `CoordPairSeparatorSpacing` for `-` in pairs; `StepPrefixSpacing` for `Step(x,y)` vs `Step (x,y)`; `SpaceBetweenEmptyArguments` for `,,`; never touch `Draw` command string or `Paint` pattern string; empty slots stay put.

## 5. LSP integration

### Capability negotiation
In `FreeBasicServer::initialize` (or where capabilities are built), advertise:
- `textDocument.formatting.provider = true` (documentFormattingProvider)
- `textDocument.rangeFormatting.provider = true` (documentRangeFormattingProvider)
- `textDocument.onTypeFormatting` - **defer** (not advertised in first pass). Can add later when implemented.

LspCpp types exist (`td_formatting`, `td_range_formatting`).

### Handlers
Register handlers in `FreeBasicServer` constructor via `server.on<...>(...)` following existing patterns.

New module: `src/format/format_lsp.{h,cpp}` with pure functions that:
- Take LSP request params and produce `std::vector<lsp::TextEdit>` (or LspCpp edit types)
- Convert URIs via existing mechanisms; get document content from `WorkingFiles` (or from analysis cache - server has access)
- Load config for the document path (respecting precedence: config file > client options only if no config file)
- Run formatter on content
- Compute text edits: if content unchanged, return empty vector. Otherwise return a single full-document replace edit (`range` is whole document) or range-based edits. For full formatting, replace from start to end.
- For range formatting (question 3 deferred in scope? question 3 says defer range focus to full file first; but LSP method exists - implement basic range: format that range as best-effort, but be conservative; if cross-boundary issues possible, better to return no edits or format whole? Or implement minimal range support. But user says "focus on full file support first" - so implement full formatting thoroughly, range formatting can be a simple wrapper that extracts range text, formats it in isolation? But that breaks continuations and indentation context. Better to implement full-document first and have rangeFormatting return full-document edits if that’s safest, or implement proper range. But easier to start with full-document only in LSP and add range later. But advertise rangeFormatting.provider true? Maybe implement by formatting whole doc and returning single edit - that's correct if whole doc changes. Or compute minimal diff - but not necessary. Start with full-document formatting working well.)

But question 3: "i would say let's defer this and focus on full file support first" - so focus implementation effort on full file; LSP can initially implement `textDocument/formatting` fully and `textDocument/rangeFormatting` can return empty or format whole document (conservative). Or skip implementing range until later. But better to have the method registered and working conservatively.

### Config precedence in LSP
When handling formatting request:
1. Find config for document URI/path: check `.fbformat` up from dir, else `freebasicd.toml` at index root. 
2. If any config file found → load resolved config from files/presets (ignore client `FormattingOptions` except maybe not present). Client options are in `params.options` but we must not apply them.
3. If no config file found → build config from client `FormattingOptions` (map as above) plus FBdocs defaults for others? Or treat as "no project config" case - apply client options mapping.
4. Pass resolved config to engine.

This matches "A configured project is not silently re-tabbed by an editor whose defaults are spaces."

## 6. Presets

Create preset files in repo:
- `styles/FBdocs.toml`: encode FBdocs style from research (KeywordCase Title, TypeNameCase Title, IntrinsicCase Title, Dim As T x style preference? Research says type-first dominates; set DimTypePosition appropriately. Indent with tabs, 80 column limit, tight parens.)
- `styles/fbc.toml`: compiler style (lowercase keywords, specific paren spacing, tabs for continuation)
- Also support `inherit` in preset files.

Built-in defaults embedded in code as fallback if files missing.

## 7. Testing

### Unit tests (existing test style)
Add to `tests/` as appropriate:
- Config precedence: `.fbformat` overrides `freebasicd.toml`; client options ignored when config exists; inheritance order (child overrides parent, ancestor base)
- Case transforms with catalogs
- Block comment preservation
- Format-off pragma behavior
- Idempotence
- Graphics exceptions (exclude list, coordinate pairs)

### Session integration tests
Add `tests/session_formatting_checks.cpp` (one feature file per AGENTS.md rule):
- Basic full formatting (indent, spacing)
- Keyword case application
- Config file discovery affects behavior
- Format-off pragma
- Empty edits when already formatted (idempotent)
- Block comment preserved

Register in:
- `CMakeLists.txt` in `session_integration` target sources
- `tests/session_integration.cpp`: declare `void RunFormattingTests();` and call it

### CLI tests (optional but useful)
Smoke test via shell: format sample file, check output, `--check` behavior.

## 8. Implementation order

1. **Scaffolding**: Create `src/format/` dir, add `freebasicd_format` lib to CMake, add `fbformat` exe, add preset files.
2. **Config**: Implement config loader with discovery, preset loading, single-parent inheritance, cycle detection, client options mapping, defaults (FBdocs as default preset).
3. **Lexer audit + block comments**: Extend lexer to handle `/'...'/` block comments losslessly. Add token stream for formatter.
4. **Core engine minimal**: Indentation, spacing basics, case transforms, preserve directives/macros/metacommands and comments/strings. Full-document formatting.
5. **CLI**: Wire up fbformat_main with flags.
6. **LSP**: Add formatting handlers (full document) and wire into session. Advertise capabilities.
7. **Wrappers/continuations**: Line breaking with `_`, `ContinuationStyle`, `BreakBinaryOperators`. Be conservative initially.
8. **Safety features**: Format-off regions, graphics exceptions, one-way block expansion.
9. **Tests**: Integration tests + unit tests. Verify idempotence on corpus.
10. **Polish**: Range formatting if time (or defer), ensure clang-format clean.

## Notes

- **Preserve invariants**: Never rewrite string sigils, Print separators, `ElseIf`, `_` insertion/removal except via explicit break options, metacommands, enum trailing commas (Leave), `#include` order (SortIncludes defaults false), Draw/Paint strings, empty graphics slots.
- **Structure-aware**: Use `ParseResult.blockRanges` for `SplitSafe` and one-line block detection. The formatter needs parser view - that's fine (single file parse is cheap).
- **No reparse on every tiny thing**: Full-document format does one parse.
- **Logging**: Follow existing pattern (stderr for server; CLI can be quiet by default).
- **Cycle detection**: For `inherit` chains, track visited set and error if cycle found (treat as config error, fall back to defaults conservatively).
- **Cross-platform**: Reuse `normalizePath`, path handling as in index/settings.
**Status update (2026-10-16):** Phase 1 complete (scaffolding/config). Phase 2 complete (formatter tokenizer with block comment support, lossless block comment preservation, CLI builds, LSP formatting handlers wired and advertised). All existing tests pass. Remaining: Phase 3+ core engine (indentation/spacing/case transforms), format-off regions, CLI full flags, integration tests.
**Status update (2026-10-18):** `fbformat`'s hand-rolled argument parsing replaced with argparse (vendored submodule `third_party/argparse`, v3.2). Same flag surface, but unknown flags now fail with usage and exit 2 instead of being silently ignored, and unreadable/unwritable files report an error and exit 2 instead of silently formatting empty content (which would have become a data-loss bug once the engine emits real output). `--config`/`--style` remain parsed-but-not-honored until the config layer grows override support. Remaining: Phase 3+ core engine (indentation/spacing/case transforms), format-off regions, CLI full flags, integration tests.
