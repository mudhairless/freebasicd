# FreeBASIC LSP Server

Implement an LSP server for the FreeBASIC Language in C++. The server must be
cross platform. Build targets: LSP 3.17 over stdio. The project root is a git
repository (default branch `main`).

## Dependencies

- **LspCpp** (github.com/kuafuwang/LspCpp) is the LSP/JSON-RPC library. It is
  vendored as a **git submodule** at `third_party/LspCpp`, **pinned to commit
  `19150d12c4ae26239d75258ed598ba8ea3587cb7`** (master, 2026-08-21; no release
  tag exists yet). Restore it with `git submodule update --init`. Consumed via
  `add_subdirectory(third_party/LspCpp)` and linked as the `lspcpp` target. No
  Boost is required (`LSPCPP_STANDALONE_ASIO` is the default); build with
  `LSPCPP_BUILD_WEBSOCKETS=OFF`, `LSPCPP_BUILD_EXAMPLES=OFF`,
  `LSPCPP_BUILD_TESTS=OFF`.
- **No simdjson** (dropped). LspCpp handles all protocol JSON via its bundled
  RapidJSON. Do not add simdjson back for protocol work.
- Requires CMake 3.16+ and C++17.

## FreeBASIC facts (encode these in the lexer/parser)

- Identifiers are **case-insensitive**; canonical lookup key = lowercase name
  **including** any type-suffix char. Suffix chars on identifiers: `$` STRING,
  `%` SHORT, `&` LONG, `!` SINGLE, `#` DOUBLE, `@` LONG.
- Keywords form **block structures** closed by `END <keyword>`
  (`SUB ... END SUB`, `FUNCTION ... END FUNCTION`, `TYPE/UNION/ENUM ... END *`,
  `IF ... END IF`, `SELECT`, `FOR`, `WHILE`, `DO`, `WITH`, `NAMESPACE/`MODULE`,
  `SCOPE`, plus preprocessor `#IF..#ENDIF`).
- Line continuation is trailing `_` (whitespace-tolerant); statements split on `:`.
- Comments: `'` to EOL and line-leading `REM`. `'` inside a string is not a comment.
- Doc comments: `///` and `''` lines directly above a declaration → hover text.
- Numbers: decimal; `&H`/`&O`/`&B` radix; floats `1.5`/`1e-5`; optional suffix.
- Strings: `"..."` with doubled `""` as an escaped quote.
- Preprocessor lines start with `#` (`#include once`, `#define`, `#if..#endif`, `#print`).
- `.` member access and `->` are operators, never identifier parts.

## Architecture

- **Persistence/layering**: `lexer` + `parser` + `language` are LSP-agnostic and
  work in **byte offsets**. LSP interaction happens only in `session`; convert
  positions via LspCpp `WorkingFile` line offsets (`utf16.h` helper). All LSP
  positions are UTF-16 code units.
- **LspCpp API**: use `lsp::LanguageSession`; register handlers with
  `server.on(...)`. Request types are `td_<feature>::request/response`,
  notifications `Notify_<Name>::notify`. Buffers via `WorkingFiles`/`WorkingFile`
  (`OnOpen`/`OnChange`/`OnClose`, `GetOffsetForPosition`, `RebuildLineOffsets`).
  Diagnostics are pushed with `Notify_TextDocumentPublishDiagnostics::notify`.
- **Concurrency**: LspCpp runs a parse pool + handler pool + FIFO notification
  thread. Requests can run concurrently with each other; keep `max_workers=2`.
  Guard shared language state with a mutex; take the lock only to snapshot a
  parse, build responses lock-free.
- Capabilities advertise only implemented features; `positionEncoding: "utf-16"`.

## Verification

- Unit drivers in `tests/` via ctest: `lexer_checks`, `parser_checks`,
  `utf16_checks`, and a `session_integration` test driving `LanguageSession`
  with in-memory streams (LspCpp `tests/test_helpers.h`).
- The system `fbc` compiler (1.10.2) is available for ground-truthing ambiguous
  FreeBASIC constructs.
