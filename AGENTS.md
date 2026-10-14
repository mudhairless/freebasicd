# FreeBASIC LSP Server

Implement an LSP server for the FreeBASIC Language in C++. The server must be
cross platform. Build targets: LSP 3.17 over stdio. The project root is a git
repository (default branch `main`).

## Dependencies

- **LspCpp** is the LSP/JSON-RPC library, vendored as a git submodule at
  `third_party/LspCpp` from our fork
  [mudhairless/LspCpp](https://github.com/mudhairless/LspCpp), pinned to
  `e4b177e` on the fork's `lsp-3.17-completions` branch. That branch is upstream
  `kuafuwang/LspCpp` `19150d12` (master, 2026-08-21; no release tag exists) plus
  our commits, which carry the LSP 3.17 types and serialization upstream lacks:
  the watched-files registration types, the `codeAction` result's `CodeAction`
  arm and its writer, the diagnostic-report types, `SemanticTokensEdit`'s wire
  shape, `InitializeResult::serverInfo`, the `WorkingFiles::OnOpen` version fix,
  and the CMake/MSVC build fixes. The commits and their rationale live on the
  branch and in `CHANGELOG.md`. Restore with `git submodule update --init`;
  consumed via `add_subdirectory(third_party/LspCpp)` and linked as the `lspcpp`
  target. No Boost is required (`LSPCPP_STANDALONE_ASIO` is the default); build
  with `LSPCPP_BUILD_WEBSOCKETS=OFF`, `LSPCPP_BUILD_EXAMPLES=OFF`,
  `LSPCPP_BUILD_TESTS=OFF`, and `USE_ZLIB=OFF` (ixwebsocket is added whatever
  `LSPCPP_BUILD_WEBSOCKETS` says, and defaults on a compression a stdio server
  cannot reach). Windows builds with the Visual Studio generator, so keep that
  branch of LspCpp working — and note that only that generator reaches the
  boost-nuget branch and no local platform can configure it, so a change to
  LspCpp's CMake has to be checked by reading it, not by building it.
- **Working on the fork**: inside `third_party/LspCpp`, `origin` is
  `mudhairless/LspCpp` (fetch HTTPS, push SSH) and `upstream` is
  `kuafuwang/LspCpp` with its push URL disabled; the checked-out branch is
  `lsp-3.17-completions`. To pick up upstream changes: `git fetch upstream &&
  git switch lsp-3.17-completions && git rebase upstream/master`, then
  `git push --force-with-lease origin lsp-3.17-completions` and bump the pinned
  commit here (`.gitmodules` URL is unchanged, so `git submodule update --init`
  keeps working for contributors).
- LspCpp handles all protocol JSON via its bundled RapidJSON.
- **tomlplusplus** (github.com/marzer/tomlplusplus) is a git submodule at
  `third_party/tomlplusplus`, pinned to `30172438` (v3.4.0). Header-only — an
  INTERFACE target (`tomlplusplus::tomlplusplus`). It parses the server's
  `freebasicd.toml` config file (`src/settings.{h,cpp}`, M11).
- Requires CMake 3.16+ and C++17.

## FreeBASIC facts (encode these in the lexer/parser)

**`FreeBASIC.md` is the single source of truth for the language.** Read it
before touching the lexer/parser. The keyword catalog, block closers, wiki URLs,
and scope rules already encoded in `src/language.cpp` are data, not to be
re-derived. Known divergences from real `fbc` live in FreeBASIC.md §12 and must
stay there. Only `fb` is implemented: `#LANG "<name>"` and the `$lang`
metacommand both set the dialect, recorded in `ParseResult.lang`, and a non-`fb`
file gets a best-effort `fb` parse plus one `lang-mode` Information diagnostic.

Two conventions are ours rather than the language's:

- Identifiers are case-insensitive; the canonical lookup key is the lowercase
  name, and in `fb` mode a trailing type-suffix char is dropped from it, because
  fbc ignores suffixes there (warning 44) and aliases `foo`/`foo$` to one symbol
  (qb/fblite/deprecated keep the suffix). Build the key with `canonicalKey(name,
  lang)` (`src/language.h`), never `toLowerChars` alone; the one deliberate
  exception is the preprocessor define table, which still keys a `#define` name
  with its suffix (FreeBASIC.md §12.1).
- Doc comments: `///` and `''` lines directly above a declaration become hover
  text (our convention, FreeBASIC.md §5).

## Architecture

- **Layering**: `lexer` + `parser` + `language` are LSP-agnostic and work in
  byte offsets. LSP interaction happens only in `session`; convert positions via
  LspCpp `WorkingFile` line offsets (`utf16.h`). All LSP positions are UTF-16
  code units.
- **Boundary discipline**: `session` is the only system boundary — parse and
  validate external input there (protocol params, `freebasicd.toml`, paths) and
  let the pure modules trust their inputs. A contradiction at the boundary is
  reported, not silently resolved, and a malformed value never degrades a
  session.
- **LspCpp API**: use `lsp::LanguageSession`; register handlers with
  `server.on(...)`. Request types are `td_<feature>::request/response`,
  notifications `Notify_<Name>::notify`. Buffers via
  `WorkingFiles`/`WorkingFile` (`OnOpen`/`OnChange`/`OnClose`,
  `GetOffsetForPosition`, `RebuildLineOffsets`). Diagnostics are pushed with
  `Notify_TextDocumentPublishDiagnostics::notify`.
- **Concurrency**: LspCpp runs a parse pool + handler pool + FIFO notification
  thread, so requests can run concurrently with each other. Keep
  `max_workers=2`; guard shared language state with a mutex; take the lock only
  to snapshot a parse, and build responses lock-free. New handlers follow the
  same snapshot discipline (see §ThreadSanitizer).
- **Index**: each `WorkspaceIndex` is **in-memory only** — nothing is ever
  written to disk. A background scan parses the workspace (plus a debounced
  rescan on watched-file events); open-buffer entries are marked
  `fromDisk=false` and the scan **skips them entirely**, because the index's
  byte offsets have to describe the bytes a reply is measured against. That rule
  is enforced at **both halves**, and the write half is the one that matters:
  `upsert` itself refuses to let a `fromDisk` entry replace a `fromDisk=false`
  one — a rule a reader can only satisfy at the check is not the rule; enforce it
  where the decision is committed. Closed files stay disk truth, which is what
  makes a watched-files event converge an external edit. Since M11 the server
  owns **one index per workspace root**: `indexes_` (a map keyed by normalized
  root) under a single `indexesMutex_` holds the registered client folders that
  are themselves workspace roots, detected roots, and single-file roots.
  Session handlers snapshot a `shared_ptr<WorkspaceIndex>` (`indexFor` for a
  path, `allIndexes()` for workspace/symbol) and hold it while raw result
  pointers are in use. Watched-file events route per path to the owning root's
  index and are deduped by owner root; folder add/remove re-key the map.
- **Workspace roots (M11)**: `chooseIndexRoot`'s priority 0–5 — the deepest
  registered marker-root containing the file; a client root that is itself a
  workspace root as-is; else the nearest VCS marker, then the nearest
  `freebasicd.toml`, then the source/include layout walk
  (`findSourceLayoutRoot`, up to the drive root / `$HOME`); and single-file mode
  when no client root exists. A file outside every index root is served
  **resolution-only** through the session-root index's on-demand closure — never
  its own index (it would leak into workspace/symbol) and never the single-file
  branch while a client root exists. A detected root that replaces the client's
  is logged to stderr with the signal. The two unbounded walks end at the home
  folder, identified by `isHomeFolder` with `std::filesystem::equivalent`, not a
  path compare: Windows spells one path long and another 8.3-short and a
  symlinked home keeps the link, so a `==` misses the guard and the index roots
  at the profile. `src/settings.{h,cpp}` parses `freebasicd.toml`
  (`hasConfigFile` is the marker; `Settings` keys have fixed defaults). Each
  index adopts a `Settings` snapshot at construction (`ensureWorkspaceIndex` →
  `index->applySettings(settingsForDirLogged(root))`);
  `workspace/didChangeConfiguration` re-reads every root's file (the payload is
  ignored — the file is the truth; idempotent), applies per-root changes
  (`reindexIncludeEdges`, no re-parse), reconciles the root's open buffers
  (diagnostics off ⇒ one empty publish then silence, on ⇒ re-publish; an
  `includePaths` change re-resolves the buffers' include edges), and the
  diagnostics / semantic-tokens / inlay-hints / code-lens / code-action handlers
  gate on `settingsForDocument` with empty-result semantics when a flag is off.
- **Quick fixes (M12)**: `src/code_actions.{h,cpp}` is the LSP-agnostic,
  byte-offset fix layer, registered as a `{diagnostic code, provider}` table
  (`quickFixProviders()`, looked up with `quickFixProviderFor`). A provider is a
  pure function of (diagnostic, `QuickFixContext`); the context is the *only*
  way it reaches workspace state (an index snapshot, the document path, and a
  `resolveInclude` callback wired to `resolveIncludeTarget`), so a fix never
  guesses: offer a candidate only when that seam accepts it, or nothing.
  **Adding a fix = one function + one table row**; do not touch `session.cpp` or
  the capability. A fix ships as an LSP `CodeAction` (`title`, `kind:
  "quickfix"`, the diagnostic it answers, and `edit.changes` keyed by the
  request's own URI) — never as a `Command`, which the client *executes*, so an
  empty-id Command with the edit in `arguments` lists in the lightbulb and does
  nothing when picked. `context.only` is filtered server-side. Diagnostics a fix
  keys on must be built by the same function the publish path uses
  (`unresolvedIncludeDiagnostics`), or the published range and the fix's range
  drift apart.
- **Code lens (M13)**: `src/code_lens.{h,cpp}` is LSP-agnostic and byte-offset.
  It owns *which* declarations get a lens (`carriesLens`: the procedure-like
  kinds plus Type/Union/Enum/Namespace, nesting flattened) and the title, and
  takes the count through one `ReferenceCounter` seam, so adding a lens kind
  touches this module and nothing else. `LensAnchor` is pointer-free, so a lens
  pins no snapshot. `resolveProvider` is false and there is no `data` blob: the
  lens's `range` is the declaration's name token and the click sends that same
  position back, as a `Command` (`freebasicd.showReferences`, advertised through
  `executeCommandProvider`) — the same honesty rule as M12, one layer up. Count
  and list come from one walk, `referenceSites(..., scope)`, with `Closure` (the
  request file + its transitive includes — what `textDocument/references`
  answers) or `Workspace` (that plus the includers, through the M6
  reverse-reachability graph — what a lens counts). Sites are matched by
  `DeclIdentity`, **never** by `Symbol` pointer, because each candidate is parsed
  into its own analysis, so one declaration is a different object in every one.
  A lens cannot ask for `includeDeclaration`, so the count excludes the
  declaration's own name token — the convention `references` reports with it
  false.
- **Pull diagnostics (M14)**: the LSP 3.17 alternative to push, **negotiated and
  never assumed**. `capabilities.textDocument.diagnostic` present ⇒ advertise
  `diagnosticProvider` and disable `publishDiagnostics` for the whole session;
  absent ⇒ the push path is exactly what it was. The rule is not politeness: a
  client supporting both must never see the same diagnostic twice, and the spec
  has the server stop publishing once pull is negotiated.
  - **One payload, two deliveries.** `documentDiagnostics(content, parse, entry)`
    is the single definition of a document's diagnostics (parse diagnostics plus
    its own unresolved include edges) that the push path and both pull handlers
    call, so a client cannot be shown two answers for the same bytes.
  - **resultId** = `AnalysisCache::hashContent(content)` folded with the include
    edges and the diagnostics gate, computed fresh per request. Equal ids promise
    equal payloads, and nothing else is folded in, so a request naming a matching
    `previousResultId` can answer `unchanged` without recomputing. The gate has
    to be in the id: a config flip empties the report, and a client caching by id
    would otherwise keep the old one.
  - **relatedDocuments** is the M6 transitive include closure, one level deep (a
    related report is never itself related), gated on `relatedDocumentSupport`.
  - **`workspace/diagnostic/refresh`** is the server hint that stands where a
    push would have been: an edit, a close, a config change, and a watched-files
    rescan all fire it. The rescan needs the `WorkspaceIndex::onRescanCompleted`
    seam; the callback is copied out under `rescanMu_` and invoked with no lock
    held.
- **Message catalogs**: `fblang::trf` for a message, `fblang::trn(msgid, plural,
  n)` when the count changes the form — `ngettext`, because only the catalog's
  plural rule can serve a language with three or four forms, which two
  hand-written msgids cannot. Both share `detail::substitutePercentS` (`%s`
  only, verbatim, no printf — every argument is already a `std::string`). Editor
  UI strings (a lens title) are messages too. After a new or changed message,
  regenerate `po/freebasicd.pot` (`po-template` target) — `i18n_checks` fails on
  a literal missing from it. `po/*.po` are deliberately not regenerated in the
  same wave; they lag the pot.
- Capabilities advertise only implemented features; `positionEncoding: "utf-16"`.

## Verification

- Unit drivers in `tests/` via ctest (19 suites, one per subsystem) plus
  `session_integration`, which drives `LanguageSession` with in-memory streams
  (LspCpp `tests/test_helpers.h`). The system `fbc` compiler (1.10.2) is
  available for ground-truthing ambiguous FreeBASIC constructs.
- **`session_integration` is one executable, one file per feature.** The shared
  harness is `tests/session_support.{h,cpp}` (`namespace fbtest`); each feature
  is one translation unit that `using namespace fbtest;` and exposes
  `void Run<Feature>Tests()`; `tests/session_integration.cpp` holds `main()`
  alone and calls each runner in a fixed order. Two rules keep the split from
  eroding, both enforced by reading the tree rather than by a script: a **wire
  frame or helper only one feature file's tests send stays in that file**,
  declared just above them, and **adding a test touches exactly one file** — the
  function plus one `RUN_TEST` line in that file's runner.
- **A new feature file is registered in two places, and only one fails loudly.**
  Add the source to the `session_integration` target in `CMakeLists.txt`, and
  declare + call its `Run<Feature>Tests()` in `tests/session_integration.cpp`.
  Miss the first and the link fails with `undefined reference to
  fbtest::Run<Feature>Tests()` — the error names the step. **Miss the second and
  nothing says anything**: the file compiles, links, and its tests never run, so
  the suite prints `0 failure(s)` having tested less than the day before. When
  adding a feature file, grep the driver for the runner name and check the new
  tests appear in the `[ RUN ]` output; a green run is not evidence that a new
  file is wired in.
- **Keep it one ctest suite.** The `[ RUN ]` / `[ DONE ]` markers and the final
  "all tests ran" line are properties of the *process*, and `ctest --timeout`
  prints one captured stream per test, so splitting the suite into many
  executables would trade a readable crash log for a per-file timeout budget
  nobody can act on. One file per feature is a source-layout choice; one
  executable is a diagnostics choice, and the second is deliberate.
- Hosted CI (`.github/workflows/ci.yml`): the `build-test` job configures,
  builds, and `ctest`s on Linux (gcc and clang), macOS (AppleClang), and Windows
  (MSVC), then installs into a scratch prefix and checks the tree with
  `tools/check_install_tree.cmake`; the `clang-format` job enforces the
  formatting gate below on Linux. A green local run is not a substitute for the
  macOS and Windows legs — those only exist on CI.

### ThreadSanitizer

**On demand, like clang-tidy — not part of the gate.** `ctest` cannot see a data
race, and the workspace index is the one place where a bug of this class is
invisible locally: the handler pool reads a `WorkspaceIndex` snapshot while the
background scan thread `upsert`s the same paths. Run the integration driver
under TSan when touching `src/index.{h,cpp}`, `src/session.cpp`, or anything that
hands a raw pointer into a snapshot:

```
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan --target session_integration --parallel
./build-tsan/session_integration 2>&1 | grep -c 'WARNING: ThreadSanitizer'
```

Zero is the bar. The rules it enforces:

- **A result type that hands out a pointer into a snapshot must carry the pin.**
  `CrossDecl::file`, `MemberAccess::file`, `MemberCompletion::owner.file`, and
  `fblang::IncomingCall::caller` are that pin (the last a bare `Symbol const*`
  into a scanned `AnalyzedDoc`, so the caller's `shared_ptr<DocumentContent>` is
  held across the whole conversion), and `onWorkspaceSymbol`'s per-file hit list
  holds `shared_ptr<IndexedFile const>` for the same reason. Holding the
  `WorkspaceIndex` is *not* the pin: a scan replaces the entries inside it.
- **A pointer that must outlive the handler** is the same rule. LspCpp's
  `SelectionRange::parent` is a non-owning `optional<SelectionRange*>`, so
  `src/selection_lsp.{h,cpp}` parks every ancestor in a `thread_local
  std::deque` cleared once per request. It is sound *because*
  `sendSessionMessage` serializes the reply inline on the handler thread under
  the send mutex and `ResponseOrError(T&&)` moves the response — one thread, one
  arena, no lock. The lifetime is asserted rather than assumed: the
  selection-range integration test matches the **nested** `"parent"` chain on the
  wire. The same reasoning puts a whole batch of chains in one `selectionRanges`
  call; per-position calls would clear the arena and dangle the ones already
  built.
- **An offset is meaningless without the content it is relative to.** Whatever
  hands out an offset has to be the same thing that supplies the line table, so
  the scan must not re-parse an open buffer (§Index). The tell is a fixture that
  writes bytes the client never sent: the two-file fixtures write through a
  text-mode `ofstream`, so on MSVC the disk copy is CRLF under an LF `didOpen`.
- **A condition variable's predicate state changes under the mutex the waiter
  blocks on.** An atomic flag is not a substitute — the waiter holds nothing the
  notifier is obliged to take, so the notification can be lost. Every wait that
  shutdown depends on needs a timeout *or* the lock discipline.
- **One `std::thread`, one thread touching it.** Every `join()` and every
  reassignment of `scanner_` is under `scannerMu_`.

What TSan and `ctest` cannot see, and how to diagnose it:

- **Make a hanging suite name itself.** `session_integration` prints
  `[ RUN ] <test>` per test (flushed, so it survives a process that never exits)
  and CI passes `ctest --timeout`, which prints a timed-out test's captured
  output — that is what localizes a platform-only hang.
- **Silence is not a pass.** On MSVC every way out of `main` is quiet, so a bare
  `***Failed` with no reason line means the process never returned. `RUN_TEST`
  brackets every test with `[ DONE ]`, catches an escaping exception to name and
  count the test (one bad test no longer takes the suite with it), and `main`
  prints a final "all tests ran" line: a missing final line puts the death inside
  a test, a present one puts it in teardown or static destruction.
  `std::set_terminate` reports what escaped the tests and whether an exception
  was active at all.
- **Keep this list to the portable markers.** A Windows unhandled-exception
  filter was removed: it printed an unresolvable faulting address in Release and
  dragged `<Windows.h>` into a cross-platform test file. Add a platform reporter
  back only if a crash localizes to a test and `[ DONE ]` is demonstrably not
  enough.
- **Then reproduce the platform's difference locally.** A platform-only trigger
  is not an excuse for a test you can only run there:
  `TestHomeFolderGuardAsksTheFilesystem` sets `HOME` to a symlink and to a `..`
  spelling in turn and asserts the home folder's own project stays out of the
  index, so the defect fails a Linux run before it ever reaches a Windows runner.

Test fixtures and wire frames:

- **`file://` + `path.string()` is not a URI.** Build URIs through LspCpp's
  `make_file_scheme_uri` over `fblang::normalizePath` output — the same call the
  server makes when it echoes a URI, so a test and the server agree byte for byte
  on every platform. A backslash inside a JSON frame is an *escape*, so
  `"d:\tmp\ws"` silently corrupts the path and a test fails on a poll budget
  rather than on an assertion. A hardcoded `file:///tmp/x.bas` is not a Windows
  URI at all, so write documents against the `{{tmp}}` placeholder that
  `MakeLspFrame` expands per platform. A fixture that builds a wire-format string
  by string surgery fails as *malformed input*, not as a wrong answer.
- **A raw string that ends in a JSON quote is a silent truncation.**
  `R"(","value":"")"` *looks* like a fragment ending in a closing quote, but the
  `)"` is the raw string's own terminator, so the literal ends at `:"` and the
  stray `"` starts an empty C string. The frame never parses, the handler never
  runs, and the poll reports a timeout — a malformed input wearing a wrong
  answer's clothes. When a poll times out with an empty newest reply, print the
  frame and read it before reading the server.
- **A poll's needle must be scoped to the reply it returns.** `PollRequest`
  matches its needle against the cumulative output stream, but every `didOpen`
  publishes diagnostics, so a needle naming a document is already in the stream
  before the first request is answered. Match in the region the caller will
  inspect (the reply's tail), and let a poll that spends its budget say so — from
  the caller's side a give-up and a wrong answer are the same failed assertion.

### clang-format

Formatting is **part of the milestone gate** (unlike tidy): the repo's
`.clang-format` pins the LLVM standard (2-space indent, attached braces,
80-column). CI runs the command below on every push and pull request
(`clang-format` job, Linux only); run it locally before acceptance / commit:

```
git ls-files '*.cpp' '*.h' | grep -v '^third_party/' \
  | xargs clang-format --dry-run --Werror
```

`git ls-files` does not descend into the submodules, and the filter says so out
loud, so a new file in a new directory cannot escape the gate. **Run the
system-installed `clang-format` — the `/usr/bin` copy.** This machine tracks the
latest LLVM, so the local tool drifts ahead of the workflow's pinned
`CLANG_FORMAT_VERSION`; that is expected while both versions agree on the tree.
**When a discrepancy arises — the system version formats a file differently than
the pin — tell the user and bump `CLANG_FORMAT_VERSION` to the system version in
its own standalone commit**: no `CHANGELOG.md` entry, it is a tooling sync, not a
milestone. Apply with `clang-format -i <file>` (or `--lines=start:end` for a
region, e.g. right after `clang-tidy --fix`). Corpus `.bas`/`.diag` files and
`third_party/` are out of scope.

### clang-tidy

On-demand only — not part of the milestone gate. Baseline config lives at repo
root `.clang-tidy`; keep `src/` **zero-diagnostic** under it whenever you do run
it, but don't block a milestone on tidying. Run it with the system-installed
`clang-tidy` (the same "run what the system installed" policy as clang-format)
when asked or when a change looks tricky (copy-paste code, heavy templates, new
headers):

```
cmake -S . -B build-tidy -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CXX_COMPILER=clang++
clang-tidy -p build-tidy --quiet <changed src/*.cpp>
```

Re-create the throwaway `build-tidy/` DB above (gitignored via `build*/`) rather
than trusting the checked-in `build/compile_commands.json`, which is stale.
`WarningsAsErrors` gates bug-class checks (`bugprone-*`, `cert-*`,
`clang-analyzer-*`, `performance-*`), so a real finding exits non-zero; do not
widen the suppression list to silence a new bug-class hit — those entries exist
for deliberate conventions only, each with its reason in `.clang-tidy` and
`TIDY.md`. The raw `--fix` output is not formatter-clean, so run `clang-format
-i` over the touched lines afterward and review the diff.

## Versioning

The project follows **semantic versioning**. The single source of the number is
`project(freebasicd LANGUAGES CXX VERSION <x.y.z> ...)` in `CMakeLists.txt`;
CMake passes it to `main.cpp` and `session.cpp` as `FBLANG_VERSION`, and it
surfaces in the LSP `initialize` reply's `serverInfo` and on the startup stderr
line.

- **Bump the version only when a release ships.** Feature commits, fix commits,
  refactors, and docs commits never touch it. A release is a version bump plus a
  tag, and the tag is what the number is for. A release also retitles
  `CHANGELOG.md`'s `## [Unreleased]` to `## [<x.y.z>] — <date>` and opens a fresh
  empty `## [Unreleased]` above it, so the two numbers cannot drift apart.
- Pre-1.0 (anything `0.y.z`): MINOR is the feature wave, PATCH is fixes and
  docs. BREAKING in the 0.x line means "the protocol surface or the config file
  changed shape", and it takes the MINOR.
- Keep it below 1.0 until the LSP surface, the `freebasicd.toml` keys, and the
  install layout have held still long enough to call stable.

## Git workflow

- **Commit automatically when a milestone is achieved.** Do not wait for an
  explicit commit request. A milestone is achieved when its acceptance criteria
  pass: the milestone's tests are green (`ctest` / `cmake --build`), changed C++
  files are `clang-format` clean (see §clang-format), and the milestone
  deliverable is complete. Commit even if the milestone is "small"; never commit
  half-finished or failing work.
- **A finished milestone goes in `CHANGELOG.md`, not `PLAN.md`.** The plan is for
  work still to do; the changelog is the record of what landed, when, and what it
  cost. A milestone's closing wave:
  1. **Add a dated `CHANGELOG.md` entry** at the top of `## [Unreleased]`,
     newest first, headed `### <date> — <what shipped>`. Keep a Changelog shape
     (`### Added` / `### Changed` / `### Fixed` only when the entry genuinely has
     those three kinds). Record the deviations from the sketch and the defects
     found on the way *with their lesson* — that is the part a future reader
     cannot reconstruct from the diff, and it is why the file exists. Point at
     `FreeBASIC.md` for language facts and this file for the generalized
     engineering rule; do not restate either.
  2. **Delete the milestone's section from `PLAN.md` §5** and drop its §1 table
     row. A completed milestone keeps at most a one-line pointer in the table if
     that is genuinely useful context; its sketch, acceptance list, and status
     block do not survive the wave. The plan must not accumulate history.
  3. **Refresh the now-stale cross-references in `PLAN.md`**: §2's module map and
     implemented-methods list, §4 gaps (delete resolved ones and renumber), and
     any "until M<n>" forward note. Also update `README.md`'s capability table
     when the milestone changed what the server advertises.
- Do not leave a finished milestone marked `next`, and do not commit a milestone
  whose changelog entry is missing — an unrecorded milestone is the failure mode
  this rule exists to prevent.
- Keep the working tree clean between milestones: stage only intended files,
  never build artifacts or secrets, and write a short conventional `scope:`-style
  message summarizing what the milestone delivers (see PLAN.md §5 forward plan,
  §7 acceptance).
- Before committing, quickly review `git status` / `git diff` so the commit
  contains exactly the milestone's changes, nothing stray.
- The repo lives on `main`; push only when asked.
- Don't add attribution trailers for incorrect models
