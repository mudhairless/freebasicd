# Formatting (fbformat)

`freebasicd` includes `fbformat`, a FreeBASIC formatter, and also provides `textDocument/formatting` / `textDocument/rangeFormatting` over LSP. This document describes what the formatter does, how configuration works, and how to use the CLI.

## What it does (current scope)

The formatter is conservative and preserves things that cannot be unambiguously rewritten:

- **Indentation**: statement blocks (Sub/Function/Property/Operator/Constructor/Destructor, If/ElseIf/Else/End If, Select/Case/End Select, Do/Loop, While/Wend, For/Next, With/End With, Type/End Type, Union/End Union, Enum/End Enum, Namespace/End Namespace, Scope/End Scope, SplitSafe variants). `If cond Then stmt` on one line does not open a new block.
- **Case**: configurable per category (`KeywordCase`, `TypeNameCase`, `IntrinsicCase`, `BooleanLiteralCase`, `DirectiveCase`, `MetacommandCase`). `Canonical` uses the documented spellings (ElseIf, EndIf, LBound etc). `sub`/`function` are treated as types only after `as` (so `Sub Foo()` stays a procedure keyword).
- **Spacing**: spaces around binary operators, after commas, before trailing comments (configurable), sign-before-paren preserved in tightness for graphics coordinate pairs (`Line -(10,10)`); adjacent commas (empty argument slots) keep author spacing; Print semicolons remain tight; combined assigns (`and=`) stay one token.
- **Preservation**: string literals verbatim; `''` line comments verbatim; `/'...'/` block comments preserved losslessly (interior copied byte-for-byte); `#` directives and metacommand comments kept at column 0 in their raw form; directive macro bodies that continue with `_` are treated as raw verbatim on the following line(s); `#macro ... #endmacro` bodies and `Asm ... End Asm` bodies are copied byte-for-byte (the delimiter lines are still formatted, and body lines do not affect indentation or blank-line accounting); lines continuing a logical line via `_` (continuation lines) are copied byte-for-byte (Leave). Numbers (`1.5`, `.25`, `1.`, `&H1F`, `&hFF`) are not re-spelled.
- **Blank lines**: runs collapsed to `MaxEmptyLinesToKeep` (default per preset). Trailing whitespace trimmed (when enabled). EOF gets a single newline (when enabled). Goto labels keep their position per `IndentGotoLabels`.
- **Disable/enable**: the engine returns bytes identical when `DisableFormat = true` in config. A line whose only significant token is the off pragma (`FormatOffPragma` in the presets; the matching name is `FormatOnPragma`) opens a verbatim region: the lines from the pragma line through the on pragma line are copied byte-for-byte, and an unclosed region runs to the end of the file.
- **Exceptions respected**: never rewrites string sigils, Print separators, metacommand comments, enum trailing commas (Leave), empty graphics slots, Draw/Paint strings. `#include` order is not sorted (defaults false).

## Configuration

The formatter discovers configuration by walking up from the file being formatted (CLI or LSP). Precedence:

1. If `.fbformat` exists in the document's directory or any ancestor, use it and ignore `freebasicd.toml` in those ancestors for formatting (with a warning, to stderr in the CLI and to the client log over LSP, if both exist in the same directory).
2. Otherwise, look for `freebasicd.toml` in the document's directory or any ancestor (uses `[format]` table).
3. If neither found, load the default preset `FBdocs`.

`.fbformat` is a TOML file with a `[format]` section. It may `style = "FBdocs"` (or `inherit`) to inherit a preset, and then override specific keys. `freebasicd.toml` may also contain a `[format]` table.

A config file that is found but cannot be read or parsed is an error, not a reason to fall back: the CLI exits with code 2, and the server answers a formatting request with no edits and logs the reason. The same applies to a `style` name that does not resolve, to a preset file that cannot be read or parsed, and to a preset inheritance cycle. Only the implicit `FBdocs` default in step 3 may fall back to the built-in factory defaults, and only when the preset itself cannot be loaded.

### Presets

- **FBdocs** (default): KeywordCase/TypeNameCase/IntrinsicCase = `"Canonical"` (documented spellings). UseTab = `"ForIndentation"`, IndentWidth=4, TabWidth=4, TrimTrailingWhitespace/TrimFinalNewlines/InsertNewlineAtEOF true, SpaceBeforeParens = `"Never"`, SpaceAroundBinaryOperators true, SpaceAfterComma true, SpaceBeforeComma false, ContinuationStyle `"Leave"`, ColumnLimit 80, etc. See `styles/FBdocs.toml`.

Additional presets live under `share/freebasicd/styles/` at install time and `styles/` in the source tree. A preset named by a config file is also searched in that file's own `.styles/` directory, in `$HOME/.config/fbformat/styles/`, and on Windows in `%USERPROFILE%\fbformat\styles\`. (Earlier revisions of this page said `%APPDATA%`; the code has never read `APPDATA`.)

### Key options (honored by the engine today)

These are the fields the current engine reads (see `src/format/engine.cpp`); the
rest of `src/format/config.h` is parsed and stored but not yet consumed — treat
those as reserved.

- Case: `KeywordCase`, `TypeNameCase`, `IntrinsicCase`, `BooleanLiteralCase`,
  `DirectiveCase`, `MetacommandCase` — each `"lower"|"UPPER"|"Title"|"Canonical"|"Leave"`
- Indentation: `UseTab` (`"Never"|"ForIndentation"|"ForContinuationAndIndentation"|"Always"`),
  `IndentWidth`, `TabWidth`, `IndentPPDirectives`, `IndentGotoLabels`
- Spacing: `SpaceAroundBinaryOperators`, `SpaceAfterComma`, `SpaceBeforeComma`,
  `SpaceAfterStatementSeparator`, `SpaceAfterUnaryOperators`, `SpaceBeforeAssignmentOperators`,
  `SpaceBeforeColon`, `SpaceAroundMemberAccess`, `SpacesBeforeTrailingComments`,
  `StepPrefixSpacing`, `SpaceBeforeParens` (`"Never"|"ControlStatements"|"Always"`),
  `SpaceBeforeParensAfterControlStatements`,
  `SpaceBeforeParensAfterFunctionDeclarationName`,
  `SpaceBeforeParensAfterFunctionCallName`,
  `SpacesInParensInConditionalStatements`, `SpacesInParensInEmptyParentheses`,
  `SpacesInParensInParameterLists`, `SpacesInParensInCallParens`
- Layout: `MaxEmptyLinesToKeep`, `KeepEmptyLinesAtStartOfFile`, `LineEnding`,
  `TrimTrailingWhitespace`, `TrimFinalNewlines`, `InsertNewlineAtEOF`
- Safety: `DisableFormat`

`ColumnLimit`, `BreakBinaryOperators`, `AlignEscapedNewlines`,
`StatementSeparatorStyle`, `NamespaceIndentation`, `IdentifierCase`, and the
`NumericLiteralCase*` switches are declared in `config.h` but the engine is
conservative today and does not yet act on them; they are reserved for a future
wave.

See `src/format/config.h` for the full field list with defaults, and the shipped
presets for a worked example. Client LSP formatting options (`tabSize`,
`insertSpaces`, `trimTrailingWhitespace`, `insertFinalNewline`,
`trimFinalNewlines`) are mapped at the session boundary, and they apply only
when no config file was found for the document. A config file, when one exists,
wins outright; client options are not merged over it.

## CLI (`fbformat`)

The CLI reads FreeBASIC source and writes formatted output. Without `--in-place`
or `--check`, output goes to stdout; with `--in-place` the files are rewritten
in place; with `--check` nothing is written and the exit code reports whether
changes were needed.

| Flag | Meaning |
|---|---|
| `-i`, `--in-place` | Write formatted output back to the file(s) |
| `--check` | Exit 1 if any file would be reformatted, 0 if unchanged |
| `--stdin-filepath <path>` | Path used for config discovery when reading from stdin |
| `--config <file>` | Explicit config file, skipping discovery; an unreadable or invalid file is an error (exit 2) |
| `--style <name>` | Preset name, overriding the config file's style; an unknown name is an error (exit 2) |
| `files...` | Files to format; reads stdin when no file is given, or when a file argument is `-` |

Exit codes: 0 = formatted or unchanged, 1 = changes needed under `--check`,
2 = error (usage, config, I/O).

Examples:

```sh
# Format a file to stdout
./build/fbformat src/main.bas

# Format in place
./build/fbformat -i src/main.bas

# Check if formatted (exits 1 if any file needs reformatting)
./build/fbformat --check src/*.bas

# Read from stdin, write to stdout; --stdin-filepath picks the config to use
cat src/main.bas | ./build/fbformat --stdin-filepath src/main.bas -
```

Notes: reading from stdin requires one of `--stdin-filepath`, `--config`, or
`--style`, so the configuration is never guessed; a bare stdin read is an error
(exit 2), as is a repeated `-` or `-` mixed with file arguments. Writing more
than one file to stdout is also an error: use `--in-place` or `--check` for
multiple files.

## LSP

The server advertises `documentFormattingProvider` and `documentRangeFormattingProvider`.
- `textDocument/formatting` returns a single full-document `TextEdit` when the buffer differs from formatted output, else `[]`.
- `textDocument/rangeFormatting` returns a single `TextEdit` confined to the lines the range touches. The range expands to whole lines, back to the head of a `_` continuation chain and forward to its end, and across a block comment it overlaps; blank lines at the edges are dropped, and a range over only blank lines returns `[]`. The lines before the range set the indent the range opens with, and a mid-document range does not normalize the end of the file.
- The handler resolves config by walking from the document URI. A broken config is logged and answered with no edits. Client formatting options apply only when no config file is found (see Configuration).
