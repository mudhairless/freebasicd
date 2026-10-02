#FreeBASIC LSP Server — Implementation Plan

> **This file is for work that is still to do.** What already shipped is in
> [`CHANGELOG.md`](CHANGELOG.md), which is the record of what landed, when, and
> what it cost — including the defects found on the way and the lessons worth
> keeping. Do not add a finished milestone here: add a dated entry to the
> changelog and delete the milestone's sketch from §5, leaving at most a pointer.
> The durable engineering rules are in [`AGENTS.md`](AGENTS.md) and the language
> facts in [`FreeBASIC.md`](FreeBASIC.md).

## 1. State summary

  The project is `freebasicd` (renamed from `freebasiclsp` 2026-09-25), version
  0.7.0 under semantic versioning — the number is bumped only when a release
  ships, never in an ordinary feature or fix commit. `ctest` is 18/18 green.

  LspCpp is vendored from our fork `mudhairless/LspCpp` at `e4b177e` (upstream
  `19150d12` plus eleven local commits) and supplies framing / JSON-RPC / typed
  3.17 messages. tomlplusplus is vendored, pinned `30172438` v3.4.0, and parses
  the server's config file. GNU gettext is the system library, never vendored
  (`cmake/FindIntl.cmake` + `FindGettext`), and localizes log and diagnostic
  messages from the committed `po/*.po` catalogs. The language layer is
  LSP-agnostic and byte-offset based.

  The full language reference — keyword catalog, block closers verified against
  fbc 1.10.2, dialect and scope rules — lives in `FreeBASIC.md`. This plan
  covers roadmap, architecture, and the remaining work.

  Milestones M1–M15, M19, and M20 are `done`; their entries, with dates and
  lessons, are in [`CHANGELOG.md`](CHANGELOG.md). What is left:

  | Milestone | Status |
  |-----------|--------|
  | M16 — document links + completion resolve + protocol polish (backlog) | next |
  | M17 — FreeBASIC formatter (backlog, scope TBD) | next |
  | M18 — public release: editor setup docs, first tag | in progress |

## 2. What exists (condensed)

  Module map — the stable shape. A new module is called out here when it lands
  (its changelog entry carries the design; this list carries the map):

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
    `Name.member`, §8 Enums). The type graph joins them here rather than in a
    module of its own: `TypeItem`, `typeOf`, `supertypes`, `subtypes`,
    `findVisibleMember` (own members, then each base in turn), and the
    `memberImplementation` / `memberDeclaration` pair.
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
    inlayHintsOn, codeLensOn }` with fixed defaults, unknown keys ignored,
    malformed values never degrading a session. `hasConfigFile` marks a directory
    a workspace root (joins the VCS marker and source/include-layout signals);
    parsed with the vendored tomlplusplus. Consulted by `chooseIndexRoot` when
    narrowing a broad root or in single-file mode, by `ensureWorkspaceIndex` (each
    root adopts its file at construction), and by `workspace/didChangeConfiguration`
    (re-read on notification).
  - `src/code_actions.{h,cpp}` — quick fixes (M12), LSP-agnostic and in byte
    offsets: `QuickFix`/`TextEditBytes` plus the `quickFixProviders()` registry
    keyed on diagnostic code, so a new fix is one row and one function and a
  code with no row offers nothing. Providers are pure functions of (diagnostic,
    `QuickFixContext`), which carries the document bytes, its analysis, an index
    snapshot, and the document's own `resolveInclude` seam — the last is what
    makes an include fix correct by construction (a candidate is offered only
    when the next publish would resolve it). Also owns
    `unresolvedIncludeDiagnostics`, shared with the publish path.
  - `src/selection.{h,cpp}` — expand-selection chains (M13), LSP-agnostic and in
    byte offsets: `selectionChain(analysis, content, off)` returns the levels
    around one offset, innermost first — the token the cursor is in (or the one
    ending exactly at it), the `:`-separated statement segment, every enclosing
    `parse.blockRanges` entry sorted by size, and the whole file. A level is
  kept only when it strictly contains the level below it *and* adds some
    non-blank text, so an expansion is always a visible change (a block's range
  stops at the newline after its closer, so the file level would otherwise add
    one newline and nothing else). `:` is never a level, a blank line seeds the
    block walk at the cursor so the enclosing procedure survives, and an
  unterminated block has no range at all, so its level is absent rather than
    wrong.
  - `src/selection_lsp.{h,cpp}` — the LSP seam for the above: `selectionRanges`
    converts a whole batch of chains (one per requested position, in order) and
    parks the nodes in a `thread_local` `std::deque` arena, because LspCpp links
    `SelectionRange::parent` as a *non-owning* pointer and the response vector
  owns only the innermost node. Safe because the reply is serialized inline on
    the handler thread; the integration test asserts the nested chain reaches
  the wire, which is what proves the lifetime.
  - `src/call_hierarchy.{h,cpp}` — call hierarchy (M13), LSP-agnostic and in
    byte offsets: `callItemOf` copies a declaration into a `CallItem`;
    `procedureAt` answers which procedure an offset belongs to (innermost node,
    then out to the enclosing Sub/Function/Property/Constructor/Destructor — the
  analyzer own containment rule, reused); `outgoingCalls` scans the caller body
    for call-*shaped* tokens and resolves each through the `CalleeResolver` seam,
    merging by `DeclIdentity`; `incomingCallsIn` is the per-file half over the
    whole document, dropping module-level call sites (no enclosing declaration
    to be the `from` node). Three call shapes, all fbc 1.10.2 ground truth:
  `name(` (Name), after `.`/`->` (Member, parens optional), and a bare
    statement-head name (Statement). Callable kinds are
  Sub/Function/Constructor/Destructor — a property read is not a call and an
    operator is not reached as `name(`.
  - `src/call_hierarchy_lsp.h` — the one type this feature needs locally:
    `td_callHierarchyOutgoingCalls`, registered under the protocol method name
    `callHierarchy/outgoingCalls` (LspCpp own is `callHierarchy/
    CallHierarchyOutgoingCall`) — the same precedent as `semantic_tokens_lsp.h`.
  - Type graph (M15) lives in `src/resolve.{h,cpp}` beside its siblings rather
    than in a module of its own: `TypeItem` is a type copied out of the symbol
    tree by value (the `CallItem` contract, so nothing hands out a pointer into
  a snapshot); `typeOf` answers which type a symbol at an offset has;
    `supertypes` follows the `extendsKey` edge nearest-first through the request's
    include closure, revisiting no key; `subtypes` is the workspace-wide half and
    answers from `WorkspaceIndex::extendingTypes` (the direct extenders), which it
    walks breadth-first into the whole subtree the protocol asks for;
    `memberImplementation` / `memberDeclaration` are the two ends of a member
    procedure's `declare`↔defined edge, separate names rather than one function
    with a direction flag; `findVisibleMember` is `findMember` plus the `extends`
    walk, so an inherited field resolves and appears in completion.
  - `src/type_hierarchy_lsp.h` — the three type-hierarchy request types this
    feature needs locally: `td_typeHierarchyPrepare` (the vendored
    `td_typeHierarchy` is typed as a bare `TypeHierarchyItem` where the protocol
    says `TypeHierarchyItem[] | null`) and `td_typeHierarchySupertypes` /
  `td_typeHierarchySubtypes` (absent from the vendored tree entirely, and
  their params embed the client's `TypeHierarchyItem` rather than a document and
    position) — the same precedent as `call_hierarchy_lsp.h`.
  - `src/code_lens.{h,cpp}` — code lens (M13), LSP-agnostic and in byte offsets:
    `carriesLens` picks the kinds that get a lens (procedure-like plus the
    type-ish roots, nesting flattened); `collectAnchors` walks the symbol tree
  in source order (`std::stable_sort` on the name token start, skipping
    zero-width selections); `codeLenses` pairs each anchor with the count the
    `ReferenceCounter` seam returns and the `trn` title built from it. The
    `LensAnchor` carries no pointer — selection, name, kind — so the module
    never pins a snapshot and its suite needs no index.
  - `src/session.{h,cpp}` — `FreeBasicServer` registers every handler, owns
    `WorkingFiles` + the per-workspace `WorkspaceIndex` map (`indexes_`, keyed
    by normalized root under `indexesMutex_`; registered client folders with a
    root marker, detected roots, and single-file roots each index
  independently), serves a content-addressed `AnalysisCache` (replacing
    per-request reparse), pushes diagnostics. Index-root selection
    (`chooseIndexRoot`, priority 0–5) uses a registered marker-root containing the
    file first; a client root that is itself a workspace root is used as-is; a
    *broad* client root is narrowed to the opened document's project — nearest VCS
    marker, then nearest config file, then the source/include layout walk
    (`findSourceLayoutRoot`, up to the drive root / `$HOME`) — and single-file
    mode roots at marker/config/layout or the file's directory; the two unbounded
  walks end at the home folder, which `isHomeFolder` identifies with
    `std::filesystem::equivalent` rather than a path compare (Windows spells
    `%USERPROFILE%` long and `%TEMP%` 8.3-short, so a compare misses the guard and
    the walk roots the index at the profile); a detected root that replaces the
    client's is logged to stderr with the signal. Watched-file events route to the
  owning root's index
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
    on `settingsForDocument` (semantic tokens / inlay hints / code actions →
  empty results when off; diagnostics → empty publish per open buffer then
    silence). The session also owns both diagnostics deliveries (M14):
  `documentDiagnostics` is the single definition of a document's payload (parse
  diagnostics plus its own unresolved include edges) that the push path and both
    pull handlers share, and `diagnosticsResultId` is that payload's identity —
  the content hash folded with the include edges and the diagnostics gate, so a
  changed id means the report changed. `pullDiagnostics_` (negotiated at
    initialize) picks the delivery, and `notifyDiagnosticsRefresh` is the server
  hint a pull client receives wherever the push path would have published — from
    the notification FIFO thread and from `WorkspaceIndex::onRescanCompleted`.
  - `src/main.cpp` — stdio entry; `LanguageSession` + exit condition.

  Implemented LSP methods: `initialize`/`shutdown`/`exit`,
  `didOpen`/`didChange`/ `didSave`/`didClose`, `publishDiagnostics`,
  `documentSymbol`, `hover` (symbols + member access + intrinsic signatures +
  keyword wiki links), `foldingRange`, `definition`, `references`,
  `documentHighlight`, `completion` (keywords + `END`-block snippets + in-scope
  symbols + intrinsic catalog + context-aware UDT member filtering after
  `.`/`->`), `signatureHelp` (user declarations and built-in functions),
  `workspace/symbol` (aggregated across per-root indexes), `prepareRename`,
  `rename` (resolution-based workspace edits),
  `codeAction` (M12 quick fixes: missing-include retarget + missing block
  closer, answering from the diagnostics the next publish would carry),
  `selectionRange` (M13 expand selection: token → statement → enclosing blocks →
  file, one chain per requested position),
  `prepareCallHierarchy` + `callHierarchy/outgoingCalls` + `callHierarchy/
  incomingCalls` (M13 call hierarchy: nodes are procedures, edges are resolved
  call sites merged per callee and per caller),
  `codeLens` + `workspace/executeCommand` (M13: a "N references" lens per
  declaration, counting the includers, whose click sends
  `freebasicd.showReferences` and gets the location list back),
  `typeDefinition` + `implementation` + `typeHierarchy` + its `supertypes` /
  `subtypes` follow-ups (M15, through the `Extends` graph; `resolve` is not
  implemented and the capability says so),
  `textDocument/diagnostic` + `workspace/diagnostic` +
  `workspace/diagnostic/refresh` (M14 pull diagnostics: negotiated via
  `textDocument.diagnostic` and advertised as `diagnosticProvider`; a client
  without it keeps the push path, and negotiating pull disables push for the
  session; `full` | `unchanged` against the client's `previousResultId`, with
  the include closure as `relatedDocuments` and a refresh hint fired on a
  watched-files rescan, a config change, an edit, or a close),
  `workspace/didChangeWatchedFiles` (per-index routing),
  `workspace/didChangeWorkspaceFolders` (per-root index add/remove),
  `workspace/didChangeConfiguration` (per-root `freebasicd.toml` re-read +
  `Settings` re-apply, feature-gate and include-seam behavior), and the
  `workspaceFolders` capability (`supported` + `changeNotifications`).
  - `tests/session_support.{h,cpp}` — the integration harness in `namespace
    fbtest`: the reporters, `RUN_TEST`, the `file://` encoder, `ScopedEnv`,
    `TwoFileFixture` + its six documents, `StartIndexedSession`, `PollRequest`,
    and the waiters. One translation unit per LSP feature
    (`tests/session_hover_checks.cpp`,
  `tests/session_pull_diagnostics_checks.cpp`, …) holds that feature's tests
    plus the wire frames only it sends, and exposes `void Run<Feature>Tests()`;
  `tests/session_integration.cpp` holds `main()` alone and calls each runner in
    a fixed order. One ctest suite, one process — the `[ RUN ]` / `[ DONE ]`
    markers that name a crashed test are a property of the process, and `ctest
    --timeout` prints one stream per test.

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

  1. Include-once *guard states* are not evaluated — `#include once` /
     `#pragma once` / `#ifndef` are processed as recorded metadata, not macros
     (FreeBASIC.md §12.6) — and `#inclib` is not treated as a source include.
     Force-disabling the fbc system include search (step ⑥) also remains open.
  2. The install tree is **not relocatable**: `FBLANG_LOCALEDIR_INSTALL` is
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
  3. Windows localization is borrowed, not shipped. `cmake/FindIntl.cmake` needs
     a real libintl on Windows, and CI gets one from a downloaded
     `mlocati/gettext-iconv-windows` bundle (tools + MSVC import library +
     `intl-8.dll` in `bin`). So a Windows build links a DLL from outside the
     repo: an installed tree would start with the DLL beside the binary or not at
     all. `install(TARGETS ...)` does not install DLLs, so this is packaging
     work for the first release that ships a Windows binary — deciding between
     vendoring the DLL, static-linking libintl, or dropping gettext on Windows
     (all 29 catalogs are empty today, so an English-only Windows build loses
     nothing yet).
  4. Pull diagnostics ships `full` | `unchanged`, and the per-item *delta* form
     is not implemented: LSP 3.17 defines a `DocumentDiagnosticReport` with
     `kind: "unchanged"` and an optional `relatedDocuments`, and the server answers
     `full` | `unchanged`. What is missing is `textDocument/diagnostic` with
     `previousResultId` and no item list, and no target editor asks for it. (The
     other feasible 3.17 extras are not: `codeLens` shipped with M13,
     `typeDefinition` / `implementation` / `typeHierarchy` shipped with M15 —
     `typeHierarchy/resolve` excepted, since prepare fills both lists — and
     `completionItem/resolve` waits on the M10 catalog making items heavy.)

## 5. Forward plan

  Milestones are independently shippable: each leaves `ctest` green and carries
  its own acceptance tests. A milestone that ships leaves this section: its entry
  goes to [`CHANGELOG.md`](CHANGELOG.md) and its sketch is deleted here. Commits
  happen per milestone (AGENTS.md).

### M16 — Document links + completion resolve + polish (backlog)

`documentLink` over keyword/wiki URLs (hover already carries them), optional
`completionItem/resolve` once the M10 catalog makes items heavy, advertised
`willSave`, `window/logMessage` + `$/progress`/`workDoneProgress` for long
scans, and small telemetry. Individually tiny; bundle as one polish drop.

Note the shape precedent from M12/M13: a fix that must carry an edit ships as a
`CodeAction`, and a thing the client executes ships as a `Command`. A
`documentLink` is neither.

### M17 — FreeBASIC formatter (backlog, scope TBD)

There is no community formatter standard for FreeBASIC — the plan previously
called that "low payback / don't do". Reconsidered: the absence of a standard
is exactly what makes this high payback. Whoever ships the first real
FreeBASIC formatter sets the de-facto standard, and the LSP server is the
natural place for it (`textDocument/formatting`, `rangeFormatting`,
`onTypeFormatting`). Full scope (lexer round-trip fidelity, `:` vs line-split
policy, continuation `_` handling, comment/dialect preservation, integration
with the file pipeline, format-on-type triggers) is deliberately
unspecified here; it gets fleshed out as a dedicated design pass before
implementation.

### M18 — Public release: first tag

Remaining of a milestone whose CI and install work landed 2026-09-25 and whose
`docs/editors/` wiring recipes plus `docs/install.md` landed 2026-10-02 (see
[`CHANGELOG.md`](CHANGELOG.md)):

- **A first tagged release**: `git tag` at the version the release notes claim,
  plus a CPack config if a downloadable artifact is wanted. Nothing else in this
  milestone needs to be invented for that. The changelog's `## [Unreleased]`
  becomes the released version heading with its date.
- Acceptance: a tagged version whose `CMakeLists.txt` number, README, and
  `CHANGELOG.md` heading agree.

## 6. Not doing (soon)

- **QB / fblite / deprecated dialects** — current behavior (best-effort `fb`
  parse + `lang-mode` Information diagnostic) degrades gracefully; full dialect
  semantics is niche.
- **Debugger / DAP** — out of scope for a language server.
- **Recorded non-starters** (never scheduled): `moniker`, `linkedEditingRange`,
  `documentColor`/`colorPresentation`, the deprecated `declaration` alias —
  exercises for editors we do not target.
- **Scheduled but deferred** (each lives in §5): document links + completion
  resolve + protocol polish (M16), and the FreeBASIC formatter (M17; high-payback
  — it sets the de-facto standard, scope TBD by a dedicated design pass).

## 7. Cross-cutting engineering notes

- **Concurrency (implemented as-is):** LspCpp handler pool runs requests
  concurrently; the index is snapshot-based and mutex-guarded, responses build
  lock-free. New handlers must follow the same snapshot discipline
  (shared_ptr copies only, and a result type that hands out a pointer into a
  snapshot must carry the pin — AGENTS.md §ThreadSanitizer).
- **Per-milestone acceptance:** `cmake --build` + `ctest` green, milestone
  deliverable complete, a dated `CHANGELOG.md` entry, commit on `main`, push only
  on request.