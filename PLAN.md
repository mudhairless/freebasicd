# FreeBASIC LSP Server — Implementation Plan

## 1. State summary

Repository `main`, clean working tree, `ctest` 15/15 green. The project is
`freebasicd` (renamed from `freebasiclsp` 2026-09-25), version 0.7.0 under
semantic versioning: the number is bumped only when a release ships, never in
an ordinary feature or fix commit. LspCpp (vendored from our fork
`mudhairless/LspCpp` at `8a67671`, i.e. upstream `19150d12` plus
eight local commits) supplies
framing/JSON-RPC/typed 3.17 messages,
tomlplusplus (vendored, pinned `30172438` v3.4.0) parses the server's config
file, and GNU gettext (system libintl, never vendored; `cmake/FindIntl.cmake`
+ `FindGettext`) localizes log and diagnostic messages from committed
`po/*.po` catalogs; the language layer is LSP-agnostic and byte-offset based.
Full language
reference (keyword catalog, block closers verified against fbc 1.10.2, dialect
and scope rules) lives in `FreeBASIC.md`; this plan covers roadmap,
architecture, and the remaining work.

| Milestone | Status |
|-----------|--------|
| M1 — LspCpp bring-up (sync, capabilities, diagnostics push) | done |
| M2 — Lexer + parser language layer, dialects, fbc corpus | done |
| M3 — documentSymbol, hover, folding, definition, references, highlight, completion, signatureHelp | done (2026-09: hover resolves member access `.`/`->` through the base variable's declared type — cross-file, `with`-implicit, and indexed/chained — instead of falling back to the enclosing routine; a follow-up bugfix serves documents opened from a sibling project *outside* the workspace root via an on-demand include closure; a second bugfix adds a soft fallback: when the declared type is unknown or the member missing, `.walls` inside `with map` still reads "Member of `map`." instead of a colliding identifier or the sub signature; a final conformance pass makes enum members resolve and hover — qualified `Name.member` for explicit and plain enums, bare `member` for plain ones only, reserved-word enum names like `enum color` working, all cross-file) |
| M4 — persistent workspace symbol index + `workspace/symbol` | done (2026-09: rev'd to an **in-memory-only** index — no on-disk cache; workspace-root fallback detection: when the client root (or single-file mode) has no version-control marker, the root is narrowed from the opened document by walking up to the drive root / `$HOME` for a parent holding a catalogued `source`/`include` directory — e.g. `/tmp/test/inner/src/file.bas` roots at `/tmp/test/inner`; the detected root (and its signal: VCS marker vs source/include directory) is logged to stderr) |
| M5 — workspace spine: occurrence projection + include graph | done |
| M5.5 — lifecycle: `initialized` + dynamic capability registration | done (2026-09: static/dynamic negotiated, registerCapability frame verified) |
| M6 — include resolution + watched files + missing-include diagnostics | done (2026-09: missing-include diagnostics, debounced watched-files rescan, `#pragma once` metadata; the include search gained the project-dir (`-i inc`) step and the index an on-demand, resolution-only closure for out-of-root documents) |
| M7 — cross-file definition / references / highlight / completion | done (2026-09: `resolveAcross` tiers, `Shared` storage gate, four cross-file handlers, two-file tests) |
| M8 — `prepareRename` + `rename` (workspace) | done |
| M9 — semantic tokens + inlay hints + highlight grammar | done (2026-09: full/delta + opt-in range tokens, block-closer/inferred-type hints, catalog-derived TextMate + vim grammars with a freshness gate) |
| M10 — intrinsic catalog + request-side parse cache | done (2026-09: content-addressed `AnalysisCache` behind a `ContentProvider` seam, plus a 247-row intrinsic catalog feeding completion/hover/signatureHelp) |
| M11 — configuration + workspace folders | done (2026-09: `freebasicd.toml` settings (`src/settings.{h,cpp}`) + config-file root detection; the single session index became one in-memory `WorkspaceIndex` per workspace root — `chooseIndexRoot` priority 0–5 (registered marker root → client root as-is → VCS marker / config file / source-layout walk → single-file), where both unbounded walks stop at the home folder, decided by `std::filesystem::equivalent` rather than a path compare because one directory routinely has two spellings (Windows' 8.3 `%TEMP%` against a long `%USERPROFILE%` is how the Windows CI leg found it), `workspaceFolders` capability, `workspace/didChangeWorkspaceFolders` handler, per-index watched-file routing, workspace/symbol aggregation; `workspace/didChangeConfiguration` re-reads each root's toml on the notification (payload ignored, idempotent), applies `Settings` per root — `includePaths` joins include resolution as step ② (`reindexIncludeEdges`, no re-parse) and the diagnostics / semantic-tokens / inlay-hints gates serve empty-result + clear semantics with per-root isolation tests) |
| M12 — code actions: quick fixes for missing includes + block closers | done (2026-09: `textDocument/codeAction` with `codeActionKinds: ["quickfix"]`; a registry keyed on diagnostic code (`src/code_actions.{h,cpp}`) so a new fix is one row plus one function; two fixes shipped — `unterminated-block` appends the closer the opener expects (one fix per block, re-parse nests them) and `include-not-found` retargets the existing directive at a workspace file the document's own include-resolution seam accepts, never a guess; the publish path and the fix key now build the include diagnostic from one shared function, so they cannot disagree; fixes answer as LSP `CodeAction`s carrying `kind` + the diagnostic + an `edit` keyed by the request's URI, not as empty-id `Command`s — the first cut shipped the `Command` shape and the actions listed but did nothing) |
| M13 — editor extras: selectionRange, callHierarchy, codeLens | done (2026-09-27: aspect 1 `selectionRange` shipped — `src/selection.{h,cpp}` derives the expand-selection chain from `(tokens, blockRanges, content)` alone (token → `:`-separated statement → enclosing blocks → file), keeping a level only when it strictly contains the one below *and* adds non-blank text; `src/selection_lsp.{h,cpp}` parks the chain in a `thread_local` arena because LspCpp's `SelectionRange::parent` is a non-owning pointer, and the integration test asserts the nested chain on the wire. 2026-09-27: aspect 2 `callHierarchy` shipped — `src/call_hierarchy.{h,cpp}` scans a body for the three call shapes fbc 1.10.2 accepts (`name(`, after `.`/`->`, and a bare statement-head name) and resolves each through one `CalleeResolver` seam, matching by `DeclIdentity` (file + name-token range) rather than by name, so a shadowing local is excluded by resolving the site and `foo`/`foo$` stay one symbol; nodes are procedures, properties are never callees, module-level call sites are dropped; `src/call_hierarchy_lsp.h` defines the outgoing-call request type locally because LspCpp registers it under the wrong wire name; the 17th suite `call_hierarchy_checks` plus a two-file integration test. 2026-09-27: aspect 3 `codeLens` shipped — `src/code_lens.{h,cpp}` owns which declarations carry a lens (the procedure-like kinds and the type-ish roots, nesting flattened) and the localized title, and takes the count through one `ReferenceCounter` seam so the module itself has no workspace knowledge; the session answers the count and the click from one `referenceSites` walk, so the number a lens shows and the list `freebasicd.showReferences` returns cannot drift; the lens is a `Command` (the protocol's CodeLens has no edit field) with `codeLensProvider = { resolveProvider: false }` + `executeCommandProvider`; the count crosses the include boundary in both directions, which required making the walk match by `DeclIdentity` instead of by `Symbol` pointer; a `trn` ngettext wrapper for the plural (`trf` cannot express one); a `codeLensOn` settings key; the 18th suite `code_lens_checks` plus a two-file integration test that also drives the command) |
| M14 — pull diagnostics (backlog) | next |
| M15 — type/go-to + type hierarchy (backlog) | next |
| M16 — document links + completion resolve + polish (backlog) | next |
| M17 — FreeBASIC formatter (backlog, scope TBD) | next |
| M18 — public release: install + version, editor setup docs, CI (moved from M11; README landed 2026-09-25 with the rename) | in progress (2026-09-25: `README.md` shipped, the project was renamed to `freebasicd`, `cmake --install` now installs the binary + catalogs + `LICENSE.md`, the version is pinned at 0.7.0, the hygiene files (`CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`) and the issue forms are in, and `.github/workflows/ci.yml` builds and tests on Linux (gcc, clang), macOS, and Windows while a Linux job enforces the `clang-format` gate. 2026-09-26: the matrix has run — Linux (both compilers), `clang-format`, and macOS are green and holding; each platform leg failed first for a reason only that platform could show, and every cause found so far is fixed (Windows: an MSVC `min`/`max` macro collision in vendored LspCpp, a CRLF checkout breaking the byte-exact `grammar_checks`, a *test-harness* defect — the fixtures built `file://` URIs by string concatenation, which is malformed JSON on Windows, so eleven tests never got an answer and burned their poll budgets, which is what looked like a hang — and, once that was gone, a *server* defect the harness had been hiding: the home-folder guard that ends root selection's two unbounded walks compared path objects, and Windows spells the profile two ways (`%USERPROFILE%` long, `%TEMP%` 8.3-short), so the walk left the temp tree and every test's index rooted at the profile — 5 assertions across 3 tests, fixed with `std::filesystem::equivalent` and covered by a test that reproduces the spelling mismatch on any platform, which left exactly one failure: the suite's `PollRequest` helper matched its needle against the cumulative output stream rather than the reply, and every `didOpen` publishes diagnostics, so a needle naming a document was already there and the poll returned the first reply whatever it said — a premature answer read as a wrong one (proved on Linux with a probe whose reply can never name the header, and fixed by scoping the match to the reply, which also removes an `out_of_range` on a dead server); macOS: a use-after-free in the `workspace/symbol` reply build, found with TSan). 2026-09-27: with that fixed, the Windows leg's one remaining failure is *silent* — 70 tests start, not one assertion reports, and ctest still says `***Failed`, which can only mean the process never returned (MSVC is mute on a crash, a `std::terminate` and an unhandled exception alike). The suite now brackets each test with `[ DONE ]`, catches and attributes an escaping exception, prints a final line that splits a death inside a test from one in teardown, and on Windows prints the exception code and address, so the next run names the cause instead of leaving it to guesswork, and on the next run named it: the fixtures write CRLF to disk under an LF `didOpen` (a text-mode `ofstream`), and the scan was re-parsing the disk copy over the live buffer's entry, so cross-file ranges were measured against different bytes than the index described — `dim localOnly` at 1:5 instead of 1:4. The scan now skips open buffers; the condition is reproduced on Linux and both checks are verified non-vacuous. Two real latent index defects found while chasing the apparent hang (a lost wakeup in rescan shutdown, and a `std::thread` reachable from two threads) are fixed on their own merits, not as the cause. All five legs are green as of run `36293790173` (2026-09-27), the first run nothing failed on. Left: per-editor wiring docs, the first tag) |
| M19 — context-aware member completion (UDT members only) | done (2026-09: `p.` after a UDT variable completes only the owner type's accessible members — Public always, `Private:`/`Protected:` only inside the type's own member procedures (fbc's error-202 gate), qualified `EnumName.` members ungated; `.`/`->`/`with`-implicit/chained chains share the hover walk; unclosed blocks are closed at EOF so completion keeps working while a procedure is half-typed) |
| M20 — gettext localization of log + diagnostic messages | done (2026-09: system GNU gettext via `cmake/FindIntl.cmake` (`Intl::Intl`) + `FindGettext` tools; new `src/i18n.{h,cpp}` — `fblang::tr`/`trf`/`initI18n`/`setClientLocale` (domain `freebasicd`, UTF-8 catalogs, `InitializeParams.locale` honored best-effort); CMake `po-template`/`translations`(`ALL`)/`update-po` targets, committed `po/freebasicd.pot` + 29 msginit-generated `po/<lang>.po` (English is the msgid language — no en.po), install tree under `<prefix>/share/locale`; a `tests/i18n_checks` gate enforces "FreeBASIC/keywords are never translated" (structural scan of src/) + pot freshness + a CMake-built `de` catalog round-trip; all 14 suites green) |

## 2. What exists (condensed)

Module map — only new/modified modules are called out in §5; this is the
stable shape:

- `src/lexer.{h,cpp}` — tokenizer over the full FB surface (byte offsets,
  continuation-aware logical lines). `Token`, `TokenKind`, `Lexer`.
- `src/parser.{h,cpp}` — `parseDocument(src) -> ParseResult`
  (`roots`, `diagnostics`, `blockRanges`, `lang`); decl extraction, block
  matching, dialect detection, doc comments.
- `src/language.{h,cpp}` — reserved-word catalog, block-closer facts, wiki doc
  URLs, dialect detection helpers, `isSuffixChar`, and the 247-row `Intrinsic`
  catalog (`intrinsicFor`, `intrinsics`, `intrinsicDocsUrl`,
  `signatureParamLabels`, `statementPosition`, `expectedCloserAt` — the one
  answer for "which closer does the block opened here expect", shared by the
  inlay-hint labels and the M12 quick fix); the project-layout folder-name
  catalog (`isSourceDirName`/`isIncludeDirName` — a static 48+68 table of
  source/include directory names across ~30 languages, matched ASCII-
  case-insensitively) that names project roots for workspace detection.
  `isReservedWord` matches the keyword catalog case-insensitively, as fbc
  does.
- `src/analysis_cache.{h,cpp}` — `AnalysisCache`: content-addressed
  `ParseResult` + token vector per path (FNV-1a content hash as the identity),
  open-buffer entries exempt from FIFO eviction, `removePath` on close.
- `src/symbols.h` — shared model: `Symbol`, `SymbolKind`, `Diagnostic`,
  `ParseResult`, `SourceRange` (byte offsets), `toLowerChars`, and the
  `Access` visibility gate (`Public`/`Private`/`Protected`, stamped by the
  parser from a TYPE body's access sections).
- `src/resolve.{h,cpp}` — same-file resolution: `resolveAt`, `occurrencesOf`,
  `visibleSymbols`, `innermostScope`, `parentOf`, and the cross-file member
  chain: `declaredTypeName`, `findMember`, `findTypeDecl`,
  `resolveMemberAccess` (`.`/`->`, `with`-implicit, indexed/chained) and its
  completion twin `resolveMemberCompletion` (same chain walk, returns the
  owner type's members filtered by the `Access` gate + owner-context). Enum
  members of plain enums join the module name space via
  `moduleLevelCandidates` (explicit-enum members stay gated behind
  `Name.member`, §8 Enums).
- `src/index.{h,cpp}` — `WorkspaceIndex`: per-workspace symbol index, purely
  in memory (nothing is ever written to disk), background scan + debounced
  watched-files rescan threads, immutable `IndexedFile` entries + snapshot
  reads. Helpers: `normalizePath`, `statFile`, `resolveIncludeTarget` (six
  steps: including file's dir → config include dirs (step ②) → workspace root
  → the including file's project dir for out-of-root documents → immediate
  root subdirs → the fbc system folder).
  Each root stores its `Settings` snapshot (`applySettings` re-resolves every
  indexed entry's include edges against the configured dirs, no re-parse) and
  `ensureClosure` + the `FileResolver` alias build the transitive `#include`
  closure of an out-of-root requesting document on demand into a
  resolution-only store consulted by `fileAt`/`transitiveIncludes` but never
  by `snapshot`/`byKey` (workspace/symbol stays strictly workspace-scoped).
- `src/utf16.{h,cpp}` — byte ↔ UTF-16 position conversion (session boundary).
- `src/i18n.{h,cpp}` — GNU gettext wrapper: `tr(msgid)` (plain lookup),
  `trf(msgid, a0..a2)` (translates the template, inserts `%s` arguments —
  keywords, identifiers, file names, the proper noun `FreeBASIC` — verbatim so
  they never enter a translatable literal), `initI18n()` (domain
  `freebasicd`, UTF-8 output, environment message locale), and
  `setClientLocale(IETF tag)` wired to `InitializeParams.locale` (best-effort;
  only tags the OS can install switch the catalog). Translational invariants
  ("never translate FreeBASIC or keywords") are enforced as code by
  `tests/i18n_checks`; catalogs build from committed `po/*.po` via the
  `translations` target.
- `src/settings.{h,cpp}` — server configuration from a `freebasicd.toml` at a
  workspace root: `Settings{ includePaths, diagnosticsOn, semanticTokensOn,
  inlayHintsOn }` with fixed defaults, unknown keys ignored, malformed values
  never degrading a session. `hasConfigFile` marks a directory a workspace
  root (joins the VCS marker and source/include-layout signals); parsed with
  the vendored tomlplusplus. Consulted by `chooseIndexRoot` when narrowing a
  broad root or in single-file mode, by `ensureWorkspaceIndex` (each root
  adopts its file at construction), and by `workspace/didChangeConfiguration`
  (re-read on notification).
- `src/code_actions.{h,cpp}` — quick fixes (M12), LSP-agnostic and in byte
  offsets: `QuickFix`/`TextEditBytes` plus the `quickFixProviders()` registry
  keyed on diagnostic code, so a new fix is one row and one function and a code
  with no row offers nothing. Providers are pure functions of (diagnostic,
  `QuickFixContext`), which carries the document bytes, its analysis, an index
  snapshot, and the document's own `resolveInclude` seam — the last is what
  makes an include fix correct by construction (a candidate is offered only
  when the next publish would resolve it). Also owns
  `unresolvedIncludeDiagnostics`, shared with the publish path.
- `src/selection.{h,cpp}` — expand-selection chains (M13), LSP-agnostic and in
  byte offsets: `selectionChain(analysis, content, off)` returns the levels
  around one offset, innermost first — the token the cursor is in (or the one
  ending exactly at it), the `:`-separated statement segment, every enclosing
  `parse.blockRanges` entry sorted by size, and the whole file. A level is kept
  only when it strictly contains the level below it *and* adds some non-blank
  text, so an expansion is always a visible change (a block's range stops at the
  newline after its closer, so the file level would otherwise add one newline
  and nothing else). `:` is never a level, a blank line seeds the block walk at
  the cursor so the enclosing procedure survives, and an unterminated block has
  no range at all, so its level is absent rather than wrong.
- `src/selection_lsp.{h,cpp}` — the LSP seam for the above: `selectionRanges`
  converts a whole batch of chains (one per requested position, in order) and
  parks the nodes in a `thread_local` `std::deque` arena, because LspCpp links
  `SelectionRange::parent` as a *non-owning* pointer and the response vector
  owns only the innermost node. Safe because the reply is serialized inline on
  the handler thread; the integration test asserts the nested chain reaches the
  wire, which is what proves the lifetime.
- `src/call_hierarchy.{h,cpp}` — call hierarchy (M13), LSP-agnostic and in byte
  offsets: `callItemOf` copies a declaration into a `CallItem`; `procedureAt`
  answers which procedure an offset belongs to (innermost node, then out to the
  enclosing Sub/Function/Property/Constructor/Destructor — the analyzer own
  containment rule, reused); `outgoingCalls` scans the caller body for
  call-*shaped* tokens and resolves each through the `CalleeResolver` seam,
  merging by `DeclIdentity`; `incomingCallsIn` is the per-file half over the
  whole document, dropping module-level call sites (no enclosing declaration to
  be the `from` node). Three call shapes, all fbc 1.10.2 ground truth: `name(`
  (Name), after `.`/`->` (Member, parens optional), and a bare statement-head
  name (Statement). Callable kinds are Sub/Function/Constructor/Destructor — a
  property read is not a call and an operator is not reached as `name(`.
- `src/call_hierarchy_lsp.h` — the one type this feature needs locally:
  `td_callHierarchyOutgoingCalls`, registered under the protocol method name
  `callHierarchy/outgoingCalls` (LspCpp own is `callHierarchy/
  CallHierarchyOutgoingCall`) — the same precedent as `semantic_tokens_lsp.h`.
- `src/code_lens.{h,cpp}` — code lens (M13), LSP-agnostic and in byte offsets:
  `carriesLens` picks the kinds that get a lens (procedure-like plus the
  type-ish roots, nesting flattened); `collectAnchors` walks the symbol tree in
  source order (`std::stable_sort` on the name token start, skipping zero-width
  selections); `codeLenses` pairs each anchor with the count the
  `ReferenceCounter` seam returns and the `trn` title built from it. The
  `LensAnchor` carries no pointer — selection, name, kind — so the module never
  pins a snapshot and its suite needs no index.
- `src/session.{h,cpp}` — `FreeBasicServer` registers every handler, owns
  `WorkingFiles` + the per-workspace `WorkspaceIndex` map (`indexes_`, keyed
  by normalized root under `indexesMutex_`; registered client folders with a
  root marker, detected roots, and single-file roots each index independently),
  serves a content-addressed `AnalysisCache` (replacing per-request reparse),
  pushes diagnostics.
  Index-root selection (`chooseIndexRoot`, priority 0–5) uses a registered
  marker-root containing the file first; a client root that is itself a
  workspace root is used as-is; a *broad* client root is narrowed to the
  opened document's project — nearest VCS marker, then nearest config file,
  then the source/include layout walk (`findSourceLayoutRoot`, up to the drive
  root / `$HOME`) — and single-file mode roots at marker/config/layout or the
  file's directory; the two unbounded walks end at the home folder, which
  `isHomeFolder` identifies with `std::filesystem::equivalent` rather than a
  path compare (Windows spells `%USERPROFILE%` long and `%TEMP%` 8.3-short, so
  a compare misses the guard and the walk roots the index at the profile); a
  detected root that replaces the client's is logged to stderr with the
  signal. Watched-file events route to the owning root's index
  (`indexFor`); `workspace/symbol` aggregates the live indexes. A file outside
  every index root is served resolution-only through the session-root index's
  on-demand closure — never its own index or the single-file branch.
  `ensureRequestClosure` wraps the index walk with a resolver over the live
  open buffer (else disk) and runs before cross-file resolution, member hover,
  and completion.
  `workspace/didChangeConfiguration` re-reads every index root's config file
  (payload ignored, idempotent) and re-applies per-root `Settings`
  (`index->applySettings`, which re-resolves include edges); the root's open
  buffers are cleared/re-published to match the diagnostics flag and their
  include edges re-resolved on an `includePaths` change. Feature handlers gate
  on `settingsForDocument` (semantic tokens / inlay hints / code actions → empty
  results when off; diagnostics → empty publish per open buffer then silence).
- `src/main.cpp` — stdio entry; `LanguageSession` + exit condition.

Implemented LSP methods: `initialize`/`shutdown`/`exit`, `didOpen`/`didChange`/
`didSave`/`didClose`, `publishDiagnostics`, `documentSymbol`, `hover` (symbols +
member access + intrinsic signatures + keyword wiki links), `foldingRange`,
`definition`,
`references`, `documentHighlight`, `completion` (keywords + `END`-block
snippets + in-scope symbols + intrinsic catalog + context-aware UDT member
filtering after `.`/`->`), `signatureHelp` (user
declarations and built-in functions), `workspace/symbol` (aggregated across
per-root indexes), `prepareRename`,
`rename` (resolution-based workspace edits),
`codeAction` (M12 quick fixes: missing-include retarget + missing block closer,
answering from the diagnostics the next publish would carry),
`selectionRange` (M13 expand selection: token → statement → enclosing blocks →
file, one chain per requested position),
`prepareCallHierarchy` + `callHierarchy/outgoingCalls` + `callHierarchy/
incomingCalls` (M13 call hierarchy: nodes are procedures, edges are resolved
call sites merged per callee and per caller),
`codeLens` + `workspace/executeCommand` (M13: a "N references" lens per
declaration, counting the includers, whose click sends
`freebasicd.showReferences` and gets the location list back),
`workspace/didChangeWatchedFiles` (per-index routing),
`workspace/didChangeWorkspaceFolders` (per-root index add/remove),
`workspace/didChangeConfiguration` (per-root `freebasicd.toml` re-read +
`Settings` re-apply, feature-gate and include-seam behavior), and the
`workspaceFolders` capability (`supported` + `changeNotifications`).

## 3. FreeBASIC semantics that gate the remaining work

The full reference lives in `FreeBASIC.md`. These are the rules the forward
plan engineers around:

- **Module model.** A `.bas` file is one program; `.bi` files are shared
  headers. Cross-file visibility is **module-scope only** (top-level
  `shared`/`common`/`const`/`type`/`sub`/`function` facts) and exists **only
  through the `#include closure`** of a document. Procedure bodies see module
  names only if they are `Shared`/`Common Shared` — plain module-level `Dim`/
  `Common` is not visible in procedures even in-file (FreeBASIC.md §8,
  fbc-verified). Procedure-local names never cross a file boundary; a header
  never sees the `.bas` that included it.
- **Case-insensitive identity.** Canonical `key` = lowercase name including
  any type-suffix char (`foo$`, `i%`, …). The suffix is part of the token:
  lexers tie it to the identifier for resolution and edits.
- **Byte-offset core.** Lexer/parser/resolve work in byte offsets; UTF-16
  conversion happens only at the session boundary per file buffer. Any
  cross-file reply must convert against the *target* file's content.
- **Blocks close exactly** (`END SUB`, `NEXT`, `WEND`, `END IF`, …) as
  verified against fbc 1.10.2 — enforced by `language.cpp` closer facts.

## 4. Gaps — what is yet needed

1. Include edges and missing-include diagnostics are live (M6), but include-once
   *guard states* are not evaluated — `#include once` / `#pragma once` / `#ifndef`
   are processed as recorded metadata, not macros (FreeBASIC.md §12.6) — and
   `#inclib` is not treated as a source include.
2. Watched files and workspace folders are handled end-to-end (M5.5/M6/M11):
   a dynamic client is registered for `workspace/didChangeWatchedFiles` on
   `initialized` via `client/registerCapability`, a static one is served the
   watchers in the `initialize` reply, events fan into the owning root's
   `WorkspaceIndex::watchedFilesChanged`, and folder add/remove re-key the
   per-root indexes. `workspace/didChangeConfiguration` re-reads each root's
   `freebasicd.toml` (payload ignored) and re-applies `Settings` per root
   (include seam + feature gates). Force-disabling the fbc system include
   search (step ⑥) remains open.
3. No per-editor wiring docs yet. `README.md`, `cmake --install`, and the
   0.7.0 version landed 2026-09-25; so did `.github/workflows/ci.yml` — four
   `build-test` legs (Linux gcc and clang, macOS AppleClang, Windows MSVC), a
   Linux-only `clang-format` job pinned to 22.1.8, and an install-tree check
   (`tools/check_install_tree.cmake`) that asserts the binary, `LICENSE.md`,
   and all 29 catalogs. The matrix has now run on GitHub, and the three
   platform legs each failed first for a reason only that platform can show,
   which is the argument for having them at all:
   - **macOS** died at `#include <libintl.h>` in `src/main.cpp`. The header was
     found and the gettext include directory did reach `freebasicd_lang`; it
     never reached the executable, because `freebasicd_core` linked
     `freebasicd_lang` PRIVATE and a static library hands its private
     dependencies to consumers as `$<LINK_ONLY:...>` — on the link line,
     without the usage requirements. Linux hid it: glibc's `libintl.h` is in
     `/usr/include`, a default search directory. `freebasicd_lang` is PUBLIC
     now; the executable's flags gained `-I src`, which it had never had.
   - **Windows** died in its gettext step, then in configure three times over:
     mlocati splits the release and the `-dev-msvc` bundle has `libintl.h` and
     the import library but *no* tools (both bundles now go into one prefix);
     a `$root:` in a `throw` string, which PowerShell reads as a drive-qualified
     variable, so the script never parsed (actionlint and shellcheck only lint
     bash, so the pwsh blocks are now parse-checked locally with the real
     parser); a missing zlib that only ixwebsocket's unused websocket path
     wants (`USE_ZLIB=OFF`); and LspCpp asking a Visual Studio generator for
     seven boost nuget packages the build does not have (fork commits
     `a98ddce` + `9feb484`).
   - Two latent `cmake/FindIntl.cmake` defects surfaced with them and are
     fixed: its not-found branch could never be fatal (CMake does not turn a
     module's `<Name>_FOUND FALSE` into a configure error, which is *why* the
     macOS leg died in the compiler), and the `-DGETTEXT_ROOT` its header
     documented was never read.
   The editors' setup recipes are still open.
4. The install tree is **not relocatable**: `FBLANG_LOCALEDIR_INSTALL` is
   `${CMAKE_INSTALL_PREFIX}/share/locale` baked in at configure time
   (`src/i18n.cpp`'s probe order: `FBLANG_LOCALEDIR` env override, then the
   build tree, then that path). `cmake --install --prefix /somewhere/else`
   therefore leaves the binary unable to find its own catalogs unless the env
   override is set, which the README documents. The fix is to resolve
   `share/locale` relative to the executable's own path
   (`/proc/self/exe`, `_NSGetExecutablePath`, `GetModuleFileName`), or to drop
   `CMAKE_INSTALL_PREFIX` in favor of a relative lookup. Not done in the 0.7.0
   wave because it is platform code that only a real multi-platform CI run can
   verify.
5. The version reaches users only through a startup stderr line. The protocol
   has a place for it, `initialize`'s `serverInfo`, but LspCpp's
   `InitializeResult` models `capabilities` alone, so reporting it means a
   sixth commit on the fork branch (`lsp-3.17-completions`) plus a pin bump.
6. Windows localization is borrowed, not shipped. `cmake/FindIntl.cmake` needs
   a real libintl on Windows, and CI gets one from a downloaded
   `mlocati/gettext-iconv-windows` bundle (tools + MSVC import library +
   `intl-8.dll` in `bin`). So a Windows build links a DLL from outside the
   repo: an installed tree would start with the DLL beside the binary or not at
   all. `install(TARGETS ...)` does not install DLLs, so this is packaging
   work for the first release that ships a Windows binary — deciding between
   vendoring the DLL, static-linking libintl, or dropping gettext on Windows
   (all 29 catalogs are empty today, so an English-only Windows build loses
   nothing yet).
7. Pull diagnostics (M14) is unimplemented and unadvertised. None of it is
   required by the target editors; it ships as its own milestone. (The other
   feasible 3.17 extras are not: `codeLens` landed with M13, and
   `completionItem/resolve` waits on the M10 catalog making items heavy.)

## 5. Forward plan

Milestones are independently shippable: each leaves `ctest` green and carries
its own acceptance tests. Commits happen per milestone (AGENTS.md).

### M5 — Workspace spine: occurrence projection + include graph

The one structural investment everything cross-file derives from. Reshape the
index **before** building features on it.

- **Shared analysis.** One `analyze(source) -> AnalyzedDoc` (parse + token
  vector + includes + per-symbol occurrence sites), reused by the background
  scan, the open-buffer `upsert`, and request-side resolution so no layer
  diverges on what a document contains.
- **`IndexedFile` additions:** `includes` (resolved include targets + their
  source ranges + `once` flag), `occurrences` (def + resolved usage sites with
  a `moduleScope` flag), `fromDisk` flag.
- **Inverted projections** under the index mutex, maintained incrementally by
  `upsert`/`remove`: `byKey_` (lowercase key → sites across files) and
  `outInc_` (file → direct includes), plus `transitiveIncludes(file)` with a
  cycle guard.
- **In-memory only** (2026-09 revision): the M4 disk cache is removed — the
  index never writes to disk. The `persisted` flag narrows to `fromDisk`:
  false for open-buffer entries, and scan's mtime/size cache-hit never accepts
  one, so scan stays disk truth and buffers stay live truth.
- Files: `symbols.h`, `resolve.{h,cpp}` (shared `analyze` + occurrence sweep,
  legacy ParseResult wrappers internally analyze-backed), `index.{h,cpp}`,
  `session.cpp` (open-buffer upserts go through `analyze`, `fromDisk=false`),
  `resolve_checks` + `index_checks` cases. **Delivery deviation:** `parser.cpp`
  untouched (include extraction uses the public `preprocessorWord()` seam);
  `Storage`/`Shared` tagging deferred to M7 (`moduleScope` = "is a file root").
- Acceptance: `byKey_`/`outInc_` projections and the closure (diamond + cycle
  `a.bi`↔`b.bi`) hold in memory across scans/upserts; `fromDisk=false` entries
  never shadow scan hits; all 7 suites green.
- Risk: occurrence-vector memory for large workspaces (mitigate: sites only,
  no payload text; FB files are tiny). Residual: request-side re-analyze per
  call — removed by M10's content-addressed analysis cache.

### M5.5 — Lifecycle: `initialized` + dynamic capability registration

The transport the watched-file feature (M6) and config-driven watcher changes
(M11) need, and the first server→client request the server issues.

- Handle `initialized` (the client's first notification after `initialize`):
  read client capability `workspace.didChangeWatchedFiles.dynamicRegistration`.
  When true, send `client/registerCapability` for `workspace/didChangeWatchedFiles`
  (WatchKind create/change/delete, globs `**/*.{bas,bi}`); when false, carry
  static watchers in the `initialize` reply's `workspace.didChangeWatchedFiles`.
- Wire the LspCpp plumbing only (`initialized.h`, `registerCapability.h`,
  `did_change_watched_files.h` — all vendored). No watcher *logic* in M5.5:
  the registration target exists; M6 fills in the notification handler and the
  debounced rescan.
- Files: `session.{h,cpp}`, `session_integration` (init → `initialized` →
  observed registerCapability frame, plus a non-dynamic-client variant).
- Acceptance: after `initialized`, a dynamic client receives exactly one
  registerCapability request for watched files; a non-dynamic client sees the
  static watchers in the initialize reply.
- Risk: send-registration before the reply matters to some clients — sequence
  the registerCapability send as part of the `initialized` queue.

### M6 — Include resolution + convergence

Closed includes end-to-end and made the index converge on disk edits. Two
M6-plan bullets had effectively shipped early and are recorded here as
deviations rather than reworked.

- **Search policy shipped in M5 + amended** (deviation): `resolveIncludeTarget` — the
  including file's dir first, then workspace-root fallback — landed inside
  `index.{h,cpp}` during M5, so no `src/includes.{h,cpp}` was created; M6
  consumes it. Later amended (post-M6 bugfix) to widen the workspace search to
  every immediate child dir of the root (`inc`/`include`/`src` etc., so
  `#include "folder/file.bi"` matches under any of them) and to fall back to
  the FreeBASIC installation's own header folder found via `fbc` on PATH
  (Windows `<exeDir>/inc`, POSIX `<exeDir>/../include/freebasic`). fbc `-i`
  dirs landed as `Settings.includePaths` (M11 step ②, config-relative);
  force-disabling the system search (step ⑥) remains open (§4).
- **Include-not-found diagnostics (pushed, open files only):** the open-buffer
  publish path builds the `IndexedFile` once, reads back its resolved include
  edges, and emits an `include-not-found` `Error` covering the filename
  literal for every own `#include`/`#include once` whose literal resolved to
  nothing — one analysis, one resolution, one publish, merged with the parse
  diagnostics. Inter-file closure diagnostics wait for pull diagnostics (M14).
- **Watched-files convergence:** the session registers the vendored
  `Notify_WorkspaceDidChangeWatchedFiles` and fans every event into a new
  `WorkspaceIndex::watchedFilesChanged()`. A dedicated debounce thread (300ms
  trailing edge, its own cv/flag, started in `open()`, joined in `close()`
  before `scanner_`) coalesces bursts and runs one async full-root
  `scan(true)`, so the LSP notification FIFO thread never blocks on a scan.
  Event payloads are otherwise ignored: the registered glob
  (`**/*.{bas,bi}`) plus a re-stat of the root is authoritative and cheap for
  FB-sized files.
- **`#pragma once` recorded as metadata** (FreeBASIC.md §12.6): detected in
  `analyze()`, carried on `IndexedFile.pragmaOnce`. Recording only;
  guard-state evaluation is still a documented divergence.
- Files: `resolve.{h,cpp}`, `index.{h,cpp}`, `session.{h,cpp}`,
  `resolve_checks`, `index_checks`, `session_integration`.
- Acceptance (green): a `.bas` with a missing `.bi` publishes `include-not-found`
  over the literal; a resolvable `#include` does not; a `.bi` touched on disk +
  a watcher notification makes `workspace/symbol` return the new symbol without
  a restart; `#include once` and duplicate/cyclic paths stay out of the closure.
- Risk retired: LspCpp watched-file semantics verified against the vendored
  forks (M5.5 work). Residual invariant: the rescan thread is the only
  post-open `scan(true)` requester — the initial scan returns before any event
  can be handled, so `scanner_` is never written concurrently.

### M7 — Cross-file definition / references / highlight / completion

> Status: landed 2026-09, `ctest` 7/7 green. Realized as designed below, one
> detail beyond the sketch: `references` re-resolves the decl's own file to its
> local parse identity too (an afresh parse of a header cannot pointer-match the
> index entry), so the target's in-file usages are attributed by re-resolution
> exactly like every other site. The tier-3 leniency and the `.`/`..` escape
> hatch remain tracked §12 divergences.

The four consumer features share one new primitive in `resolve.cpp`, behind a
storage-tagging first step that the `FreeBASIC.md` §12.2 gate needs. This is
the design; sub-tasks land in order.

- **Storage tagging (§12.2 gate, sub-task 1).** `Symbol` gains `bool shared`
  (default false); the parser tags any **module-level** var declaration carrying
  the `Shared` modifier (`Dim Shared`, `Redim Shared`, `Common Shared`,
  `[Static] Var Shared` — wiki KeyPgShared syntax box) as `shared=true`; plain
  module-level `Dim`/`Common` stay `false`. Detection: a `seenShared` flag in
  `handleVarDecls` (today `shared` is silently skipped at parser.cpp:818),
  honored only while the parser's block stack is empty — `Shared` inside scope
  blocks is not supported (wiki KeyPgShared: "inside scope blocks … is not
  supported"), so procedure-local dims never carry the flag. One routing tweak
  beyond `handleVarDecls`: module-level `Static Shared x` today dies in the
  `static` branch (parser.cpp:417 requires the next token to be an
  *identifier*, but `shared` is a keyword) — make that branch treat
  `static` + keyword `shared` as a var decl too.
  Procedure/type/enum/**const** roots are storage-less and always visible:
  module-level `Const` was probe-verified to work inside a `Sub` with
  fbc 1.10.2 (wiki KeyPgConst is silent on this — resolved by probe). The gate
  applies in `declAt` (in-file resolution **and** the `analyze()` occurrence
  sweep) and in `visibleSymbols`: from inside a procedure body (its own
  control blocks included), module-scope `Dim`-kind candidates require
  `shared`; at module level — SCOPE/IF/FOR/... control blocks inherit
  module scope, so plain module dims stay visible inside them (probe:
  `Dim outer` at module level is visible inside a `SCOPE` block) — everything
  is visible. The gate therefore keys on a procedure boundary
  (`insideProcedureBody`), not on "inside any block". This fixes the §12.2
  over-resolution whose exact rule cross-file
  resolution then reuses. The §8 rows are wiki + fbc verified: plain module
  `Dim`/`Common` → "module-level only; NOT inside procedures" (probe:
  `Dim plain_v As Integer` + `Print plain_v` in a `Sub` → error 42; wiki
  KeyPgShared "the variable is only visible to the module-level code in that
  file", KeyPgCommon "The Shared optional parameter makes the variable global
  so that it can be used inside subs and functions"); `Dim Shared` →
  compiles and prints from a `Sub`. The include-closure side is probed too:
  `#include "globals.bi"` with `Dim plain_counter` in the `.bi` is visible at
  *module level* in the includer (compiles, prints) but error 42 inside an
  includer `Sub`, while `.bi` `Dim Shared` works from both — so the closure is
  treated as **one textual module** for resolution (tier-2 below), exactly the
  §9 model.
- **`resolveAcross`** (sub-task 2): `AnalyzedDoc` + the document's normalized
  path + `off` + `WorkspaceIndex const&` → three tiers: (1) in-file scopes,
  shadowing wins; (2) module scope of each closure file in
  `transitiveIncludes` textual pre-order, first key match (honoring the gate);
  (3) `byKey_` workspace fallback when the closure resolves nothing — a
  leniency for still-unincluded headers, recorded as a divergence in
  `FreeBASIC.md` §12. Returns a `CrossDecl{file, decl}`: `file==null` ⇒ `decl`
  points into the request-local `AnalyzedDoc`; else `file` is an index
  snapshot shared_ptr the caller keeps alive. `byKey_` indexes file roots
  only, so a procedure-local name in a `.bi` cannot resolve from a `.bas` by
  construction.
- **Session plumbing** (sub-task 3): a `contentForPath(path)` helper serves
  open buffers from `WorkingFile` and closed files from disk (lifting the
  `workspace/symbol` ifstream pattern); every remote range converts UTF-16
  against that file's own content; remote URIs build as
  `lsDocumentUri(AbsolutePath(normalizedPath))` — the proven
  `onWorkspaceSymbol` mapping. All four handlers drive `analyze()` once per
  request (dropping the per-handler `parseDocument`/`lexAll` churn).
- **Handlers** (sub-task 4): `definition` returns the resolved `CrossDecl`;
  the remote case converts the decl's `selection` against the target content.
  `references` = the target decl's own file's stored `occurrences`, plus
  identifier tokens in every closure file whose own in-file resolution reaches
  a module-scope decl of the target key (shadowing-aware re-resolution per
  site; `includeDeclaration` honored); out-of-closure `byKey_` hits are
  excluded (acceptance: closure only). `highlight` stays per-document (LSP
  semantics): the cross-resolved decl's in-document usages. `completion`
  merges closure module-scope roots (deduped by key) behind in-file
  `visibleSymbols` (gate applied), inner-scope keys shadowing closure keys.
- Files: `symbols.h`, `parser.{h,cpp}` (storage tagging), `resolve.{h,cpp}`
  (`CrossDecl` + `resolveAcross` + gate), `session.{h,cpp}` (four handlers +
  `contentForPath`), `resolve_checks`, `index_checks`, `session_integration`
  (two-file frames). **Deviation:** M5's
  "`Storage`/`Shared` tagging deferred to M7" lands exactly as deferred —
  `parser.cpp` is touched now. §12.1 suffix-collapse stays a tracked
  divergence (it touches the whole key model; not this milestone).
- Acceptance (all green): def at a call site in `main.bas` lands in `lib.bi`;
  a local dim shadowing a header global still resolves locally; a
  procedure-local name in a `.bi` never resolves from `.bas`; references
  enumerate only closure files; a plain module `dim` in a `.bi` resolves at
  *module level* in `main.bas` (textual include, probe-verified) but never
  inside a `main.bas` procedure (error-42-equivalent: resolves to nothing),
  while `dim shared` in the `.bi` resolves from procedures; a lenient
  out-of-closure `byKey_` def still works; the `shared` flag is carried on
  the in-memory index entries; `ctest` 7/7 green.
- Risk: closed files are read from disk per request for range conversion (FB
  files are tiny; M10's content-addressed cache serves repeat reads without
  re-reading or re-parsing). Duplicate
  module-scope keys across files disambiguate to the first closure hit — a
  documented edge case. The tier-3 leniency can point outside the closure —
  tracked as a divergence, per the plan's stated fallback. The `.`/`..`
  prefix escape hatch to reach a shadowed module global (wiki KeyPgDim
  dialect differences) is unmodeled: "shadowing wins" is absolute, matching
  fbc only for code that does not use the prefix — recorded as a §12
  divergence.

### M8 — `prepareRename` + `rename` (workspace)

> Status: landed 2026-09, `ctest` 7/7 green. Realized as designed, two
> details beyond the sketch: `occurrencesAcross` extends the candidate site set
> with reverse reachability — every file whose include closure reaches the
> declaration's file — so a rename issued at a header declaration covers all of
> its includers; and an invalid `newName` (keyword, digit-leading, lone `_`,
> suffix-only) is rejected up front in `onRename` before any site collection.

- `prepareRename`: return the identifier token range; error (not a renameable
  target) for keywords/non-identifiers.
- Rename is **resolution-based, never name-based**: collect candidate sites
  from `byKey_`, re-resolve each against its owning file, and edit only sites
  whose resolution is the target declaration (identical name elsewhere that
  shadows is untouched).
- Edits across files as `WorkspaceEdit.documentChanges`; closed files re-parsed
  on demand from disk, open ones from `WorkingFile`. Case-insensitivity means
  every occurrence's text is replaced with the user's `newName`; declaration
  token replaced too (suffix char rides with the token). Collision guard:
  reject if the new key matches an unrelated module-scope key in the closure.
- Capability: `renameProvider.prepareProvider = true`.
- Files: `session.{h,cpp}`, `resolve.cpp` (`occurrencesAcross`), tests driving
  two open files.
- Acceptance: rename of a global dim rewrites both files' sites; an
  unrelated same-named local elsewhere is untouched; local-only rename stays
  in-file; colliding rename is rejected.
- Risk: clients may refuse edits to unopened files — report as expected LSP
  behavior, not a bug.

### M9 — Semantic tokens + inlay hints + highlight grammar

> Status: landed 2026-09, `ctest` 10/10 green. Realized as designed, three
> deviations from the sketch: the vendored `SemanticTokensEdit` was reshaped to
> the wire `start`/`deleteCount`/`data` form (third local LspCpp commit
> `45846f7`) instead of adding a translation layer, so generic reflection is
> correct at every call depth (a later local LspCpp commit took the same route
> for the `codeAction` result type — see M12); only `full` results enter the
> delta cache (a `range` resultId is never a baseline, so a delta can never
> diff against a viewport-scoped set); and the grammar emitter is a shared
> `tools/grammar_emitter` module consumed by both the `gen_grammar` tool and
> `grammar_checks`, so the freshness gate byte-diffs by construction rather than
> regenerating into a temp dir. Follow-up: the enum conformance pass surfaced
> that enum *members* are module-scope constants (semantic-token
> `enumMember`, §8 Enums), the `explicit` keyword entered the catalog (grammar
> regen), and the editor grammars are now emitted as `editors/freebasic.vim`
> (the stale `editors/basic.vim` rename-orphan from `7be2612` was deleted).

Independent UX wins; LspCpp typed types confirmed present (`td_semanticTokens_full`,
`td_inlayHint`). Together these deliver the full editor-highlighting story:
semantic tokens are the LSP-native colorizer, and a static grammar is the
fallback every non-semantic-token editor (and every file pre-first-parse) renders
from. Both must agree with the lexer so highlighting never diverges from what the
parser sees.

- **Semantic tokens** (`src/semantic_tokens.{h,cpp}`): legend + `full` and
  `full/delta` from the cached token stream — `TokenKind` → `lsSemanticTokenType`
  (keyword, string, number, comment/preprocessor, operator/symbol); identifier
  classification via the symbol tree (known decl → kind) with a plain-variable
  fallback. Ship exact lexer kinds first, refine classifiers later. `range`
  (viewport) is declared **only if** the client asks for it — LspCpp vendors no
  `td_semanticTokens_range` type, so it needs custom-protocol plumbing; otherwise
  clients fall back to `full`, which both VSCode and Neovim accept.
- **Highlight grammar** (`editors/freebasic.tmLanguage.json` (TextMate),
  `editors/basic.vim`, `editors/README.md`): **generated from the `src/language.cpp`
  keyword catalog plus the lexer's suffix/operator/comment facts so the grammar
  cannot drift from the parser** — the same single source of truth the lexer
  honors (FreeBASIC.md is the root reference; the catalog is the machine form).
  Base scopes for `.bas`/`.bi`: comments (`'`, `REM`, nestable `/'...'/`),
  strings, numbers, `#`-preprocessor, `$`-metacommands, operators, and the
  dialect surface — mirrored to the semantic-token legend names. A committed
  generator script produces the files; edits go through the catalog, never the
  generated files.
- **Inlay hints** (`src/inlay_hints.{h,cpp}`), small scope: "expected closer"
  hints at block openers (`END SUB`, `NEXT`, `WEND`…) from `blockRanges` +
  `language.cpp` closer facts; optional inferred `AS type` on `dim` without a
  declared type.
- Files: new modules (`semantic_tokens`, `inlay_hints`) + `editors/` grammar +
  generator script + `session.cpp` (`semanticTokensProvider` capability with the
  legend, `inlayHintProvider`, handlers) + `session_integration`.
- Acceptance: a fixture yields correct keyword/operator/string token spans with
  UTF-16 (non-ASCII) offsets; a `range` client gets viewport-matching tokens or a
  correct `full` fallback; the generated grammar colorizes the same fixture with
  no unclassified tokens; block opener offers its closer hint; `ctest` green.
- Risk: token-type string spellings must match the 3.17 legend exactly;
  delta-encoding correctness (mitigate: full first, delta second); the grammar is
  easy to let rot — M18 CI regenerates it from the catalog so a catalog edit
  cannot ship without a matching grammar update.

### M10 — Intrinsic catalog + request-side parse cache

> Status: landed 2026-09, `ctest` 12/12 green. Both halves shipped. The cache
> is **content-addressed**, not version-gated: entry identity is an FNV-1a hash
> of the buffer (plus a size/head verify), so a `didChange` that bumps the
> version with byte-identical text still hits, and the open-buffer path never
> inserts (the `didChange` fill owns inserts) — staleness is structurally
> impossible rather than invalidated by hand. Cache entries own their content,
> so cached tokens stay valid. The catalog is one row per base key
> (`hasDollar` marks the `left`/`left$` alias), built from the wiki's function
> index and cleaned to 247 live rows; dead parser-only keys (`printpp`,
> `seekreturn`, …) are dropped. Its names are not all reserved — 33 are
> header-provided functions (`now`, `format`, `year`, …), so lookup relies on
> user-symbol precedence and `seen` dedupe, not a reserved-word assertion.

- **Intrinsic catalog** in `language.cpp` (pattern: the `keywordDocsUrl`
  per-word table): ~200 intrinsics (`Left$`, `Mid`, `Print`, `Val`, `CInt`,
  `Space$`, …) as `{ name, kind, signature, wikiSuffix }`; completion merges
  with keyword items, hover + `signatureHelp` consume it. Name-collision
  handling (`Left`/`Left$`) and statement-vs-expression position filtering.
- **Parse cache** in `session`: cache `ParseResult` + token vector per open
  document keyed by content (WorkingFiles version); invalidate on
  `didChange`. Removes the §4.3 repeated-full-parse across 10 handlers and the
  `lexAll`-per-`resolveAt` tax.
- Files: `language.{h,cpp}`, `session.{h,cpp}`, tests (`language_checks`,
  `session_integration`).
- Acceptance: known-intrinsic completion item carries the right signature +
  wiki link; two sequential requests on an unchanged buffer served from the
  same cached parse (identical result, no reparse observable).

### M11 — Configuration + workspace folders

> Status: landed 2026-09, `ctest` 13/13 green, `clang-format` clean. Both
> halves shipped. **Config half:** each index root owns its `freebasicd.toml`
> (`src/settings.{h,cpp}`, `Settings{ includePaths, diagnosticsOn,
> semanticTokensOn, inlayHintsOn }`, tomlplusplus vendored and pinned to
> `30172438` v3.4.0). `workspace/didChangeConfiguration` re-reads every live
> root's file — the notification payload is ignored, the file is the truth; the
> re-read is idempotent and a missing/comment-only file means defaults,
> unchanged. Each root's `WorkspaceIndex` stores the `Settings` snapshot under
> its mutex (`applySettings`), and the include seam consumes it:
> `Settings.includePaths` join `resolveIncludeTarget` as step ② (resolved
> absolute against the config-file dir, consulted in config order before the
> workspace-root search); on apply the include edges are re-resolved wholesale
> (`reindexIncludeEdges`, no re-parse, never a second scan), and the scan +
> open-buffer paths snapshot the dirs. The feature gates stay advertised and
> serve empty results when off: diagnostics off ⇒ one empty publish per open
> buffer then silence (on re-publishes; onDidClose always clears),
> semanticTokens — full: empty data + fresh *cached* resultId; delta: full-empty
> variant + fresh resultId; range: empty data + fresh *uncached* resultId —
> and inlay hints: empty result. Settings apply per root only (a
> multi-folder test flips one folder and leaves the sibling untouched).
> **Workspace-folders half shipped whole:** the single session index became
> `indexes_` — one in-memory `WorkspaceIndex` per workspace root, keyed by
> normalized path under `indexesMutex_` — plus the `workspaceFolders`
> capability (`{"supported":true, "changeNotifications":true}`, byte-verified
> by the integration test), a `workspace/didChangeWorkspaceFolders` handler
> that adds/removes per-root indexes, per-index watched-file routing (`indexFor`
> on the event path, deduped by owner root), and workspace/symbol aggregation
> across live indexes (`allIndexes()`, deduped by file). Root selection is the
> 0–5 `chooseIndexRoot` priority in §2. Files opened outside every index root
> are served resolution-only through the session-root index's on-demand
> closure — never their own index (would leak into workspace/symbol) and never
> the single-file branch while a client root exists. Deviations from the
> sketch: a config change never triggers a disk rescan by itself (config files
> are not watched; the open-buffer republish after `applySettings` is what
> re-resolves includes); `workspaceFolderRoots_` holds the registered folders
> that are themselves workspace roots, and a folder removal closes its root's
> index only while nothing else needs it; a present-but-malformed
> `freebasicd.toml` logs an error and keeps the defaults.

> Re-scoped (2026-09): the release-facing deliverables (README, editor setup,
> CI) moved out to M18. M11 is now the server-configuration and multi-root
> milestone; the public-release polish ships last, after the feature work.

- **Configuration** — `workspace/didChangeConfiguration` re-reads each root's
  `freebasicd.toml` (payload ignored, idempotent) and re-applies `Settings{
  includePaths, diagnosticsOn, semanticTokensOn, inlayHintsOn }` per root —
  few keys, fixed defaults, forward-compatible unknown-key ignore; a
  present-but-malformed file logs an error and keeps the defaults. The index
  honors `includePaths` as include-resolution step ② (config-relative, config
  order), wiring the per-workspace include seam §M6 already reserved; the
  feature gates serve empty results when the flag is off (exact semantics in
  the Status block). Config-driven watcher changes would ride M5.5's
  `client/registerCapability` path (unregister old globs, register new) — not
  implemented.
- **Workspace folders** — `workspace/didChangeWorkspaceFolders`: added folders
  get their own `WorkspaceIndex` (keyed by normalized root), removed ones
  close/scan-drop; single-root behavior stays the default. *Landed (2026-09).*
- Files: `src/settings.{h,cpp}`, `session.{h,cpp}`, `index.{h,cpp}`, tests
  (`settings_checks`, `session_integration`).
- Acceptance (green): a `didChangeConfiguration` with a new include path makes
  a previously-missing `#include` resolve — the re-published buffer no longer
  reports include-not-found and cross-file references reach the header; the
  three feature flags gate with empty-result/clear semantics; no-config
  sessions behave exactly as before (the pre-existing suites run configless);
  settings apply per root only; adding a folder to the workspace makes its
  symbols answer `workspace/symbol` and removing one drops them. Covered by
  `TestDidChangeConfigurationIncludePathResolves`,
  `TestDidChangeConfigurationDiagnosticsToggle`,
  `TestSemanticTokensAndInlayHintsGates`, `TestSettingsApplyPerRootOnly`,
  `TestMultiWorkspaceFoldersStayIsolated`, and
  `TestWorkspaceFoldersChangedAddRemove`.

### M12 — Code actions

> Promoted from the §6 "not doing (soon)" list (2026-09): thin while
> diagnostics were syntax-level only, now that M6 ships include diagnostics the
> two quick fixes below are cheap and high-value.

> Status: landed 2026-09, `ctest` 15/15 green, `clang-format` clean. Two
> deviations from the sketch below, both forced by how the diagnostics are
> actually produced, plus one protocol detail.
>
> **Registry, not a monolith.** `src/code_actions.{h,cpp}` holds the
> LSP-agnostic, byte-offset fix layer: `QuickFix`, `TextEditBytes`,
> `QuickFixContext`, and a `quickFixProviders()` table of
> `{diagnostic code, provider}` rows (`unterminated-block`,
> `include-not-found`) with `quickFixProviderFor(code)` doing the lookup. A
> provider is a pure function of (diagnostic, context); everything
> workspace-shaped enters through `QuickFixContext` (an index snapshot, the
> document's path, and a `resolveInclude` callback), so providers are unit
> tested with no index, session, or disk. A third fix is one row plus one
> function — no session, capability, or protocol change.
>
> **The include fix retargets instead of inserting.** The sketch said "insert
> the missing `#include` line at the top of the file", but the diagnostic is
> anchored on an *existing* directive whose literal resolved nowhere, so a new
> line leaves that edge unresolved and the diagnostic standing. The fix
> rewrites that directive's literal (the quotes are outside the diagnostic's
> range) to a workspace file whose name — or stem, so a mistyped extension
> still matches — fits. Candidates are expressed relative to the including
> file, and one is offered only when `resolveIncludeTarget` — the same seam
> `include-not-found` derives from — accepts it, so applying the edit resolves
> the edge by construction. Cap of 5, shallowest first; no candidate, no fix.
> The publish path and the fix key both build the diagnostic from
> `unresolvedIncludeDiagnostics` (now in the code-action module), which is what
> keeps their ranges from drifting apart.
>
> **The closer fix closes exactly one block.** The sketch said "inserted at
> the block end (from `blockRanges`)", but the parser closes every
> unterminated block at EOF, so `blockRanges` cannot say which block is inner:
> inserting the outer `END SUB` first would invert the nesting. Each fix
> therefore appends the *innermost* still-open closer (the one the parser
> reports first) at the buffer end, indented like its opener, in the buffer's
> line ending, above any trailing blank lines; the re-parse after each
> application makes the next enclosing block innermost, so applying the fixes
> in turn nests them correctly. The closer text comes from the new
> `expectedCloserAt` in `language.{h,cpp}`, which `inlay_hints.cpp` now also
> calls, so a hint label and a quick fix can never name different closers.
>
> **Response shape: a `CodeAction`, not a `Command`.** The protocol's
> `textDocument/codeAction` result is `(Command | CodeAction)[]`, and only the
> second variant can carry a fix: a `Command` is an id the client *executes*,
> and there is no standard id meaning "apply this edit". The first cut shipped
> `lsCommandWithAny` with an empty `command` and a serialized `WorkspaceEdit` in
> `arguments[0]` (LspCpp typed the response as `std::vector<lsCommandWithAny>`
> upstream) on the assumption that an empty command is the client's cue to
> apply `arguments[0]` — it is not, so the fix listed in the lightbulb and did
> nothing when picked. The `changes` key was a bare filesystem path for the
> same class of reason (`GetRawPath()` instead of the URI), so even a client
> that did read the edit could not match it to a document. LspCpp already
> shipped the `CodeAction` struct and the `TextDocumentCodeAction::Either`
> reader, so the fix is a local submodule commit (`50be209`): the
> request now answers with `std::vector<TextDocumentCodeAction::Either>` plus
> the missing `Either` writer, and the handler emits a typed `CodeAction` —
> `title`, `kind:
> "quickfix"`, the diagnostic it answers, and `edit.changes` keyed by the
> request's own URI, echoed verbatim so a percent-encoded or non-`file` scheme
> survives. Since every fix is a quickfix, `context.only` stays a server-side
> gate (`kindRequested`, segment-aligned prefix match, so `quickfix` serves
> `quickfix.something` but not `quickfixes`). The handler answers from what
> the *next publish* would report (parse diagnostics plus this document's
> unresolved includes, unioned with the client's `context.diagnostics`, deduped
> by code + start offset, filtered to the request range), so a client sending
> an empty context still gets its fix, and gates on
> `settingsForDocument(...).diagnosticsOn` like the publish path.
>
> Files: `src/code_actions.{h,cpp}`, `language.{h,cpp}` (`expectedCloserAt`),
> `session.{h,cpp}`, `inlay_hints.cpp`, tests (`code_actions_checks`,
> `session_integration`).

`textDocument/codeAction` returns fixes keyed off the published diagnostics
(`codeActionProvider = { codeActionKinds: ["quickfix"] }`); each fix is a
single-file `WorkspaceEdit` whose application clears the diagnostic on the next
publish.

- **Insert missing `#include`** — for the M6 `include-not-found` diagnostic:
  insert the missing `#include "literal"` line at the top of the file (the
  literal is already known from the include edge).
- **Insert `END` block closer** — for parse diagnostics where a block opener
  lacks its closer: propose the exact closer per the `language.cpp` closer
  facts (`END SUB`, `NEXT`, `WEND`, `END IF`, …) inserted at the block end
  (from `blockRanges`).
- Files: `src/code_actions.{h,cpp}`, `session.{h,cpp}`, tests
  (`session_integration`).
- Acceptance: each quick fix applies its edit and clears the diagnostic on
  re-parse; unrelated diagnostics offer no (code-action) fixes; `ctest` green.

### M13 — Editor extras: selectionRange, callHierarchy, codeLens

Three independently useful features; all build on the M7 closure/occurrence
primitives, none touches the language model.

- **selectionRange** (`src/selection.{h,cpp}`): innermost identifier token →
  statement/expression span (from `blockRanges`) → enclosing procedure/type →
  module. Cheap from the parse tree + token stream; powers expand-selection in
  every editor. Advertise `selectionRangeProvider = true`.
- **callHierarchy** (`src/call_hierarchy.{h,cpp}`): `prepareCallHierarchy`
  returns the targeted Sub/Function; outgoing calls = resolved Sub/Function
  identifiers inside its body; incoming = `byKey` filtered to call sites in
  closure files. Advertise `callHierarchyProvider = true`.
- **codeLens** (`src/code_lens.{h,cpp}`): "N references" lens on
  procedures/types from `byKey` (+ closure), cosmetic only. Advertise
  `codeLensProvider = { resolveProvider: false }`.
- Files: the three new modules + `session.{h,cpp}` + `session_integration`.
- Acceptance: expand-selection yields the token/statement/block chain; a
  two-file fixture shows outgoing and incoming calls; a referenced procedure
  carries a "2 references" lens.

> Status: all three aspects done (2026-09-27). One aspect per wave, as the
> milestones before it: each touches the wire shapes above and wants its own
> acceptance run.
>
> **Landed (aspect 1, 2026-09-27).** `src/selection.{h,cpp}` +
> `src/selection_lsp.{h,cpp}`, handler, `selectionRangeProvider = true`, the
> 16th suite `selection_checks`, and a `session_integration` test.
>
> The chain reads `(tokens, blockRanges, content)` only — the lexer has already
> merged `_` continuations, so one Newline-delimited token run *is* one
> statement, and `blockRanges` already carries Sub/Type/If blocks. The symbol
> tree adds nothing, so nothing in it is touched.
>
> Three rules, all learned from the implementation and each pinned by a test:
> a level is kept only when it *strictly contains* the level below it and adds
> non-blank text (LSP requires `parent ⊇ child`, and a keystroke that selects
> the same text reads as a broken expansion); a `:` separator is never a level
> (as the innermost level it is not contained by the statement segment around
> it, so it would suppress that segment); and a blank line seeds the block walk
> with a zero-width probe at the cursor, or the enclosing procedure would be
> skipped because no statement level exists to anchor the containment test.
>
> LspCpp's `SelectionRange::parent` is `optional<SelectionRange*>` — a
> non-owning pointer, while the response vector owns only the innermost node, so
> the ancestors must outlive the handler. They do: `sendSessionMessage`
> serializes inline on the handler thread under the send mutex, and
> `ResponseOrError(T&&)` moves the response (a vector move steals the buffer, so
> the arena's nodes stay put). Hence a `thread_local std::deque` arena cleared
> once per request — no fork commit. The whole batch of chains goes in one
> `selectionRanges` call, because per-position calls would clear the arena
> between chains and dangle the earlier ones; `session_integration` asserts the
> *nested* `parent` chain on the wire, which is what proves the lifetime rather
> than assuming it.
>
> The wire test's first cut failed for a reason worth keeping: its `didOpen`
> frame was one closing brace short, so the frame failed to parse, the buffer
> never opened, the handler answered `[]`, and the suite's
> `WaitForPublishedUri` — which hands back the last snapshot when it gives up —
> reported success anyway. The test now *counts* the publish. A fixture that
> builds a wire string by surgery fails as malformed input, not as a wrong
> answer, and a wait that can pass vacuously hides it.
>
> **Landed (aspect 2, 2026-09-27).** `src/call_hierarchy.{h,cpp}` +
> `src/call_hierarchy_lsp.h`, three handlers, `callHierarchyProvider = true`,
> the 17th suite `call_hierarchy_checks`, and a two-file `session_integration`
> test (a header declares the callee, the client calls it twice — once with a
> parameter list, once bare — and calls itself, so both directions cross the
> include boundary).
>
> Three call shapes, all checked against fbc 1.10.2 before anything was written:
> `name(`, after `.`/`->` (parens optional), and a bare statement-head name.
> fbc accepts `s`, `s 1` and `s()` in `fb` mode, while `Call s` is error 146, so
> there is no fourth shape to look for. Each shape resolves through the
> `CalleeResolver` seam (a bare name through `resolveAcross`, a member through
> the member resolver) and is then matched by `DeclIdentity` — the declaring
> file plus the name-token range — never by name, because `foo`/`foo$` are one
> symbol in `fb` mode. A local `dim helper` shadowing a `sub helper` is
> excluded by *resolving the site*, not by comparing text, and the suite pins
> it.
>
> Two filters are load-bearing, not cosmetic. A declaration own name token is
> call-*shaped* (`sub s(` is `s` followed by `(` at a statement head), so every
> declaration name is collected up front and skipped; without that, a recursive
> Sub reports itself as calling its own signature line. And keyword tokens are
> scanned in the Member shape only, because a reserved word is a legal member
> name (`sub open()` parses, and the parser records it) while an `if` or a
> `then` at statement position is not a callee — the resolver answers only for
> a name that resolves, so a false positive costs a lookup.
>
> A `Property` is a node but never a callee (a read is not a call, and the
> token stream cannot tell a read from a write without the assignment context),
> and an `Operator` is neither — it is reached through the operator, never as
> `name(`. A module-level call site is dropped from incoming calls: it has no
> enclosing declaration to be the `from` node, and a node the client cannot
> navigate to is not an answer. `prepareCallHierarchy` answers null at module
> level for the same reason.
>
> The target is identified by `uri` + `selectionRange` — both already in the
> item, and both needed to convert the follow-up ranges — so
> `CallHierarchyItem::data` stays unset: the field is optional in the protocol
> and nothing here could read an opaque blob back. That is the whole reason
> there is no fork commit beyond the local `td_callHierarchyOutgoingCalls`. The
> guess recorded above before the work started (that `data` cannot be written
> because the tree lacks a `Reflect` for `map<string, lsp::Any>`) was wrong —
> `Any` has a writer and the map does not — and it did not matter, because the
> field is not used.
>
> Lifetime follows the TSan rule in AGENTS.md. `OutgoingCall::to` is a value
> copy, so no symbol pointer escapes the outgoing scan; `IncomingCall::caller`
> is a raw pointer into the scanned document, so the session holds that
> document `shared_ptr` for the whole conversion loop — the pin is the pin.
> `resolve.{h,cpp}` gained `DeclIdentity::operator==`, `identityOf`,
> `innermostNode` and `referencingFiles` by promotion (the helpers were already
> there under local names; `sameIdentity` is gone), so the scan and the
> occurrence walk now share one definition of "the same declaration" and one
> candidate set for "who can name this file" — which is why incoming calls walk
> exactly the files `references` does.
>
> The suite first failed nine expectations, every one of them in the test own
> expected strings: a `find` with no anchor returns the *declaration* of a
> name, not its call site — the same first-match trap the selectionRange suite
> walked into with `off()`, and the reason each anchor in
> `call_hierarchy_checks` is a needle plus the snippet it sits after.
>
> **Landed (aspect 3, 2026-09-27).** `src/code_lens.{h,cpp}`, two handlers
> (`textDocument/codeLens`, `workspace/executeCommand`), `codeLensProvider =
> { resolveProvider: false }` + `executeCommandProvider`, a `codeLensOn`
> settings key, the 18th suite `code_lens_checks`, and a two-file
> `session_integration` test that also drives the command.
>
> The module owns two things and reaches for nothing else: *which* declarations
> carry a lens (the procedure-like kinds — Sub, Function, Property,
> Constructor, Destructor, Operator — plus the type-ish roots Type, Union,
> Enum, Namespace, with nesting flattened so a member procedure gets its own
> lens; Dims, Parameters and labels do not) and the localized title. The count
> comes in through one `ReferenceCounter` seam, a `size_t(LensAnchor const&)`,
> so the module has no workspace knowledge and its suite needs no index: one
> test counts the calls through a recording counter and pins that the anchors
> arrive in source order and that each is asked exactly once.
>
> The anchor is the declaration's **name token**, and that is also the `range`.
> A lens cannot carry an edit, so the click is a `Command` whose arguments are
> `[uri, position]` — the position being the anchor's own start, which is what
> identifies the declaration now that `resolveProvider` is false and there is no
> `data` blob for it. The uri is the request's own `raw_uri_` echoed verbatim,
> so the client can ask about a document it has since closed. The count is
> deliberately *not* carried: it is recomputed from the same walk the click
> answers, which is why the number on the lens and the list the command returns
> cannot drift.
>
> That made the reference walk's scope a decision rather than a default. It now
> takes an explicit `enum class ReferenceScope`: `Closure` is the request file
> plus its transitive includes — what `textDocument/references` answers, and
> unchanged, because that request is about the document the editor is in — and
> `Workspace` adds every indexed file that reaches the declaration's own file
> through the M6 reverse-reachability graph, which is what a lens should count
> (and what makes the count worth drawing: `hubProc` in a header reports the
> includer's call sites). `referenceSites` therefore had to stop comparing
> `Symbol` pointers: every candidate is parsed into its own analysis, so one
> declaration is a *different* object in each of them, and the `ownerFile` /
> `ownerLocal` re-resolution hack existed only to paper over that. Matching is
> by `DeclIdentity` (file + name-token range), which also gets shadowing right
> for free — a same-named local in a candidate file resolves to its own identity
> and is simply not a match. The count convention is fixed too: references
> *exclude* the declaration's own name token, which is what `references`
> reports with `includeDeclaration` false; a lens cannot ask for the other
> convention, and a lens that disagreed with the command it opens would be a bug
> report.
>
> The plural is the catalog's decision, not a two-msgid guess: `fblang::trn`
> wraps `ngettext`, because only the catalog's plural rule can serve a language
> with three or four forms, and `%s`-substitution is now shared with `trf`
> through `detail::substitutePercentS` (verbatim, no printf — the argument is a
> count rendered by `to_string`). The `po-template` target learned the `trn`
> keyword, and `i18n_checks` collects only its first argument, so the singular
> stays freshness-checked and the plural rides along in the same entry.
>
> The command answers `std::vector<lsLocation>` through `lsp::Any`, which is the
> one place this server parses JSON itself (LspCpp hands the arguments over
> untyped), so `GetType()` is checked before each read and an unknown command
> or a payload that is not a lens's answers `null` rather than reading a
> position out of a number. The list itself is the whole feature: drawing the
> panel stays client-side, and the shape here is the one a first-party editor
> extension consumes.


### M14 — Pull diagnostics (backlog)

`textDocument/diagnostic` + `workspace/diagnostic` + `workspace/diagnostic/refresh`
(3.17) as a client-negotiated alternative to pushed `publishDiagnostics`
(LspCpp types in `protocol_3_18.h`). Sets `DiagnosticOptions.interFileDependencies = true`
— driven by the M6 include-edge graph — so pull diagnostics can surface
`relatedDocument` reports. Push stays the default; only worth building if a
target editor prefers pull.

### M15 — Type/go-to + type hierarchy (backlog)

`typeDefinition`, `implementation`, and typeHierarchy need inheritance facts
(`Type ... : base`, `Interface`, `Extends`) the parser does not emit yet. Land
the parser edges first, then reuse `byKey` + closure — otherwise identical in
shape and plumbing to M7.

### M16 — Document links + completion resolve + polish (backlog)

`documentLink` over keyword/wiki URLs (hover already carries them), optional
`completionItem/resolve` once the M10 catalog makes items heavy, advertised
`willSave`, `window/logMessage` + `$/progress`/`workDoneProgress` for long
scans, and small telemetry. Individually tiny; bundle as one polish drop.

### M17 — FreeBASIC formatter (backlog, scope TBD)

There is no community formatter standard for FreeBASIC — the plan previously
called that "low payback / don't do". Reconsidered: the absence of a standard
is exactly what makes this high payback. Whoever ships the first real
FreeBASIC formatter sets the de-facto standard, and the LSP server is the
natural place for it (`textDocument/formatting`, `rangeFormatting`,
`onTypeFormatting`). Full scope (lexer round-trip fidelity, `:` vs line-split
policy, continuation `_` handling, comment/dialect preservation, integration
with the M5.5/6 file pipeline, format-on-type triggers) is deliberately
unspecified here; it gets fleshed out as a dedicated design pass before
implementation.

### M18 — Public release: install, version, editor setup, CI

> Moved out of M11 (2026-09): the user-facing and shipping artifacts land
> after the configuration/workspace-folder, code-action, and editor-extras
> milestones, at the end of the near-term plan.
>
> Status: partially landed 2026-09-25. The project is `freebasicd` 0.7.0
> (renamed from `freebasiclsp`), `README.md` ships with the repo,
> `cmake --install` lays down the binary, the message catalogs, and
> `LICENSE.md`, the hygiene files and issue forms are in, and the CI workflow
> is written and linted but has never run. No version tag, no packaged
> artifact.

- Done (2026-09-25):
  - The rename to `freebasicd` across the CMake project, the binary and static
    library targets, the gettext domain and `po/freebasicd.pot`, the
    `freebasicd.toml` config file, the runtime log/diagnostic `source`
    strings, and the docs. The fork's LspCpp branch was renamed
    `freebasic-lsp` → `lsp-3.17-completions` to name what it carries.
  - `README.md`: status (early, agentic-coding testbed), build/test,
    capability table, `freebasicd.toml`, architecture map, localization.
  - `project(freebasicd VERSION 0.7.0 ...)`, semantic versioning, bumped only
    at a release. The number reaches users through a startup stderr line; the
    protocol's `serverInfo` is **not** reported, because LspCpp's
    `InitializeResult` has no such field and adding it means a sixth fork
    commit.
  - `install(TARGETS freebasicd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})`
    plus the catalog tree and `LICENSE.md` (GPL requires shipping the license
    with the binary).
  - Public-repo hygiene: `CONTRIBUTING.md` (the three gates an outside
    contributor must pass, where things live, how to add a quick fix, how to
    patch LspCpp through the fork, translation rules, and the rule that agent
    authorship is disclosed in the PR), `CODE_OF_CONDUCT.md` (a short custom
    policy rather than Contributor Covenant, reporting to the maintainer),
    and `SECURITY.md` (GitHub private advisories, an in-scope list for an LSP
    server, out-of-scope list, and the "no CLA, no DCO" position inherited
    from GPL terms).
  - `.github/ISSUE_TEMPLATE/`: four issue forms (bug, fbc divergence, build or
    CI failure, feature request) plus a `config.yml` that routes security
    reports to the private advisory form and points language questions at the
    wiki. Each form asks for what that class of report actually needs, and
    nothing is a free-form "describe the problem": the bug form wants the
    feature, the dialect, a minimal `.bas`, the exact diagnostic, the stderr
    startup line that carries the version, and the client; the divergence form
    wants fbc's own output and whether `FreeBASIC.md` §12 already lists it;
    the build form wants the first error, the toolchain, gettext, and
    `git submodule status`.
  - `.github/workflows/ci.yml`, two jobs. `build-test` runs a four-leg matrix —
    Linux gcc, Linux clang, macOS AppleClang, Windows MSVC — through
    configure, build, `ctest` (15 suites), then `cmake --install` into
    `build/install-prefix` and `tools/check_install_tree.cmake`, which asserts
    the binary, `LICENSE.md`, and all 29 catalogs (a leg that silently skipped
    gettext therefore fails there instead of shipping an English-only tree).
    gettext is installed per platform: apt on Linux, `brew install gettext` +
    `GITHUB_PATH` on macOS (keg-only, so it is not on PATH by default), and
    the `mlocati/gettext-iconv-windows` bundle on Windows, which is the only
    way to get both the tools and an MSVC-linkable libintl there. The
    `clang-format` job is Linux-only and pins the formatter to the local
    version (22.1.8) through pipx, gating the same `git ls-files | xargs
    clang-format --dry-run --Werror` command AGENTS.md documents. The
    workflow is linted with `actionlint` (1.7.12) and its shell blocks with
    shellcheck; both the Linux legs and the install check were also run
    locally end to end. Its `paths-ignore` was `["**.md"]`, which skipped
    nothing — two docs-only pushes (`f1b3f51`, `eecbf81`) ran all five legs —
    so it now reads `["**/*.md"]`.
  - The first pushed runs, and what each failing leg turned out to be:
    - **Windows** died in `lspcpp.vcxproj` on `error C2589: '(': illegal
      token on right side of '::'` at `third_party/LspCpp/src/lsp/utils.cpp:594`
      — the UTF-16 offset conversion's `std::min`, with `min`/`max` arriving
      as function-like macros from that file's `<Windows.h>`. Only MSVC pulls
      in `Windows.h`, so no local platform can show it. Fixed in the fork
      (`8a67671`, the eighth local commit: `#define NOMINMAX` before the
      include, which is what `lsp.cpp` already does for its own).
    - **macOS** was green on four runs and then took `session_integration`
      with a segfault, in `TestSourceLayoutRootRecognizesCatalogNames` — the
      one test that hammers `workspace/symbol` while a background scan
      re-indexes the same paths. ThreadSanitizer found the cause in one run
      (9 reports, all one bug): `onWorkspaceSymbol`'s `FileMatches` held a
      raw `IndexedFile const *` out of `idx->snapshot()`, and the scan thread
      `upsert`s entries wholesale, so the reply build read a freed entry
      while doing per-file I/O. Linux reads stale-but-intact freed memory,
      so 12/12 local runs were green; macOS's allocator reuses the block and
      it crashes. Fixed by carrying the pin in the value
      (`FileMatches::file` is a `shared_ptr<IndexedFile const>`, and
      `MemberAccess` grew a `file` pin beside `member` for the same reason —
      both now match `CrossDecl`'s documented contract). TSan on the rebuilt
      binary: 0 reports. Green on every run since (the pin fix was the last
      change to touch it; several further pushes, all macOS legs green).
  - With macOS settled, the **Windows** leg turned out to have two more
    problems, both invisible to `ctest` and neither reproducible on Linux:
    - `grammar_checks` reported all three `editors/*` files stale. The Windows
      checkout rewrites LF to CRLF under the default `core.autocrlf=true`, and
      the gate byte-compares the committed files against an emitter that writes
      `\n` in binary mode. Reproduced locally by `sed`-ing CRLF into the three
      files — same three failures, same order. Fixed with a `.gitattributes`
      (`* text=auto eol=lf`), verified by forcing a re-checkout under
      `core.autocrlf=true` and confirming the files stay LF. `corpus_checks`
      reads files too and passed on Windows because it compares diagnostic
      codes, not bytes.
    - `session_integration` appeared to hang: 11 minutes with no output at all,
      killed by hand. **The two `src/index.cpp` defects fixed for this were
      real, and are still worth fixing, but they were not the cause** — the
      `[ RUN ]` instrumentation added below is what disproved that, and the
      real cause turned out to be in the tests. Read after the fact:
      (1) `close()` cleared `running_` without holding `rescanMu_`, so a
      store+notify could land in the window between `rescanLoop`'s predicate
      check and its block inside `wait()` — a lost wakeup, and `close()`'s
      `rescan_.join()` would never return. (2) `scan(true)` is reachable from a
      handler thread (`ensureWorkspaceIndex`) as well as from the rescan loop,
      and both do `scanner_.join()` then reassign `scanner_`: two threads on one
      `std::thread` is undefined behavior. Both are fixed (clear `running_`
      under `rescanMu_`; every `scanner_` join/reassignment under a new
      `scannerMu_`) and the new `index_checks` case races notifications against
      teardown 64 times and fails rather than hangs on a wedge. The case cannot
      reproduce either race deterministically — a lost notification leaves no
      trace — so it is a smoke test, not a reproducer.
    - The stall itself, once the suite could name itself: **not a hang at all.**
      The `[ RUN ]` lines and a `ctest --timeout 300` turned 11 minutes of
      silence into `14/15 ... ***Timeout 300.01 sec` with a legible failure
      list — 12 failing tests, and *the only* 12. Eleven are the
      `StartIndexedSession` tests, and the cause is that the fixtures built
      document URIs by gluing `"file://"` onto `path.string()`. On Windows
      that yields backslashes, and a backslash inside a JSON string is an
      escape: `\t` and `\f` corrupt the path silently and `\w` (from the `\ws`
      directory the fixture uses) is not a legal escape at all, so the
      `initialize` frame fails to parse, the server never answers, and each of
      those tests burns its full ~20 s poll budget before failing. A dozen
      20-second budgets *is* an apparent hang. Fixed by building every URI
      through LspCpp's `make_file_scheme_uri` over `fblang::normalizePath`
      output — the same call the server makes when it echoes a URI, so test and
      server agree byte for byte. Proof, not inference: `json.loads` on the
      Windows form reports `Invalid \escape` at the `\w`; and because
      LspCpp's drive-letter branch is a runtime check rather than a
      preprocessor one, a probe linked against `lspcpp` prints the exact
      Windows URIs from Linux (`file:///d%3A/tmp/...`, drive colon
      percent-encoded, which `RawPathFromFileUri` decodes back).
    - The same wave fixed the 68 hardcoded `file:///tmp/...` document URIs,
      which are not Windows URIs at all (they decode to a drive-less path).
      They now read `file://{{tmp}}/...`, expanded by a `MakeLspFrame` that
      shadows `test::MakeLspFrame`, so no call site changed; `{{tmp}}` resolves
      to a per-suite directory that is deliberately never created, which also
      stops the 60 in-memory tests from rooting their workspace at the shared
      temp directory and scanning each other's leftovers.
    - The twelfth test, `TestReferencesListAllSites`, is no longer unexplained:
      it passed on the run after the URI fix (it is among the 69 that ran, and
      it reported no failure), which fits the hypothesis it was left under — it
      sent a POSIX-shaped URI. The three experiments that had falsified the
      others stand: a colliding `dim counter` in a sibling file under the root
      (references is closure-scoped, so it cannot be hijacked), a torn read
      (LspCpp writes header+body as one `write`), and an index that has not
      caught up with the open buffer (`ensureRequestClosure` builds the entry on
      demand from the live buffer).
    - With the URIs fixed the suite stopped timing out and failed in 24 s with
      five assertions across three tests — and the server's own stderr in that
      log names the cause before any test does:
      `workspace root c:\users\runner~1 (detected via source/include
      directory; client root c:\users\runner~1\appdata\local\temp\fblsp-session-
      …\ws)`. The chosen root was the **profile directory**, for every test.
      `chooseIndexRoot`'s last two steps are unbounded upward walks that end at
      the home folder, and the guard that ends them was `start == home` — a
      compare of path *objects*, and the two spellings come from different
      places: `%USERPROFILE%` is `C:\Users\runneradmin` while `%TEMP%` (and so
      every document URI built from it, the fixtures' `FileUri` included) says
      `C:\Users\RUNNER~1`. Same directory, two spellings, guard missed, walk
      climbed out to the profile, which does hold a child whose name is in the
      source/include catalog — that is all the log's "detected via
      source/include directory" says.
      Everything the three failures need follows from that one root: the
      single-file test saw a sibling project it was told must stay out of the
      workspace (and every other test's sandbox with it), and the two
      `didChangeConfiguration` tests wrote `freebasicd.toml` into their client
      root and watched it be ignored — `settingsForDir` only ever looks in the
      chosen root, so the include stayed unresolved and the diagnostics toggle
      never fired. Note what this is *not*: the timeout's disappearance was the
      URI fix above, nothing here. This leg went from a 300 s timeout to a 24 s
      run that names five failed assertions, with the other four legs
      unchanged.
    - Fixed with `isHomeFolder` — `std::filesystem::equivalent` instead of the
      path compare, in both walks — and a regression test that does not need
      Windows to fail: `TestHomeFolderGuardAsksTheFilesystem` points `HOME` at
      the same directory twice, once as `<home>/src/..` and once through a
      symlink (a `ScopedEnv` shim, since MSVC has no `setenv`), and asserts the
      home folder's own layout project stays out of the workspace of a file
      below it. The symlink spelling is the load-bearing one: it is the case a
      *lexical* normalization cannot answer either, so a future refactor that
      "fixes" the guard with a normalized string compare stays green here and
      goes red on the Windows runner. Verified both ways on Linux: with
      `session.cpp` reverted the test roots the index at the home folder and
      fails its assertion, exactly as the Windows run did.
    - Run `36291181583` (the home-guard commit) took the Windows leg from five
      failed assertions in three tests to **one**, in 21.6 s, with all 70 tests
      running and the other four legs green: the three failures were the ones
      the profile-wide root explained, so the diagnosis is confirmed by its own
      cure. What is left is `TestCrossFileStorageGate`, whose module-level
      `definition` of a plain header dim into `lib.bi` came back without the
      header's range.
    - That one is the *harness*, and provably so without Windows:
      `PollRequest` resends a request until the answer contains its needle, and
      matched that needle against the cumulative output stream. Every `didOpen`
      publishes diagnostics for its document, so a needle naming a document is
      already in the stream before the first request is answered — the poll
      returned whatever that first reply said and stopped waiting. A probe whose
      reply can never name the header (`definition` at `main.bas:0:0`, needle
      `fix.libUri`) reproduces it on Linux: `"result":null` on attempt 0, with
      the header URI sitting in an earlier publish. Scoped to the reply, the
      same probe runs to its budget. The suite's three URI needles (`cdef`,
      `cg1`, `cby`) and the `"line":4` needle behind `chl` are all satisfiable
      from an earlier message, so all four polls were one-way.
    - Fixed by matching the needle in the region the caller inspects — the tail
      from this reply's id — with the `rfind` miss handled (the old
      `snapshot.substr(snapshot.rfind(id))` would have thrown `out_of_range` on
      the first timed-out attempt, so a dead server failed by exception rather
      than by assertion), plus a give-up line naming the prefix, the needle and
      the newest reply. `cg1`'s assertion now carries the reply it judged, since
      a give-up and a wrong answer are otherwise the same red line. Wall time is
      unchanged locally (14.5 s, 70 tests, no give-ups): no caller depended on
      the loose match.
    - Not established: the runner's reply text. The probe proves the mechanism
      exists and that the Windows assertion is consistent with it firing, not
      which of two candidates produced the premature reply — the `didOpen` still
      in flight when the request was handled, or the request beating the index.
      Note the index is not the obvious suspect here: both fixture files are
      *open buffers*, and an open buffer is upserted at `didOpen`, so
      `ensureClosure` finds `lib.bi` in `files_` before any scan runs (that is
      also why `/tmp` being tmpfs locally does not reproduce it). Scoped to the
      reply, the poll retries either way; if the answer was wrong rather than
      premature, the give-up line names it.
  - The lesson recorded in AGENTS.md's ThreadSanitizer section: TSan and ctest
    are both blind to a lost wakeup and to a shared `std::thread`, and both
    present as *silence*. Hence the two cheap guards that make the next one
    diagnosable instead of theoretical — `session_integration` prints a
    flushed `[ RUN ] <test>` line per test, and CI passes `ctest --timeout
    300` (ctest's own 1500s default let the stall hold the whole job).
    Verified on a synthetic always-sleeping test that ctest does print a
    timed-out test's captured output, so the last `[ RUN ]` line names the
    culprit. Paid for itself immediately: they converted a 25-minute
    hand-killed mystery into one readable failure list, which is what found
    the URI defect. The companion lesson, also in AGENTS.md: a fixture that
    builds a wire-format string by string surgery fails as *malformed input*
    rather than as a wrong answer, and a poll-based wait reports that as a
    timeout — so a bug in the harness can present as the platform being slow.
    - The next run, `36291880385` (the poll fix), cleared that assertion and
      left a failure with **no message at all**: 70 `[ RUN ]` lines, zero
      `Expect` messages, `***Failed` at 21.84 s. That is a conclusion, not a
      guess. `main` has one non-zero exit path (`return test::Failures() == 0 ?
      0 : 1`) and both `Expect` and `ExpectJsonEqual` print before
      incrementing, so a non-zero exit with an empty log means the process
      never returned: an access violation, `std::terminate`, or an unhandled
      exception, all of which MSVC reports silently. Not established: which of
      the three, or where. It died at the very end — the last test's two
      `workspace root` lines are the last output, and that test is the last in
      the file, so the death is inside `TestReferencesClosureReusesAnalysis`
      or in process teardown, the one place a crash is invisible.
    - Two candidates, neither proven. Masked-and-long-standing is the better
      fit: the previous run's tail is the same shape (same last test, same two
      root lines, same silence) and its wall time 0.3 s lower, so a crash was
      likely already there behind the visible assertion. New-from-the-poll-fix
      is the other: matching the needle in the reply means a poll whose answer
      never satisfies it now retries, and `TestReferencesClosureReusesAnalysis`
      — the last test, whose needle is a range a premature reply lacks — would
      fire its whole budget where it used to fire one request.
    - Answered with instrumentation rather than with another guess: the same
      doctrine as the `[ RUN ]` line above, applied to silence instead of to a
      stall. `RUN_TEST` brackets each test with `[ DONE ]` and catches an
      escaping exception (naming the test, counting it, so the rest of the
      suite still runs); `main` prints a final "all tests ran" line, which
      splits "died inside a test" from "died on the way out"; `std::set_terminate`
      reports what escaped the tests and whether an exception was active.
      Verified locally: an injected throw in a test is attributed, counted, and
      the run continues to the final line with exit 1, and a standalone probe
      confirms the terminate path prints `what()` and aborts (134). A Windows
      unhandled-exception filter was tried and removed: it printed a fault
      address nothing on a CI runner can resolve, and it pulled `<Windows.h>`
      into a cross-platform test file to do it. The `[ DONE ]` brackets, the
      final line and the terminate handler are what stayed.
    - It paid for itself on the next run, `36292815201`: the log carried the
      reply the assertion judged, and the reply was **right** —
      `"start":{"line":1,"character":5},"end":{"line":1,"character":14}` for a
      name the test expects at 4..13. So neither candidate from the previous
      wave was the cause: the answer was not premature, and it was not empty. It
      was a *correct answer to a different file*. No `[ SEH   ]` or
      `[ THROW ]` line appeared, so the process also returned normally this
      time (71 `[ DONE ]` lines and the final "all tests ran" line are both
      present) — the silent death was a one-off on that runner, not a standing
      defect, and it is not established what it was.
    - Root cause, and it is in the server, not the harness. The two-file
      fixtures write through `std::ofstream` in **text mode**, so MSVC wrote
      CRLF to disk under the LF text the `didOpen` carried: the file on disk and
      the open buffer were different bytes. The index then mixed them. The scan
      treated a `fromDisk=false` entry as a cache miss and re-parsed the disk
      copy over the live buffer's entry (`index_checks` asserted this as the
      intended contract), while `contentForPath` kept serving the *buffer* for
      range conversion. Byte offsets from one copy, line table from the other:
      every line the two disagreed on contributed one column, and line 1 of
      `lib.bi` came back at 1:5 instead of 1:4.
    - Fixed by inverting that contract: the scan now **skips** any path with an
      open-buffer entry. An open buffer outranks disk — the client owns those
      bytes until `didClose`, and the offsets in the index have to describe the
      bytes a reply is measured against. Closed files remain disk truth, which
      is what keeps the watched-files path working; that test needed the
      opposite setup to mean anything (it had been opening the very header it
      then edited on disk), so it now opens `main.bas` and edits a closed
      `lib.bi`, and the prime it waits for is reachable only through the scan.
    - Reproduced on Linux, which is the part that makes this a fixed defect
      rather than a plausible story. The mechanism is a text-mode `ofstream`:
      writing the fixture with an explicit CRLF translation, with the `didOpen`
      text left LF, reproduces the runner's reply byte for byte
      (`character 5`/`14`). Both new checks are verified non-vacuous with
      `src/index.cpp` reverted: `index_checks` fails 3 assertions, and
      `TestScanKeepsOpenBufferAheadOfDisk` fails with the runner's exact reply.
      The integration test proves its rescan ran before asserting — the symbol
      it waits for lives in a *closed* file only a scan reads — so it cannot pass
      on a rescan that never happened.
    - All five legs green on `36293790173` (Linux gcc + clang, macOS, Windows
      MSVC, `clang-format`), which is the first run where nothing failed on any
      platform. The troubleshooting scaffolding was then reduced to what earns
      its keep: the Windows unhandled-exception filter went (an unresolved fault
      address, plus a platform header in a cross-platform test), and
      `PollRequest`'s never-used `attempts` parameter went with it. What stayed
      is the six lines that named a real failure — `[ RUN ]`/`[ DONE ]` brackets,
      the per-test catch, the final "all tests ran" line — plus
      `std::set_terminate`, whose "no active exception" is the one message that
      names an exit-time death. A `-Wunused-function` pass over the suite found
      no dead helpers behind them.
- Left:
  - Per-editor wiring recipes under `docs/editors/`: neovim builtin LSP,
    minimal vscode client, emacs `lsp-mode`. Each installs the M9 grammar
    (`editors/`) and turns on semantic tokens. The README carries a generic
    stdio snippet until these land.
  - A first tagged release: `git tag` at the version the release notes claim,
    plus a CPack config if a downloadable artifact is wanted. Nothing else in
    this milestone needs to be invented for that.
- Files: `README.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`,
  `SECURITY.md`, `docs/editors/`, `.github/`, `.gitattributes`,
  `CMakeLists.txt`, `tools/check_install_tree.cmake`.
- Acceptance: wiring docs accurate end-to-end on all three editors; CI green on
  Linux/macOS/Windows; `cmake --install` produces a prefix whose binary runs
  with its catalogs.

### M19 — Context-aware member completion (UDT members only)

> Status: landed 2026-09, `ctest` 12/12 green, `clang-format` clean. Both
> halves shipped. The parser now recognizes TYPE-body access sections
> (`Private:`/`Public:`/`Protected:`, fbc: `:`-syntax only, only inside a
> `Type`; `Protected` ≡ `Private` until `Extends` lands) and stamps each member
> with an `Access` in `Symbol::access`; the `protected` reserved word joined
> the keyword catalog (with docs) and the TextMate/vim grammars were
> regenerated. Completion is context-aware: after a `.`/`->` the server
> resolves the chain with the *shared* hover walk (virtual member under the
> cursor, `with`-implicit leading dot, intermediate members walked through
> their declared types, cross-file via the request closure) and returns the
> owner type's members — never FreeBASIC keywords, globals, or intrinsics —
> filtered by the access gate: Public always, Private/Protected only inside
> the owner type's own member procedures. `EnumName.` completes all members
> ungated. Deviation from the sketch: hard type-qualified static-member
> completion (`T.counter`) is out of scope, and `dim T.m` — a fixed-sized type
> field whose name collides with the type — is captured as a plain member
> (fbc 1.10.2 accepts it as an inline field; noted in FreeBASIC.md §12).

- Parser stamps `Symbol::access` from a TYPE container's current section;
  `Private:`/`Public:`/`Protected:` at statement start are consumed as an
  empty statement everywhere (no phantom member in Unions or module-level),
  and only wire the gate inside `TYPE`.
- `resolveMemberCompletion` reuses the member-chain machinery
  (`collectMemberChain`/`chainOwner`/`walkIntermediateMembers` split out of
  `resolveMemberAccess` so hover's soft fallback is byte-for-byte preserved);
  the session branch runs before the keyword/symbol/intrinsic path and drops
  items only via the access gate.
- Unclosed blocks (a procedure still being typed) are closed at EOF in the
  parser, giving their symbols a range covering the rest of the source —
  regression: previously `range.end` stayed 0 until the closer, so
  containment inside a half-typed `sub` found no scope and completion/hover
  found nothing.
- Files: `src/symbols.h`, `src/parser.cpp`, `src/resolve.{h,cpp}`,
  `src/session.cpp`, `src/language.cpp`, `editors/` (grammars), tests
  (`parser_checks`, `resolve_checks`, `session_integration`).
- Acceptance: `p.` at module level completes x/y only (private excluded, no
  `dim` keyword, no intrinsics); inside `sub Position.set()` the private
  member appears; `->`/`with`/chained/`EnumName.` shapes complete; hover's
  fallback test suite stays green.

### M20 — Gettext localization of log + diagnostic messages

> Status: landed 2026-09, `ctest` 14/14 green, `clang-format` clean. System
> GNU gettext only (never vendored): `cmake/FindIntl.cmake` probes whether the
> C library provides gettext in libc and otherwise links libintl, defining
> `Intl::Intl` on plain CMake ≥ 3.16; `find_package(Gettext)` supplies the
> msgfmt/msgmerge tools and a local `find_program` adds xgettext (the bundled
> FindGettext locates only the first two). `src/i18n.{h,cpp}` wraps the
> runtime (`tr`, `trf` with up to three verbatim `%s` insertions,
> `initI18n`, `setClientLocale` from `InitializeParams.locale`); the
> `translations` target compiles every committed po to
> `<build>/share/locale/<lang>/LC_MESSAGES/freebasicd.mo` (installed to the
> prefix too), so a dev binary picks up its catalogs. The two
> never-translate invariants — the proper noun `FreeBASIC` and uppercase
> keyword spellings must never appear in a translatable literal — are
> enforced as code, and every literal must exist in the committed pot
> (freshness). Deviations: English is the msgid language so there is no
> en.po; the initial 29 po files are msginit-generated with empty msgstrs for
> translators to fill; under a C-ish default message locale glibc ignores
> `LANGUAGE`, so the functional round-trip part skips (77) while the
> structural checks still run.

- Runtime wiring: `src/i18n.{h,cpp}` — `initI18n()` binds domain
  `freebasicd` to the build-tree / install-prefix catalog (env override
  `FBLANG_LOCALEDIR`), forces UTF-8 via `bind_textdomain_codeset`, and
  activates the environment's *message* locale only (numeric/parsing facets
  stay at "C" so LSP output never depends on the UI locale);
  `setClientLocale(locale)` maps IETF `-` to the C library's `_` and
  best-effort switches `LC_MESSAGES` (returns quietly when uninstalled or
  malformed).
- Message surgery: every addDiagnostic site in `src/parser.cpp` and the
  stderr logs + missing-include diagnostic in `src/session.cpp` now go
  through `tr`/`trf`; keyword text (`ELSE`, `END SUB`, `LOOP`, …) and the
  word `FreeBASIC` only reach a user as dynamic `trf` arguments, so
  translators never see them (byte-identical English output — the
  session_integration substring assertions still pass unchanged).
- Toolchain (CMake, `if(GETTEXT_FOUND)`): `po-template` (xgettext:
  `--keyword=tr --keyword=trf:1 --flag=trf:1:c-format
  --add-comments=TRANSLATORS:`, extracts from `src/*.{cpp,h}` +
  `tools/*.cpp`) regenerates `po/freebasicd.pot`; `update-po` (msgmerge)
  refreshes the po files; `translations ALL` (msgfmt `--check`, so a
  placeholder drift against the pot becomes a build error) compiles every
  committed po under `${FBLANG_LOCALE_OUT}/<lang>/LC_MESSAGES`. Only
  `translations` runs in the default build — the committed pot/po sources
  stay untouched by a routine build.
- Language set: the human languages named in `src/language.cpp`'s
  source/include directory catalog (`isSourceDirName`/`isIncludeDirName`, 30
  entries) minus English = af cs da de eo es et fi fr hr hu id is it lt lv ms
  nl no pl pt ro sk sl sv sw tl tr vi — 29 committed po files.
- Tests: `tests/i18n_checks` — structural scan of src/ (adjacent-literal
  concatenation, identifier-boundary tokenization) fails on any translatable
  literal containing whole-word `FreeBASIC` or an uppercase keyword spelling
  (lowercase prose homographs like "for"/"not" stay allowed); pot freshness
  (every literal must be a msgid in the committed pot, including folded
  xgettext-wrapped msgids); functional round-trip over a CMake-built
  `fblang-test` de catalog (`LANGUAGE=de`, skip 77 when msgfmt was absent at
  configure time or the default LC_MESSAGES is C-ish, since glibc ignores
  LANGUAGE there).
- Files: `src/i18n.{h,cpp}`, `src/parser.cpp`, `src/session.cpp`,
  `src/main.cpp` (`initI18n()` at startup), `cmake/FindIntl.cmake`,
  `CMakeLists.txt` (gettext section + `i18n_checks`/`i18n-test-catalog`
  wiring), `po/freebasicd.pot` + 29 `po/<lang>.po`,
  `tests/i18n_checks.cpp`.
- Acceptance: `cmake --build` + `ctest` 14/14 green; `clang-format` clean;
  `po-template` then `update-po` reproduce the committed pot with no msgid
  drift; a `LANGUAGE=de` shell run of the built server logs translated
  messages once a translator fills `po/de.po`.

## 6. Not doing (soon)

- **QB / fblite / deprecated dialects** — current behavior (best-effort `fb`
  parse + `lang-mode` Information diagnostic) degrades gracefully; full dialect
  semantics is niche.
- **Debugger / DAP** — out of scope for a language server.
- **Recorded non-starters** (never scheduled): `moniker`, `linkedEditingRange`,
  `documentColor`/`colorPresentation`, the deprecated `declaration` alias —
  exercises for editors we do not target.
- **Scheduled but deferred** (each lives in §5 as a backlog milestone, M14–M17):
  pull diagnostics, type/go-to + type hierarchy, document links + completion
  resolve + protocol polish, and the FreeBASIC formatter (M17; high-payback —
  sets the de-facto standard, scope TBD by a dedicated design pass).

## 7. Cross-cutting engineering notes

- **Concurrency (implemented as-is):** LspCpp handler pool runs requests
  concurrently; the index is snapshot-based and mutex-guarded, responses build
  lock-free. New M5.5–M18 handlers must follow the same snapshot discipline
  (shared_ptr copies only).
- **Per-milestone acceptance:** `cmake --build` + `ctest` green, milestone
  deliverable complete, commit on `main`, push only on request.