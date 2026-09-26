# FreeBASIC LSP Server

Implement an LSP server for the FreeBASIC Language in C++. The server must be
cross platform. Build targets: LSP 3.17 over stdio. The project root is a git
repository (default branch `main`).

## Dependencies

- **LspCpp** is the LSP/JSON-RPC library, vendored as a **git submodule** at
  `third_party/LspCpp` from **our fork,
  [github.com/mudhairless/LspCpp](https://github.com/mudhairless/LspCpp)**
  (`.gitmodules` points there; the submodule is pinned to
  `0badddd9d76ae3582d8771b9baa86e9bddcbaf6f` on the fork's `freebasic-lsp`
  branch, which is **upstream `19150d12c4ae26239d75258ed598ba8ea3587cb7`**
  (kuafuwang/LspCpp master, 2026-08-21; no release tag exists yet) **plus five
  commits of ours**: `310e1e6` adding the watched-files registration types
  upstream lacks —
  `lsFileSystemWatcher`/`lsDidChangeWatchedFilesOptions`
  (`workspace/did_change_watched_files.h`), `Registration::registerOptions`
  (`client/registerCapability.h`), and
  `WorkspaceServerCapabilities::didChangeWatchedFiles`
  (`general/lsServerCapabilities.h`) — `f0cd2fa` covering them with
  round-trip/parse tests plus a self-contained `client/registerCapability.h`
  (it calls `DEFINE_REQUEST_RESPONSE_TYPE` from `RequestInMessage.h` and must
  include it directly), and `45846f7` fixing `SemanticTokensEdit` serialization
  (`textDocument/SemanticTokens.h`): the struct now carries the LSP wire shape
  (`start`/`deleteCount` in flat-array elements, `data`) so generic reflection
  is correct at every call depth, instead of the token-count projection whose
  non-template `Reflect` overload only won for direct top-level calls, and
  `50be209` making `textDocument/codeAction` answer with the type the protocol
  defines — the result is `(Command | CodeAction)[]`, i.e.
  `std::vector<TextDocumentCodeAction::Either>`, not
  `std::vector<lsCommandWithAny>`. A `Command` is an id the client *executes*,
  so a server that can only return those cannot hand the client a
  `WorkspaceEdit` to apply; upstream's own reader existed but the matching
  writer did not, so the local commit adds it (`lsCodeAction.h`,
  `src/lsp/lsp.cpp`, mirroring `LocationListEither::Either`) plus
  `lsp_types_roundtrip_tests` coverage of the edit variant, and `0badddd`
  fixing the test's access to the edit's `std::optional` changes.
  Restore with `git submodule update --init`. Consumed
  via `add_subdirectory(third_party/LspCpp)` and linked as the `lspcpp`
  target. No Boost is required (`LSPCPP_STANDALONE_ASIO` is the default);
  build with `LSPCPP_BUILD_WEBSOCKETS=OFF`, `LSPCPP_BUILD_EXAMPLES=OFF`,
  `LSPCPP_BUILD_TESTS=OFF`.
  **Working on the fork**: inside `third_party/LspCpp`, `origin` is
  `mudhairless/LspCpp` (our fork, fetch = HTTPS, push = SSH) and `upstream` is
  `kuafuwang/LspCpp` with its push URL disabled. The checked-out branch is
  `freebasic-lsp` (tracks `origin/freebasic-lsp`); the fork's `master` is left
  at upstream. To pick up upstream changes:
  `git fetch upstream && git switch freebasic-lsp && git rebase upstream/master`,
  then `git push --force-with-lease origin freebasic-lsp` and bump the pinned
  commit here (`.gitmodules` URL is unchanged, so `git submodule update --init`
  keeps working for contributors).
- LspCpp handles all protocol JSON via its bundled RapidJSON. 
- **tomlplusplus** (github.com/marzer/tomlplusplus) is vendored as a **git
  submodule** at `third_party/tomlplusplus`, pinned to `30172438` (v3.4.0).
  Header-only — an INTERFACE target (`tomlplusplus::tomlplusplus`) whose only
  cost is the include dir. It parses the server's `freebasiclsp.toml` config
  file (`src/settings.{h,cpp}`, M11). Restore with
  `git submodule update --init`.
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
- **Index**: each `WorkspaceIndex` is **in-memory only** — nothing is ever
  written to disk. A background scan parses the workspace (plus a debounced
  rescan on watched-file events); open-buffer entries are marked
  `fromDisk=false` so scan's mtime/size cache-hit can never accept a live
  buffer's parse. Since M11 the server owns **one index per workspace root**:
  `indexes_` (a map keyed by normalized root) under a single `indexesMutex_`
  holds the registered client folders that are themselves workspace roots,
  detected roots, and single-file roots. Session handlers snapshot a
  `shared_ptr<WorkspaceIndex>` (via `indexFor` for a path, `allIndexes()` for
  workspace/symbol) and hold it while raw result pointers (member-access
  walks) are in use. Watched-file events route per path to the owning root's
  index and are deduped by owner root; folder add/remove re-key the map.
- **Workspace roots (M11)**: root selection is `chooseIndexRoot`'s priority
  0–5 — the deepest registered marker-root containing the file; a client root
  that is itself a workspace root as-is; else the nearest VCS marker, then the
  nearest `freebasiclsp.toml` (config-file marker), then the source/include
  layout walk (`findSourceLayoutRoot`, up to the drive root / `$HOME`); and
  single-file mode when no client root exists. A file outside every index root
  is served **resolution-only** through the session-root index's on-demand
  closure — never its own index (it would leak into workspace/symbol) and
  never the single-file branch while a client root exists. A detected root
  that replaces the client's is logged to stderr with the signal.
  `src/settings.{h,cpp}` parses `freebasiclsp.toml` (`hasConfigFile` is the
  marker; `Settings` keys with fixed defaults). Each index owns a `Settings`
  snapshot adopted at construction (`ensureWorkspaceIndex` →
  `index->applySettings(settingsForDirLogged(root))`); `workspace/
  didChangeConfiguration` re-reads every root's file (the notification payload
  is ignored — the file is the truth; idempotent), applies per-root changes
  (`reindexIncludeEdges`, no re-parse), reconciles the root's open buffers
  (diagnostics off ⇒ one empty publish then silence, on ⇒ re-publish; an
  `includePaths` change re-resolves the buffers' include edges), and the
  diagnostics / semantic-tokens / inlay-hints / code-action handlers gate on
  `settingsForDocument` with empty-result semantics when a flag is off.
- **Quick fixes (M12)**: `src/code_actions.{h,cpp}` is the LSP-agnostic,
  byte-offset fix layer, registered as a `{diagnostic code, provider}` table
  (`quickFixProviders()`, looked up with `quickFixProviderFor`). A provider is
  a pure function of (diagnostic, `QuickFixContext`); the context is the *only*
  way it reaches workspace state (an index snapshot, the document path, and a
  `resolveInclude` callback wired to `resolveIncludeTarget`). A fix therefore
  never guesses: offer a candidate only when that seam accepts it, or nothing.
  **Adding a fix = one function + one table row**; do not touch `session.cpp`
  or the capability. A fix ships as an LSP `CodeAction` (`title`, `kind:
  "quickfix"`, the diagnostic it answers, and `edit.changes` keyed by the
  request's own URI) — never as a `Command`: a Command is an id the client
  *executes*, so an empty-id Command with the edit in `arguments` lists in the
  lightbulb and does nothing when picked. `context.only` is filtered
  server-side. Diagnostics a fix keys on must be built by the same function the
  publish path uses (`unresolvedIncludeDiagnostics`), or the published range and
  the fix's range drift apart.
- Capabilities advertise only implemented features; `positionEncoding: "utf-16"`.

## Verification

- Unit drivers in `tests/` via ctest: `lexer_checks`, `parser_checks`,
  `utf16_checks`, `settings_checks`, `code_actions_checks`, and a
  `session_integration` test driving
  `LanguageSession` with in-memory streams (LspCpp `tests/test_helpers.h`).
- The system `fbc` compiler (1.10.2) is available for ground-truthing ambiguous
  FreeBASIC constructs.

### clang-format

Formatting is **part of the milestone gate** (unlike tidy): the repo's
`.clang-format` pins the LLVM standard (2-space indent, attached braces,
80-column). Run the check before acceptance / commit on all supported C++
files:

```
clang-format --dry-run --Werror src/*.cpp src/*.h tests/*.cpp tools/*.cpp
```

Apply with `clang-format -i <file>` (or `--lines=start:end` for a region, e.g.
right after `clang-tidy --fix`). Corpus `.bas`/`.diag` files and
`third_party/` are out of scope.

### clang-tidy

On-demand only — not part of the milestone gate. Baseline config lives at repo
root `.clang-tidy`; keep `src/` **zero-diagnostic** under it whenever you do
run it, but don't block a milestone on tidying. Run it when asked or when a
change looks tricky (copy-paste code, heavy templates, new headers):

```
cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_COMPILER=clang++
clang-tidy -p build-tidy --quiet src/lexer.cpp src/language.cpp src/parser.cpp \
    src/resolve.cpp src/index.cpp src/settings.cpp src/session.cpp src/utf16.cpp src/main.cpp
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
- Auto-fix pitfall: the raw `--fix` output is not formatter-clean (one-line
  statements come back as `{ stmt;` with the closing brace at column 0), so
  run `clang-format -i` over the touched lines afterward and review the diff;
  never hand-write a brace shape over the tool's output.
  `readability-braces-around-statements` is enabled (since 2026-09): single-
  statement `if`/`for`/`while`/`do` bodies must be braced in the attached,
  2-space style that `.clang-format` (LLVM) enforces codebase-wide.
- 2026-09 cleanup already applied: uint→int narrowing in folding ranges,
  duplicated switch/case-`if` branches, an ignored `snprintf` result,
  `std::move` of a trivially-copyable type, `path`-by-value params made
  `const&`, const-correctness pass, loop→`std::any_of`/`find_if` conversions.
  Visual check: exit 0, zero diagnostics, `ctest` 7/7 green.

## Git workflow

- **Commit automatically when a milestone is achieved.** Do not wait for an
  explicit commit request. A milestone is achieved when its acceptance criteria
  pass: the milestone's tests are green (`ctest` / `cmake --build`), changed C++
  files are `clang-format` clean (see Verification §clang-format), and the
  milestone deliverable (e.g. lexer/parser, an LSP feature, a docs refresh) is
  complete. Commit even if the milestone is "small"; never commit half-finished
  or failing work.
- **Update `PLAN.md` in the same wave as the milestone.** Completing a
  milestone means more than green tests: flip its row in the §1 table to
  `done` (with a date + one-line summary of what shipped), add a `> Status:`
  block under the milestone heading recording the landed shape and any
  deviations from the sketch, and refresh the now-stale cross-references —
  §2 module map / implemented-methods list, §4 gaps (delete resolved ones and
  renumber), and any "until M<n>" forward notes. Commit the plan update
  alongside the code when it is part of the same change, or as a follow-up
  `docs(plan): mark M<n> done…` commit. Do not leave a finished milestone
  marked `next`.
- Keep the working tree clean between milestones: stage only intended files,
  never build artifacts or secrets, and write a short conventional `scope:`-style
  message summarizing what the milestone delivers (see PLAN.md §5 forward plan, §7 acceptance).
- Before committing, quickly review `git status` / `git diff` so the commit
  contains exactly the milestone's changes, nothing stray.
- The repo lives on `main`; push only when asked.
- Don't add attribution trailers for incorrect models
