# Changelog

Every user-visible change to `freebasicd` is recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the versions follow
[semantic versioning](https://semver.org/), pre-1.0: MINOR is a feature wave,
PATCH is fixes and docs, and a change to the protocol surface or the config-file
shape takes the MINOR.

**Nothing has been released.** There is no tag and no packaged artifact, and the
`0.7.0` in `CMakeLists.txt` is a number the build stamps on itself. Everything
therefore lives under `## [Unreleased]`, newest first. When a release ships, that
heading becomes a version heading with its date and a new empty `## [Unreleased]`
opens above it.

Entries record *what shipped and what it cost* — the deviation from the plan, the
defect found on the way, the lesson worth keeping. That last part matters because
a changelog is the only place a reader learns that, say, a win32 code lens must
be a `Command` and not a `CodeAction`, because the protocol has no field for an
edit on a lens. The generalized form of those lessons lives where it is enforced,
not here: language facts in [`FreeBASIC.md`](FreeBASIC.md), working rules in
[`AGENTS.md`](AGENTS.md), clang-tidy suppressions in [`TIDY.md`](TIDY.md).
[`PLAN.md`](PLAN.md) holds only what is still to do.

Milestone numbers (`M1`…`M20`) are the identifiers the plan and the commit
history use; they are kept here so an entry can be traced back.

## [Unreleased]

### 2026-10-01 — A record body ends at a by-value self-reference

- `dim p as point` inside `type point` is fbc's `error 88: Recursive TYPE or
  UNION not allowed` and can never be a field, so `acceptsBodyMember` now takes
  the enclosing record's key and refuses it. That is the reported case from the
  bug report (`tests/corpus/blocks_type.bas` with `end type` deleted): the
  boundary lands on the `dim p as point` line, the quick fix inserts `END TYPE`
  directly above it, and the module-level `dim p as point` below is a
  module-level Dim again instead of colliding with the field the body also
  declares. Two forms are exempt because they carry no per-instance storage and
  fbc accepts both: `ptr` (the documented workaround) and `static`. An array
  dimension does not exempt it (`dim p as point(10)` is still `error 88`) and
  neither does a type suffix, which `fb` ignores (warning 44).
- No new diagnostic: this is a boundary rule, not an error of its own. The only
  visible change is where the existing `unterminated-block` squiggle's fix
  inserts its closer — the squiggle stays on the opener.
- Defect found on the way, and the lesson. The first cut judged a record body's
  boundary on the statement's **leading word** (`end`, `next`, `wend`, `loop`
  close a block), which made `Next As Node Ptr` — the canonical FreeBASIC
  linked list — end the record body and offer to insert `END TYPE` above it.
  fbc accepts a keyword field name in a plain UDT (`error 238`, and only when
  the type also holds member functions), so the predicate now reads the
  **whole** statement: `isCloserStatement` is exported from `language.h` and
  shared by the parser's dispatch and the body check, and a closer word followed
  by an `as` clause is a field. Everything else those words carry belongs to
  the closer — `NEXT i` names the loop variable, `LOOP UNTIL cond` the condition
  — which is why the rule is one `as` check rather than a token count. **A
  leading word is never evidence about a statement; read the statement.**
  The suites already held the other half of that lesson: gating on a closer
  *word* alone regressed `arrays`, `blocks_do` and `blocks_for` plus two
  resolve checks, because `loop until` and `next i` stopped closing
  anything.
- `FreeBASIC.md` §7 gained the `error 88` row and the keyword-field-name
  paragraph; §12.15 records the recursion rule and its two exemptions, and its
  remaining divergence is the member procedure spelled with its body.

### 2026-10-01 — The `unterminated-block` fix lands where the closer belongs

- A missing closer is now inserted where the parse says it belongs instead of at
  end-of-buffer. `acceptsBodyMember` (`language.cpp`) answers whether a
  statement may appear in a record/enum body; the parser closes a body at the
  first statement it refuses, re-parses that statement in the enclosing scope,
  and records the offset on the diagnostic's new `closerAt`, which
  `insertBlockCloser` uses as its insertion point. The squiggle stays on the
  opener — unchanged ranges, so every published diagnostic is the one it was.
- A closer that does not match the innermost block is the same evidence from the
  other side: `end sub` met by an open `if` ends the `if` there (with
  `closer-mismatch` as before), and the closer then closes the block underneath,
  so `end sub` still ends the sub. `end for` / `end while`, which can close
  nothing, are handled the same way.
- Two deviations, both in `FreeBASIC.md` §12.15 and both deliberate. A member
  procedure spelled *with its body* inside a record body (`sub go()` … `end sub`)
  is still read as a member, where fbc wants `Declare Sub go()` — hover, call
  hierarchy and code lens resolve inside such a member, so a boundary there
  would re-attribute its members. And a by-value self-reference
  (`dim p as point` inside `type point`) was left as a field, which put the
  closer one statement late on the reported case (`blocks_type.bas` with `end
  type` deleted); the next entry closes that half.
- `FreeBASIC.md` §7 also lost a wrong claim found while probing: `name(…) = expr`
  is **not** a legal enum member (`a(1) = 1` is `error 3`).

### 2026-10-01 — Server version over the protocol

- `initialize` now answers with `serverInfo.name` and `serverInfo.version`
  (`0243f1f`), so a client can report which build it is talking to without
  scraping the server's stderr startup line. This needed an eleventh commit on the
  vendored LspCpp fork (`e4b177e`, pinned by `4059b5b`): `InitializeResult`
  modeled `capabilities` alone, so the struct had to gain the field.
- Closes the last item of PLAN §4 that concerned the version's reach.

### 2026-09-30 — M15: type go-to and type hierarchy

- `textDocument/typeDefinition`, `textDocument/implementation`, and
  `textDocument/typeHierarchy` with its `supertypes` / `subtypes` follow-ups
  (`1d4c117`, `d63f54d`, `c937189`).
- Two parser edges were added first, because nothing else could work without
  them: `Symbol::extendsKey` and `Symbol::ownerKey`. Before them, `extends` was
  captured as a spurious `Variable` field named `extends` inside the type body,
  and a module-level `sub t.go()` was captured as a *root* named and keyed `t` —
  the same key as the type — so `documentSymbol` listed `t` twice, `codeLens`
  drew a "0 references" lens for the bogus root, and the implementation was
  unaddressable by name.
- The type graph (`typeOf`, `supertypes`, `subtypes`, `memberImplementation`,
  `memberDeclaration`, `findVisibleMember`) lives in `src/resolve.{h,cpp}` beside
  its siblings rather than in a module of its own. `subtypes` answers from a new
  `WorkspaceIndex::extendingTypes` projection, which keeps the index direct-only:
  the breadth-first walk into the whole subtree is the caller's, because only the
  caller knows that the protocol asks for direct *and* indirect subtypes.
- **The previous plan sketch was not FreeBASIC.** It named `Type ... : base` and
  `Interface`; `fbc 1.10.2` rejects `type b : a` on every dialect and the
  language has no `interface` keyword. `Extends` on the opener line is the only
  inheritance form, so the graph is single-parent with no interfaces.
- `declaredTypeName` had an unbounded loop that only the new `typeOf` reached: its
  word scan over a signature stopped at the first non-word character without
  stepping over it, so `dim arr(10) as integer` hung the server. Fixed by
  extracting `nextSigWord`, whose contract is that the cursor always moves. Worth
  remembering as a class — a word scan inside a `while` over the same buffer needs
  a stated progress invariant, because a parser that mostly sees identifiers never
  exercises the other branch until a new caller does.
- Two fixture shapes were wrong and only compiling them found it: a UDT holding
  nothing but `declare sub go()` is error 256, and the forward member-edge layout
  only compiles when the type is declared *before* the `#include` that brings in
  the implementing header.
- `typeHierarchy/resolve` is not implemented, and the capability says so: the
  vendored `typeHierarchyProvider` is the bare-bool arm, which carries no
  `resolveProvider`, so the field is *absent* — and `prepare` fills `parents` and
  `children` eagerly, so there is nothing to resolve lazily.

### 2026-09-29 — M14: pull diagnostics, negotiated and never assumed

- `textDocument/diagnostic`, `workspace/diagnostic`, and
  `workspace/diagnostic/refresh` (LSP 3.17) as the negotiated alternative to
  push (`79ec1fe`). A client advertising `capabilities.textDocument.diagnostic`
  gets `diagnosticProvider` and no pushes for the rest of the session; a client
  that does not is served by the push path exactly as it was. One build, both
  client generations, and a client that supports both is never sent the same
  diagnostic twice.
- `documentDiagnostics` is the single definition of a document's payload, shared
  by the push path and both pull handlers, so the two deliveries cannot disagree
  for the same bytes.
- `resultId` is `AnalysisCache::hashContent` folded with the include edges and
  the diagnostics gate, computed fresh per request. The gate has to be inside the
  id: a config flip empties the report, and a client caching by id would
  otherwise keep the old one.
- `relatedDocuments` carries the transitive include closure one level deep, gated
  on `relatedDocumentSupport`. `WorkspaceIndex::onRescanCompleted` is the seam
  that lets an external edit — which arrives outside every notification the
  session can see — hint a re-pull; the callback is copied out under `rescanMu_`
  and invoked with no lock held.
- Two more fork commits: `0865f2a` adds the report-union types and both request
  types (a response typed as a bare full report could never answer `unchanged`),
  and `9b7257f` fixes `WorkingFiles::OnOpen`, which recorded the document version
  only when the document was *already* open — so the first `didOpen` of every
  session left `version = 0`, indistinguishable from "unknown", and
  `WorkspaceDocumentDiagnosticReport.version` has no other source.
- Three test failures here were malformed frames, not server bugs, and each wore a
  wrong answer's clothes: a `previousResultId` nested *inside* `textDocument`
  instead of beside it, and three raw strings whose `)"` terminator silently
  truncated the literal so a stray `"` landed on the wire. The handler never ran
  and the poll timed out. See AGENTS.md — **a raw string that ends in a JSON
  quote is a silent truncation**, and a frame built by string surgery fails as
  malformed input, not as a wrong answer.

### 2026-09-29 — Integration driver split per feature

- `session_integration` is now one translation unit per LSP feature
  (`40c4592`), each exposing `Run<Feature>Tests()`, with `main()` alone in
  `session_integration.cpp`. A wire frame or helper only one feature's tests send
  stays in that feature's file; the shared harness is `tests/session_support`.
  One ctest suite on purpose: the `[ RUN ]` / `[ DONE ]` markers and the final
  "all tests ran" line are properties of the process, and `ctest --timeout`
  prints one captured stream per test.
- A new feature file is registered in two places and only one of them fails
  loudly — miss the `CMakeLists.txt` entry and the link names the missing runner;
  miss the call in `session_integration.cpp` and nothing says anything, the suite
  prints `0 failure(s)` having tested less than the day before. Grep the driver
  for the runner name and check the new tests appear in the `[ RUN ]` output; a
  green run is not evidence a new file is wired in.

### 2026-09-27 — M13: selection ranges, call hierarchy, code lens

- **selectionRange** (`42f3f88`): `src/selection.{h,cpp}` derives the
  expand-selection chain from `(tokens, blockRanges, content)` alone — token →
  `:`-separated statement → enclosing blocks → file. A level is kept only when
  it strictly contains the level below it *and* adds non-blank text, because a
  keystroke that selects the same text reads as a broken expansion.
  `src/selection_lsp.{h,cpp}` parks the chain in a `thread_local` arena: LspCpp
  links `SelectionRange::parent` as a non-owning pointer and the response vector
  owns only the innermost node. The integration test asserts the *nested* chain
  on the wire, which is what proves the lifetime rather than assuming it.
- **callHierarchy** (`6b57f3f`): `src/call_hierarchy.{h,cpp}` scans a body for
  the three call shapes `fbc 1.10.2` accepts (`name(`, after `.`/`->`, and a bare
  statement-head name — `Call s` is error 146, so there is no fourth), resolves
  each through one `CalleeResolver` seam, and matches sites by `DeclIdentity`
  (declaring file + name-token range) rather than by name, because `foo` and
  `foo$` are one symbol in `fb` mode. Nodes are procedures; a property read is
  not a call. Two filters are load-bearing rather than cosmetic: a declaration's
  own name token is call-shaped, so without skipping it a recursive Sub reports
  itself as calling its own signature line; and keyword tokens are scanned in the
  Member shape only, because a reserved word is a legal member name.
- **codeLens** (`a9fed00`): `src/code_lens.{h,cpp}` owns which declarations carry
  a lens (the procedure-like kinds plus Type/Union/Enum/Namespace, nesting
  flattened) and the localized title, and takes the count through one
  `ReferenceCounter` seam, so the module has no workspace knowledge and its suite
  needs no index. The protocol's `CodeLens` has **no edit field**, so the click
  is a `Command` (`freebasicd.showReferences`, advertised through
  `executeCommandProvider`) and the panel itself stays client-side. The count and
  the list come from one walk with one explicit scope: `Closure` is what
  `textDocument/references` answers, `Workspace` adds the includers through the
  reverse-reachability graph and is what a lens should count. That required
  `referenceSites` to stop comparing `Symbol` pointers — each candidate is
  parsed into its own analysis, so one declaration is a different object in every
  one of them.
- The lens title's plural goes through a `trn` ngettext wrapper, because only the
  catalog's plural rule can serve a language with three or four forms and two
  hand-written msgids cannot.

### 2026-09-27 — An open buffer outranks disk

- The workspace scan now skips any path that has an open-buffer entry
  (`52eb9a4`, with `aacc102` and `a9fed00`'s neighbor `TestScanKeepsOpenBuffer
  AheadOfDisk` as the pins). It was the Windows CI leg's last failure and the
  cause was not Windows-specific at all, only its trigger: the two-file fixtures
  write through a text-mode `ofstream`, so MSVC put CRLF on disk under the LF
  text the `didOpen` carried. The scan re-parsed the disk copy over the live
  buffer's entry while `contentForPath` kept serving the buffer for range
  conversion — byte offsets from one copy, line table from the other — so
  `dim localOnly` in a header came back at 1:5 instead of 1:4.
- The rule is enforced at both halves, and the write half is the one that matters:
  the scan reads the flag, reads and parses, and only then writes, so a
  `didOpen` landing in that window is a check-then-act race — and `upsert` itself
  now refuses to let a `fromDisk` entry replace a `fromDisk=false` one. A rule a
  reader can only satisfy at the check is not the rule; enforce it where the
  decision is committed. Reproduced locally on Linux by writing the fixture with
  explicit CRLF translation, and both checks are verified non-vacuous with
  `src/index.cpp` reverted.

### 2026-09-26 — The index's concurrency defects, fixed on their own merits

- A lost wakeup in rescan shutdown: `close()` cleared `running_` without holding
  `rescanMu_`, so a store-and-notify could land between `rescanLoop`'s predicate
  check and its block, and `close()`'s `join()` would never return
  (`6ce7ff4`).
- One `std::thread`, two owners: `scan(true)` is reachable from a handler thread
  as well as from the rescan loop, and both joined then reassigned `scanner_`
  (`6ce7ff4`). Every join and reassignment is now under `scannerMu_`.
- Neither was the cause of the apparent Windows hang — accumulated poll budgets
  were — but both are real, and `ctest` and ThreadSanitizer are both blind to
  them: a lost wakeup and a shared `std::thread` present identically, as silence.

### 2026-09-26 — macOS use-after-free in the workspace-symbol reply

- `ab3fba9`: `onWorkspaceSymbol`'s per-file hit list held a raw
  `IndexedFile const *` out of `idx->snapshot()`, while the background scan
  `upsert`s entries wholesale, so the reply build read a freed entry during
  per-file I/O. Linux reads stale-but-intact freed memory, so 12/12 local runs
  were green; macOS reuses the block and segfaults. Found by TSan in one run
  (9 reports, all one bug), 0 after the fix. The general rule now written into
  AGENTS.md: **a result type that hands out a pointer into a snapshot must carry
  the pin** — holding the `WorkspaceIndex` is not the pin, because a scan replaces
  the entries inside it.

### 2026-09-26 — The harness bugs that presented as a slow platform

Four defects, none of them in the server, each of which failed as a *timeout* or a
*hang* instead of as an assertion:

- Document URIs were built by gluing `"file://"` onto `path.string()` (`47b0687`).
  On Windows that yields backslashes, and a backslash inside a JSON string is an
  escape: `\t` and `\f` corrupt the path silently and `\w` is not legal at all,
  so the `initialize` frame never parsed and eleven tests burned their ~20 s poll
  budgets each. Eleven 20-second budgets *is* an apparent hang. Every URI now goes
  through LspCpp's `make_file_scheme_uri` over `fblang::normalizePath` output — the
  same call the server makes when it echoes a URI.
- The 68 hardcoded `file:///tmp/...` document URIs were not Windows URIs at all
  (they decode to a drive-less path). They now read `file://{{tmp}}/…`, expanded
  per platform by a `MakeLspFrame` that shadows `test::MakeLspFrame`, so no call
  site changed.
- The home-folder guard that ends root selection's two unbounded upward walks
  compared path objects, and Windows spells the same directory two ways
  (`%USERPROFILE%` long, `%TEMP%` 8.3-short) — so the walk climbed out of the temp
  tree and rooted every test's index at the user profile (`79516d6`). Fixed with
  `std::filesystem::equivalent`, and covered by
  `TestHomeFolderGuardAsksTheFilesystem`, which reproduces both spellings on any
  platform through `HOME` and a symlink. A platform-only trigger is not an excuse
  for a test that can only run there.
- `PollRequest` matched its needle against the cumulative output stream rather
  than the reply (`a70aa72`). Every `didOpen` publishes diagnostics, so a needle
  naming a document was already in the stream before the first request was
  answered: the poll returned whatever that first reply said and stopped waiting.
  A premature answer read as a wrong answer — provable on Linux with a probe whose
  reply can never name the header. Now scoped to the reply's tail, with a give-up
  line naming the prefix, the needle and the newest reply, because from the
  caller's side a give-up and a wrong answer are the same failed assertion.

### 2026-09-26 — A test suite that can name its own failure

- `922acef` and `13e6d1b`: `session_integration` brackets every test with a
  flushed `[ RUN ]` / `[ DONE ]`, catches an escaping exception to attribute and
  count it, and `main` prints a final "all tests ran" line. A missing final line
  puts the death *inside* a test; a present one puts it in teardown or static
  destruction. `std::set_terminate` reports what escaped and whether an exception
  was active at all — a bare terminate *is* the diagnosis. A Windows
  unhandled-exception filter was tried and removed: it printed a faulting address
  from a Release runner, where nothing can resolve one, and dragged
  `<Windows.h>` into a cross-platform test file.
- This converted an 11-minute silence with no output into one readable failure
  list, which is what found the URI defect. CI now passes `ctest --timeout` so
  the captured output of a timed-out test is printed (`d406d98`).

### 2026-09-26 — Windows checkout, LF, and CI plumbing

- `.gitattributes` pins `* text=auto eol=lf` (`b737d43`). The default
  `core.autocrlf=true` rewrote the three generated grammar files to CRLF, and
  `grammar_checks` byte-compares them against an emitter that writes `\n`.
  Reproduced locally by `sed`-ing CRLF into the three files — same three
  failures, same order.
- `paths-ignore` was `["**.md"]`, which skipped nothing; two docs-only pushes ran
  all five legs. Now `["**/*.md"]` (`17f15b0`).
- `ctest`'s own 1500 s default let one stalled leg hold the whole job; the
  workflow passes `--timeout 300`.

### 2026-09-25 — M12: quick fixes, and the `CodeAction` shape they need

- `textDocument/codeAction` with `codeActionKinds: ["quickfix"]` and two fixes:
  `unterminated-block` appends the closer the opener expects, one fix per block,
  and `include-not-found` retargets an existing directive at a workspace file the
  document's own `resolveInclude` seam accepts — never a guess (`66e81eb`).
- `src/code_actions.{h,cpp}` is the LSP-agnostic, byte-offset fix layer, keyed on
  diagnostic code in a `{diagnostic code, provider}` table. **Adding a fix is one
  function plus one table row** and touches neither `session.cpp` nor the
  capability. Providers are pure functions of (diagnostic, `QuickFixContext`); the
  context is the only way they reach workspace state, so a fix never guesses.
- Two deviations from the plan, both forced by how the diagnostics are produced:
  the include fix *retargets* rather than inserting a new `#include` line (the
  diagnostic is anchored on an existing directive whose literal resolved
  nowhere, so a new line leaves the edge unresolved and the diagnostic standing);
  and the closer fix appends the *innermost* still-open closer, because the parser
  closes every unterminated block at EOF and `blockRanges` cannot say which is
  inner.
- **The first cut shipped fixes that listed in the lightbulb and did nothing**
  (`9748541`). The protocol's result is `(Command | CodeAction)[]`, and only the
  second variant can carry a fix: a `Command` is an id the client *executes*, and
  there is no standard id meaning "apply this edit". The first cut answered with
  `lsCommandWithAny` — empty `command`, serialized `WorkspaceEdit` in
  `arguments[0]` — on the assumption that an empty command is the client's cue to
  apply it. It is not. LspCpp already had the reader but not the matching writer,
  so the fix was a fork commit (`50be209`), and the handler now emits a typed
  `CodeAction` with `edit.changes` keyed by the request's own URI. This is the
  rule the code lens hit again a month later, one layer up: **a thing the client
  has to execute is a `Command`; a thing the client has to apply is an edit, and
  an edit needs a protocol field to live in.**

### 2026-09-25 — `freebasicd`: rename, README, install target, version 0.7.0

- The project is renamed from `freebasiclsp` across the CMake project, the binary
  and library targets, the gettext domain and `po/freebasicd.pot`, the
  `freebasicd.toml` config file, the log/diagnostic `source` strings, and the docs.
  The fork's LspCpp branch is renamed `freebasic-lsp` →
  `lsp-3.17-completions`, to name what it carries.
- `project(freebasicd VERSION 0.7.0)`, under semantic versioning, bumped only at
  a release. `cmake --install` lays down the binary, the compiled catalogs, and
  `LICENSE.md` (GPL requires shipping the license with the binary).
- `README.md`: status (early, agentic-coding testbed), build/test, capability
  table, `freebasicd.toml`, architecture map, localization.

### 2026-09-25 — Public-repo hygiene and issue forms

- `CONTRIBUTING.md` (the three gates an outside contributor must pass, where
  things live, how to add a quick fix, how to patch LspCpp through the fork,
  translation rules, and the rule that agent authorship is disclosed),
  `CODE_OF_CONDUCT.md` (a short custom policy rather than Contributor Covenant),
  and `SECURITY.md` (private advisories, an in-scope list for an LSP server, an
  out-of-scope list, and the "no CLA, no DCO" position inherited from GPL terms).
- `.github/ISSUE_TEMPLATE/`: bug, fbc divergence, build/CI failure, and feature
  request forms, each asking for what that class of report actually needs, plus a
  `config.yml` routing security reports to the private advisory form. Nothing is
  a free-form "describe the problem".

### 2026-09-25 — CI that builds and tests on four legs

- `.github/workflows/ci.yml`: a `build-test` matrix (Linux gcc, Linux clang,
  macOS AppleClang, Windows MSVC) through configure, build, `ctest`, then
  `cmake --install` into a scratch prefix and `tools/check_install_tree.cmake`,
  which asserts the binary, `LICENSE.md`, and all 29 catalogs — a leg that
  silently skipped gettext therefore fails there instead of shipping an
  English-only tree. The `clang-format` job is Linux-only and pins the formatter
  to 22.1.8, gating the same command AGENTS.md documents. Linted with `actionlint`
  and `shellcheck`, and the pwsh blocks are parse-checked with the real parser
  (neither linter reads PowerShell).
- Each platform leg failed first for a reason only that platform could show, and
  every cause is fixed:
  - **macOS** died at `#include <libintl.h>` in `src/main.cpp`. The header was
    found and the gettext include dir reached `freebasicd_lang`, but never the
    executable: `freebasicd_lang` linked `freebasicd_lang`'s dependency PRIVATE,
    and a static library hands private dependencies to consumers without the
    usage requirements. Linux hid it — glibc's `libintl.h` is in `/usr/include`.
    The library is PUBLIC now.
  - **Windows** died in its gettext step (mlocati splits the release and the
    `-dev-msvc` bundle has the header and the import library but no tools), in
    configure three times over (a `$root:` in a `throw` string, which PowerShell
    reads as a drive-qualified variable; a missing zlib only ixwebsocket's unused
    websocket path wants; and LspCpp asking a Visual Studio generator for seven
    boost nuget packages the build does not have), and finally on
    `error C2589: '(': illegal token on right side of '::'` in vendored
    `utils.cpp:594` — the UTF-16 conversion's `std::min` expanding to a `(` token
    because `<Windows.h>` brings `min`/`max` in as function-like macros. That one
    needed fork commit `8a67671`, and no local platform can show it.
  - Two latent `cmake/FindIntl.cmake` defects surfaced with them: its not-found
    branch could never be fatal (CMake does not turn a module's `<Name>_FOUND
    FALSE` into a configure error, which is *why* the macOS leg died in the
    compiler), and the `-DGETTEXT_ROOT` its header documented was never read.
- All five legs green on run `36293790173` (2026-09-27) — the first run where
  nothing failed on any platform. Still open: per-editor wiring docs and the first
  tag (PLAN M18).

### 2026-09-24 — M20: gettext localization

- System GNU gettext only, never vendored: `cmake/FindIntl.cmake` defines
  `Intl::Intl`, `find_package(Gettext)` supplies msgfmt/msgmerge, and a local
  `find_program` adds xgettext (the bundled FindGettext locates only the first
  two). `src/i18n.{h,cpp}` wraps the runtime (`tr`, `trf`, `initI18n`,
  `setClientLocale` from `InitializeParams.locale`), domain `freebasicd`, UTF-8
  catalogs, and only the environment's *message* locale activated — so LSP output
  never depends on the UI locale (`bddbcbe`).
- Keyword text and the proper noun `FreeBASIC` only ever reach a user as dynamic
  `trf` arguments, so translators never see them. Two never-translate invariants
  are enforced as code by `tests/i18n_checks`, along with pot freshness.
- `po/freebasicd.pot` plus 29 msginit-generated catalogs (English is the msgid
  language, so there is no `en.po`), installed under `<prefix>/share/locale`.

### 2026-09-24 — GPL-3.0-or-later

- `LICENSE.md` and file headers, with the pot/po headers synced to match
  (`c6f66ed`, `08ed052`). GPL requires shipping the license with the binary,
  which is why `cmake --install` installs it.

### 2026-09-22 — M11: configuration and workspace folders

- `freebasicd.toml` (`src/settings.{h,cpp}`, parsed with the vendored
  tomlplusplus v3.4.0) with `includePaths`, `diagnosticsOn`, `semanticTokensOn`,
  `inlayHintsOn`, later `codeLensOn`. `workspace/didChangeConfiguration`
  re-reads every root's file — the notification payload is ignored, the file is
  the truth, and the re-read is idempotent. `includePaths` join include
  resolution as step ②, and applying them re-resolves include edges wholesale
  without a re-parse. Feature gates serve empty results when off, with
  diagnostics-off meaning one empty publish per open buffer and then silence.
- The single session index became one in-memory `WorkspaceIndex` per workspace
  root (`indexes_`, keyed by normalized path under one mutex), with the
  `workspaceFolders` capability, `workspace/didChangeWorkspaceFolders`, per-index
  watched-file routing, and `workspace/symbol` aggregation across live indexes.
- Root selection is a 0–5 priority (`chooseIndexRoot`), and both unbounded walks
  stop at the home folder — decided by `std::filesystem::equivalent` rather than a
  path compare, because one directory routinely has two spellings. That was
  itself a Windows-CI finding; see the 2026-09-26 entry.
- A file outside every index root is served resolution-only through the
  session-root index's on-demand closure: never its own index (it would leak into
  `workspace/symbol`) and never the single-file branch while a client root
  exists.

### 2026-09-21 — M19: context-aware member completion

- `p.` after a UDT variable completes only the owner type's accessible members —
  `Public` always, `Private:`/`Protected:` only inside the type's own member
  procedures, which is fbc's error-202 gate. Qualified `EnumName.` members are
  ungated. `.`/`->`/`with`-implicit/chained chains all share hover's member walk
  (`755078f`).
- The parser now recognizes TYPE-body access sections and stamps each member
  with an `Access`, and `protected` joined the keyword catalog (so the generated
  grammars were regenerated). Unclosed blocks are closed at EOF so completion
  keeps working while a procedure is half-typed.

### 2026-09-20 — Workspace root detection from the project layout

- `0ccd462`: when the client root has no VCS marker, the root is narrowed from
  the opened document by walking up for a parent holding a catalogued
  `source`/`include` directory, so `/tmp/test/inner/src/file.bas` roots at
  `/tmp/test/inner`. The detected root and the signal that found it are logged to
  stderr.
- Enum members resolve and hover (`e85cd06`): qualified `Name.member` for
  explicit and plain enums, bare `member` for plain ones only, reserved-word enum
  names working, all cross-file.

### 2026-09-18/17 — M3 follow-ups: hover that resolves instead of guessing

- Member access `.`/`->` resolves through the base variable's declared type —
  cross-file, `with`-implicit, indexed and chained (`7252cd4`, `15df79b`) — and a
  usage resolves to its declaration rather than surfacing the enclosing scope node
  (`6512c2e`).
- Documents opened from a sibling project *outside* the workspace root are served
  through an on-demand include closure (`e0b4f69`), and an open-buffer entry is
  kept indexed through a workspace scan (`a74021b`).
- When the declared type is unknown or the member missing, `.walls` inside
  `with map` still reads "Member of `map`." instead of a colliding identifier or
  a sub signature (`351251f`, `a6bf19b`).
- Parser fixes: commas inside initializer parens no longer split declarations
  (`90b314c`), block-local dims shadow instead of tripping duplicate-definition
  (`1f5f039`), `for` counters declared `as` are indexed as loop-local dims
  (`1aada0e`), and keyword matching became case-insensitive as fbc's is (`e90bd65`).

### 2026-09-16 — M9: semantic tokens, inlay hints, editor grammars

- `full`, `full/delta`, and opt-in `range` semantic tokens, inlay hints for block
  closers and inferred types, and TextMate + vim grammars generated from the
  `src/language.cpp` keyword catalog so highlighting cannot drift from what the
  parser sees. A freshness gate byte-diffs the generated files (`8405d83`).
- Two deviations from the plan: the vendored `SemanticTokensEdit` was reshaped to
  the wire `start`/`deleteCount`/`data` form (fork commit `45846f7`) instead of
  adding a translation layer, because generic reflection was only correct at the
  top level; and only `full` results enter the delta cache, since a `range`
  resultId is never a baseline.
- The enum conformance pass found that enum *members* are module-scope constants,
  so the `enumMember` semantic-token type was added and `explicit` entered the
  catalog.

### 2026-09-16 — The index is in memory only

- `e041d39`: the M4 on-disk cache is gone. Nothing is ever written to disk, the
  `persisted` flag narrows to `fromDisk`, and scan's mtime/size cache-hit never
  accepts an open-buffer entry. This is the change that made the 2026-09-27
  open-buffer bug visible rather than silent.

### 2026-09-16 — M8: rename

- `6fae157`: `prepareRename` plus a resolution-based workspace `rename`. Rename is
  never name-based: candidate sites come from the index and are re-resolved, so a
  same-named local that shadows is untouched. `occurrencesAcross` extends the site
  set with reverse reachability, so a rename issued at a header declaration covers
  all of its includers, and an invalid `newName` is rejected before any site is
  collected.

### 2026-09-15 — M5, M5.5, M6, M7: the cross-file spine

- **M5** (`81a582c`): one `analyze(source) -> AnalyzedDoc` shared by the scan, the
  open-buffer upsert, and request-side resolution, plus the inverted projections
  (`byKey_`, `outInc_`, `transitiveIncludes` with a cycle guard) that every
  cross-file feature derives from.
- **M5.5** (`435ea2a`): `initialized` handling and dynamic
  `client/registerCapability` for watched files, plus static watchers in the
  `initialize` reply for clients that do not do dynamic registration.
- **M6** (`27f8f7b`): include-not-found diagnostics over the literal, a debounced
  watched-files rescan so the notification thread never blocks on a scan, and
  `#pragma once` recorded as metadata. `#include once` guard *states* are still
  not evaluated, and `#inclib` is still not a source include.
- **M7** (`5be46af`): `resolveAcross` in three tiers (in-file scopes with
  shadowing, then the module scope of each closure file in textual pre-order, then
  a lenient workspace fallback for still-unincluded headers), behind a `Shared`
  storage-tagging step the parser needed anyway — plain module-level `Dim` is not
  visible inside a procedure, even in the same file. Four cross-file handlers and
  two-file tests.

### 2026-09-14 — M1–M4: the server exists

- **M1** (`4f22238`): LspCpp session bring-up, capabilities, pushed diagnostics.
- **M2** (`592631d`, `3ab51e8`): the FreeBASIC lexer and parser language layer,
  block-closer facts and dialect gating verified against `fbc 1.10.2`, and a
  51-file corpus whose expected diagnostics are checked against the parser.
- **M3** (`297c5a9`, `ed031ca`, `023c137`, `f2adfb5`, `9b96e13`): real diagnostics,
  hierarchical document symbols, hover with signatures and doc comments, folding
  ranges, completion (keywords, `END`-block snippets, in-scope symbols), signature
  help, definition, references, document highlight, and wiki links for keywords.
- **M4** (`5d5b21b`): the workspace symbol index and `workspace/symbol`, since
  revised to in-memory-only.

### 2026-09-14 — Foundations

- FreeBASIC.md added as the single source of truth for language facts, with known
  divergences from real `fbc` tracked in its §12; the CI matrix scaffolded; the
  `clang-tidy` baseline established with `src/` at zero diagnostics; LspCpp
  vendored as a submodule at upstream `19150d12`.