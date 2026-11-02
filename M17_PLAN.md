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

**Status update (2026-10-18, engine wave):** Phase 3+ core engine complete: real `formatDocument` in `src/format/engine.cpp` (indent stack over fblang block tables, spacing rules, case transforms by specificity, verbatim preservation for comments/strings/directives/macro bodies/continuation lines, CRLF/LF derivation, disableFormat, blank-line collapsing). Canonical spelling table (`src/format/spelling.{h,cpp}`, 206 entries) wired into `applyCase`; FBdocs preset uses `"Canonical"` for keyword/type/intrinsic case. `sub`/`function` are position-aware (`isTypeWordAt`): types only after `as`. Fixed a tokenizer OOM root cause (float/hex literals created a zero-length token and looped until memory ran out — `1.5`, `.25`, `1.`, `&H1F` now lex and round-trip; regression-tested). LSP: `onFormatting` converts bytes to UTF-16; `onRangeFormatting` passes the request's real range (clamped), deferring to a full-document edit for now. Tests: new `format_engine_checks` unit suite (23 checks: indentation, spacing, case, preservation, graphics pairs/slots, CRLF derivation, disableFormat, spelling-table contract, idempotence) and new `session_formatting_checks.cpp` integration feature file (full formatting, already-formatted → empty edits, `.fbformat` discovery, block-comment preservation, range-deferral, unknown-document-empty), registered in both `CMakeLists.txt` and `tests/session_integration.cpp`. Full tree builds; `ctest` green (22/22); clang-format clean. Remaining: format-off regions (safety features), real range formatting (currently defers to full-document edit), CLI full flags beyond argparse, idempotence verification on corpus.

**Status update (2026-10-30):** Wave 2 is planned in §9; no code has been written yet. The "remaining" lists in the status updates above are superseded by §9.

**Status update (2026-11-01):** Wave 2 Phase 1 and Phase 2 are complete. `511558a` extracted the line model (`classifyLine`, `applyLineToStack`, `Formatter`, `emitLine`) with no test changes and byte-identical corpus output. `6632ef3` closed the four soundness findings in §9.1: format-off/on regions, `#macro ... #endmacro` bodies, and `Asm ... End Asm` bodies are now copied byte for byte (delimiters still formatted, raw bodies inert to the block stack), and a bare `End` is the program-termination statement instead of a closer. Full clean build, `ctest` 22/22, clang-format gate, and 59/59 corpus idempotence are green. Remaining in this wave: Phase 3 (honest config resolution, `--config`/`--style`, client options, corpus gate, i18n recursion) and Phase 4 (true range formatting). The wave's `CHANGELOG.md` and `Formatting.md` close-out stays with §9.8.

**Status update (2026-11-02):** Wave 2 Phase 3 is complete. `6558fc0` replaced `loadConfigForPath` with `resolveConfig` and `configForStyle`, made a broken config, an unknown style, and an inheritance cycle errors instead of silent fallbacks, honored `fbformat --config` and `--style`, applied client `FormattingOptions` only when no config file exists, added `format_corpus_checks` as the 23rd ctest suite, and made the i18n extraction and freshness check recurse into `src/format`. Ebben's gate is green: a full clean build, `ctest` 23/23, and the clang-format gate with no output. Remaining in this wave: Phase 4 (true range formatting). The wave's `CHANGELOG.md` and `Formatting.md` close-out stays with §9.8.

## 9. Wave 2 (in progress): trust the inputs, bound the output

The engine now formats and round-trips: a full clean build is green, `ctest` is 22/22, all 59 corpus files are idempotent, and a significant-token comparison over the corpus passes today. The remaining work is not "more spacing rules" — it is making the formatter refuse the inputs it cannot trust and leave alone the regions it cannot model, then replacing the range-formatting deferral. Wrapping (`ColumnLimit`, `ContinuationStyle`, `_` insertion) is deliberately the wave *after* this one: it needs a penalty/line-break model and parser block ranges, and it is much safer once the corpus gate below exists.

Four phases, each independently verifiable and committable. Phase 1 is a pure refactor with no test changes; Phases 2–4 add behavior. **Phases 1, 2, and 3 have landed** (commits `511558a`, `6632ef3`, and `6558fc0`); Phase 4 remains.

### 9.1 Findings (code as of 2026-10-30)

Phase 2 closed the four soundness rows in the first block. Phase 3 closed every config, CLI, corpus, and i18n row below it. Real range formatting is the only row left, and it is Phase 4 work.

| Item | State today | Evidence |
|---|---|---|
| Format-off/on pragma regions | **closed in Phase 2**: the pragma line, the interior, and the on-line are copied raw; unclosed runs to EOF; nested off is state, not depth; an empty pragma never matches | `formatDocument`'s raw-region block; `TestFormatOffRegionPreserved`, `TestFormatOffToEof` |
| `#macro ... #endmacro` bodies | **closed in Phase 2**: body raw, delimiters still formatted, body never touches the stack | `preproc_macro.bas` keeps `    print w`; `TestMacroBodyVerbatim` |
| `Asm ... End Asm` bodies | **closed in Phase 2**: opener and closer format normally, interior raw and inert to the stack | `blocks_asm.bas` keeps `    mov eax, 0`; `TestAsmBodyVerbatim` |
| Bare `End` statement | **closed in Phase 2**: program termination, `Lead::Plain`, keeps the body indent, no `Scope` pop; `End As Integer` still a field | `TestBareEndIsAStatement` |
| Real range formatting | returns a full-document edit | `format_lsp.cpp:20-28`; the session test pins the deferral |
| `--config` / `--style` | **closed in Phase 3**: honored through `ConfigQuery`; a config error is fatal in the CLI | `fbformat_main.cpp` `configFor`; `TestExplicitConfigAndStyle` |
| Client `FormattingOptions` | **closed in Phase 3**: applied only when no config file was found | `formattingConfigFor` in `src/session.cpp`; `TestClientOptionsAppliedWhenUnconfigured`, `TestClientOptionsIgnoredWhenConfigured` |
| Malformed `.fbformat` | **closed in Phase 3**: `resolveConfig` returns `ok=false`; the session returns empty edits and the CLI exits 2 | `TestMalformedConfigIsRefused`, `TestInvalidConfigAnswersNoEdits` |
| `.fbformat` shadow warning | **closed in Phase 3**: the notice is a `warnings` entry that the caller logs; the library writes no stderr | `TestShadowWarning` |
| `-` as stdin | **closed in Phase 3**: `-` reads stdin; stdin with no `--stdin-filepath`, `--config`, or `--style` exits 2 instead of discovering from the cwd | `fbformat_main.cpp` stdin handling |
| Multiple files to stdout | **closed in Phase 3**: exits 2 unless `--in-place` or `--check` is given | `fbformat_main.cpp` argument checks |
| Corpus idempotence | **closed in Phase 3**: gated as the 23rd ctest suite, over both shipped presets | `tests/format_corpus_checks.cpp` |
| i18n extraction | **closed in Phase 3**: the extraction glob is `GLOB_RECURSE` and `i18n_checks` walks `src/` recursively; the pot is regenerated | `CMakeLists.txt` po-template block; `po/freebasicd.pot` dated 2026-11-02 |

### 9.2 Locked decisions

| Question | Decision | Why |
|---|---|---|
| Config file exists but is unreadable/invalid TOML | refuse to format (empty edits in LSP, exit 2 in CLI) | "config presence is authoritative" is the plan's own rule; defaults would silently re-tab the project |
| Unknown style (config file or `--style`) | error, not FBdocs | a typo must not degrade silently |
| Off-pragma lines themselves | copied raw | the region is "untouched"; easiest to reason about |
| Macro/asm delimiter lines | formatted; bodies raw | keeps canonical case on visible delimiters, protects opaque content |
| Range edits | confined to expanded whole lines | clients clamp; a whole-document edit from a range request is the current bug |
| Client options | only when no config file exists | §1 precedence |
| Range boundary units | expand to whole lines, `_` chain, and block-comment token; trim blank edges | never split a unit the engine treats as one |

### 9.3 Phase 1 — Extract the line model (pure refactor, no behavior change) — **done** (`511558a`)

`src/format/engine.cpp` is one 400-line loop with classification, stack update, and emission interleaved. Phases 2 and 4 need the same classification, so extract it once instead of duplicating it:

- `LineShape classifyLine(Line, tokens)` → `{blank, firstKind, w0, w1, wLast, lead, closerInfo, labelLine, continues}`.
- `applyLineToStack(stack, LineShape)` → the current block-stack update.
- `emitLine(Formatter &, LineShape)` → the current per-line emission; `Formatter` owns the output string, verbatim spans, pending blanks, continuation state, and the `Emitter`'s per-line state.
- `formatDocument` becomes an advance/emit loop over every line.

Acceptance: **no test file changes**; all 22 suites green; byte-identical output on the corpus; clang-format clean. This is the "prove the refactor before adding behavior" commit.

Landed as `511558a`, touching only `src/format/engine.cpp`; corpus output was byte-identical to the pre-refactor baseline.

### 9.4 Phase 2 — Verbatim regions and the `End` fix — **done** (`6632ef3`)

All in `src/format/engine.cpp` + `tests/format_engine_checks.cpp`:

1. **Format-off/on.** A comment token containing `config.formatOffPragma` opens a region; the line containing it, every line until the line containing `config.formatOnPragma`, and the on-line itself are copied from `line.begin` (original indent included) through `appendVerbatim`, so the trailing-whitespace pass leaves them alone. Unclosed off → EOF. Nested off is state, not depth (ignored). Guard empty pragma strings — `find("")` matches everything.
2. **Macro bodies.** A `#macro` directive opens a region; body lines are raw until `#endmacro`. Delimiter lines still go through the directive path so `DirectiveCase` applies. Body lines must not touch the block stack: a body containing `loop` must not pop a `Do`.
3. **Asm bodies.** `Asm` opener and `End Asm` closer format normally; interior lines are raw and do not update the stack.
4. **Bare `End`.** `w0 == "end" && w1.empty()` is the program-termination statement, not a closer: `Lead::Plain`, no dedent, no `Scope` pop. Keep the existing `w1 == "as"` field guard (`End As Integer`).

Tests: `TestFormatOffRegionPreserved`, `TestFormatOffToEof`, `TestMacroBodyVerbatim`, `TestAsmBodyVerbatim`, `TestBareEndIsAStatement`; extend the idempotence fixture. One session test in `tests/session_formatting_checks.cpp` proving a pragma region survives the wire while the code around it formats. Adding it touches that one feature file (function + `RUN_TEST` line), per the AGENTS.md rule.

Landed as `6632ef3`. The empty-pragma guard is asserted inside `TestFormatOffRegionPreserved` rather than as its own test; the session test is `TestFormatOffRegionSurvivesTheSession`. Corpus output moves only where the old engine rewrote opaque bodies — `blocks_asm.bas`, `preproc_macro.bas`, `preproc_macro_dead.bas` — and all 59 files remain idempotent.

### 9.5 Phase 3 — Honest config resolution, CLI flags, client options, corpus gate — **done** (`6558fc0`)

**Config API** (`src/format/config.{h,cpp}`): replace `loadConfigForPath` with

```cpp
struct ConfigQuery { std::string configFile; std::string style; };
struct ResolvedConfig {
  FormatConfig config;
  std::string configFile;      // the file used, empty if none
  bool foundConfigFile = false;
  bool ok = true;              // false => do not format
  std::string error;           // reason when !ok
  std::vector<std::string> warnings;
};
ResolvedConfig resolveConfig(std::string const &filePath, ConfigQuery const &q = {});
ResolvedConfig configForStyle(std::string const &name, std::string const &configDir = {});
```

Rules: unreadable or invalid TOML → `ok=false`; unknown style from the config or `--style` → `ok=false`; the `.fbformat`-shadows-`freebasicd.toml` notice becomes a `warnings` entry instead of a library `fprintf`. Migrate all callers (session, CLI, tests) and delete `loadConfigForPath`; no compatibility wrapper.

**Session** (`src/session.cpp`): both formatting handlers call `resolveConfig`; on `!ok`, `logToClient` the error and return **empty edits** (never format against a guessed config); on `!foundConfigFile`, apply client options, mapping the optional fields with `value_or(cfg.…)`; log warnings. Do not turn a config error into a protocol error — an editor would show a notification on every save.

**CLI** (`src/format/fbformat_main.cpp`): honor `--config` and `--style`; `-` means stdin; stdin without `--stdin-filepath` and without `--config`/`--style` exits 2 instead of discovering from the cwd; config errors exit 2; more than one file to stdout without `-i`/`--check` exits 2 (today it concatenates documents).

**Corpus gate** (`tests/format_corpus_checks.cpp`, new target, 23rd ctest suite): over `tests/corpus/*.bas`, with `configForStyle("FBdocs", CORPUS_DIR)` and the same for `fbc`:
- `format(format(x)) == format(x)`;
- significant token streams match: exact bytes for strings/comments, exact for identifiers **unless** the word is in the language catalogs, case-insensitive otherwise;
- failure names the file and the first differing token.

Tests in `format_config_checks.cpp`: explicit config/style overrides, unknown style error, malformed `.fbformat` → `ok=false`, warning vector, `foundConfigFile` false in an empty dir, `configForStyle`. Session tests: client options applied when unconfigured (spaces, `tabSize` 2), ignored when a `.fbformat` exists.

**i18n plumbing.** New log messages live in `src/format/`, which the pot extraction and `i18n_checks` both miss. Switch `po-template`'s glob to `GLOB_RECURSE` over `src/` and `i18n_checks`'s `directory_iterator` to a recursive iterator, then `cmake --build build --target po-template` and commit the pot. Otherwise the messages are untranslatable and the freshness check cannot see them.

Landed as `6558fc0`. Two points go beyond the sketch above. Preset loading reports its own failures as well: an unreadable preset file, invalid preset TOML, an unknown parent in an inherit chain, and an inheritance cycle all return `ok=false`, rather than a silent stop or a half applied chain, and only the implicit FBdocs default falls back to the factory defaults when it cannot load. The CLI also rejects a repeated `-` and `-` mixed with file arguments. The session side gained one test the sketch did not list, `TestInvalidConfigAnswersNoEdits`.

### 9.6 Phase 4 — True range formatting — **next**

Engine API in `src/format/engine.h`:

```cpp
struct RangeResult {
  size_t replacementStart;
  size_t replacementEnd;
  std::string text;
  bool changed;
};
RangeResult formatRange(content, filePath, config, rangeStart, rangeEnd);
```

Algorithm, using Phase 1's model:

1. Map byte offsets to whole lines; an empty range is the line at the offset.
2. Expand to logical units that must not be split: back to the head of a `_` continuation chain, out to the start/end of a block-comment token, forward to the chain end.
3. Trim blank lines at both edges (a blank-only selection is a no-op).
4. Dry-run `classify/applyStack` over the lines before the window so the first emitted line has the correct indent depth and continuation state; emit the window with `pendingBlank = 0`.
5. Apply `trimTrailingWhitespace` to the window only; do not normalize EOF for a mid-document range.
6. Replace `[lines[first].begin, start of the line after last)`; return empty when the window is unchanged.

`format_lsp.cpp` returns one `TextEdit` from `RangeResult`; the session needs no change (it already converts bytes to UTF-16). Tests: middle-of-block range, indent context from the prefix, block-comment boundary, continuation head, blank-only no-op, already-formatted no-op. In `session_formatting_checks.cpp`, **delete** `TestRangeFormattingDefersToTheWholeDocument` and replace it with a test asserting the edit is confined to the selected lines and that applying it equals full formatting for a fixture whose surrounding lines are already formatted.

### 9.7 Not in this wave

`ColumnLimit` wrapping / `_` insertion, `ContinuationStyle` WrapOnly/WrapAndJoin, `StatementSeparatorStyle` Split*, `CallParentheses`/`NiladicCallParentheses`, alignment options, `OneLineFormatOffRegex`, `CommentPragmas` regex, format-on-type, dialect-aware `IntrinsicDollarSuffix`. These are the next wave: they need the penalty/line-break model and parser block ranges, and the corpus gate should exist first.

### 9.8 Close-out

Per phase: `cmake --build build --clean-first --parallel` (the editor binary, not only the test targets), `ctest --output-on-failure`, the clang-format gate on changed files, and the corpus gate. The final commit adds one dated `CHANGELOG.md` entry with the deviations and the lessons (bare `End`, macro bodies, silent config fallback, `-` in the docs, the i18n subdirectory escape), updates `Formatting.md` (off regions, macro/asm, honored flags, true range formatting, client-option precedence, and the `%APPDATA%`/`%USERPROFILE%` mismatch), and refreshes this file's status line. Push only on request.
