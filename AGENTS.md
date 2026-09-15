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
- **hash_sha256** (github.com/imahjoub/hash_sha256, header-only C++11 SHA-256)
  is vendored as a **git submodule** at `third_party/hash_sha256`, **pinned to
  commit `ad118c66b7f5b8ffb5119d0f0104724d0d6db14a`** (main, 2026-09). The
  index keys every persisted source file and workspace by SHA-256 hex digest
  of the normalized path; do not swap it for another hash or hasher.
- **No simdjson** (dropped). LspCpp handles all protocol JSON via its bundled
  RapidJSON. Do not add simdjson back for protocol work.
- Requires CMake 3.16+ and C++17.

## FreeBASIC facts (encode these in the lexer/parser)

**`FreeBASIC.md` is the single source of truth for the language.** Read it
before touching the lexer/parser. Anything already encoded in
`src/language.cpp` (keyword catalog, block closers, wiki URLs) is data, not to
be re-derived. Provenance: wiki pages + fbc 1.10.2 probes; known divergences
between this implementation and real `fbc` are tracked in FreeBASIC.md §12 and
must stay there. Encoding directives that the lexer/parser must honor:

- Identifiers are **case-insensitive**; canonical lookup key = lowercase name
  including any trailing type-suffix char. Suffix chars: `$` STRING, `%`
  INTEGER (not SHORT — see FreeBASIC.md §2), `&` LONG, `!` SINGLE, `#` DOUBLE;
  `@` is the address-of operator, not a suffix. In `fb` mode fbc ignores
  identifier suffixes (warning 44) and aliases `foo`/`foo$` — one symbol.
- Built-in types, block closers, `END` vs `END IF`, `WEND`/`NEXT`/`LOOP`,
  line continuation `_`, `:` separators, comment forms (`'`, `REM`, nestable
  `/'...'/`), `#`-preprocessor, `$`-metacommands, dialect gating, and scope
  rules (module-level plain `Dim` is **not** visible inside procedures — only
  `Shared`/`Common Shared`): all in `FreeBASIC.md` (§1–§11), verified there.
- Doc comments: `///` and `''` lines directly above a declaration → hover
  text. This is our convention, not a language feature (FreeBASIC.md §5).
- `.` member access, `->`, and the combined assignments `AND=`/`OR=`/`XOR=`/
  `EQV=`/`IMP=`/`MOD=`/`SHL=`/`SHR=` are operators, never identifier parts.
- Dialects: only `fb` is implemented; `#LANG "<name>"` and the `$lang`
  metacommand both set the dialect, recorded in `ParseResult.lang`; non-`fb`
  gets a best-effort `fb` parse plus one `lang-mode` Information diagnostic.

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

### clang-tidy

Baseline config lives at repo root `.clang-tidy`; `src/` is expected to be
**zero-diagnostic** under it. Run:

```
cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_COMPILER=clang++
clang-tidy -p build-tidy --quiet src/lexer.cpp src/language.cpp src/parser.cpp \
    src/resolve.cpp src/index.cpp src/session.cpp src/utf16.cpp src/main.cpp
```

Gotchas learned the hard way (2026-09):

- The system `clang-tidy` (LLVM 22) is built with **no checks enabled** — a
  bare run without `-checks`/`.clang-tidy` aborts with "no checks enabled".
  Disable checks by *adding* `-checks` values; `.clang-tidy` `Checks:` and
  `WarningsAsErrors:` are the committed source of truth.
- The checked-in `build/compile_commands.json` is **not** usable for tidying
  (stale: missing `-std=c++17` and rapidjson include, plus third-party
  ixwebsocket entries). Using it makes clang-tidy fail to parse the files
  (`std::string_view`/`std::filesystem` "no member" errors). Always re-create
  the throwaway `build-tidy/` DB above (gitignored via `build*/`).
- Default `-header-filter` floods output from vendored headers (LspCpp,
  rapidjson). `.clang-tidy` restricts to `src/[^/]*\.h$` — our headers live
  directly under `src/`, third-party does not. Add new src headers without
  touching this.
- `WarningsAsErrors` gates bug-class checks (`bugprone-*`, `cert-*`,
  `clang-analyzer-*`, `performance-*`) — a real finding exits non-zero. Do not
  widen the suppression list (`-readability-*`, `-misc-*` entries in
  `.clang-tidy`) to silence a new bug-class hit; those entries exist for
  deliberate conventions only, each with its reason in the config.
- Auto-fix pitfall: `--fix` with `readability-braces-around-statements`
  mangles the codebase's compact single-line `if`s; run `--fix` only on a
  curated safe subset (e.g. `misc-const-correctness` plus trivial readability
  categories) and review the diff. Braces style is intentionally silenced in
  the config instead.
- 2026-09 cleanup already applied: uint→int narrowing in folding ranges,
  duplicated switch/case-`if` branches, an ignored `snprintf` result,
  `std::move` of a trivially-copyable type, `path`-by-value params made
  `const&`, const-correctness pass, loop→`std::any_of`/`find_if` conversions.
  Visual check: exit 0, zero diagnostics, `ctest` 7/7 green.

## Git workflow

- **Commit automatically when a milestone is achieved.** Do not wait for an
  explicit commit request. A milestone is achieved when its acceptance criteria
  pass: the milestone's tests are green (`ctest` / `cmake --build`) and the
  milestone deliverable (e.g. lexer/parser, an LSP feature, a docs refresh) is
  complete. Commit even if the milestone is "small"; never commit half-finished
  or failing work.
- Keep the working tree clean between milestones: stage only intended files,
  never build artifacts or secrets, and write a short conventional `scope:`-style
  message summarizing what the milestone delivers (see PLAN.md §5 forward plan, §7 acceptance).
- Before committing, quickly review `git status` / `git diff` so the commit
  contains exactly the milestone's changes, nothing stray.
- The repo lives on `main`; push only when asked.
