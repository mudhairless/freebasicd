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
  | M21 — module model, constructors, and fbc's codes | next |
  | M18 — public release: editor setup docs, first tag | blocked on M21 |

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
  - **Module level carries statements, and which `main` they belong to decides
    what runs and when** (FreeBASIC.md §9, fbc-verified). The main module's
    module level *is* an implicit `main`; **every other** module's — including
    a library module's — is a load-time constructor that runs *before* it,
    silently and with no fbc diagnostic. There is no forward declaration inside
    a module, so a module-level call to a routine defined later is `error 42`.
    `Sub name() Constructor [priority]` is the explicit form, runs before its
    own module's module-level code, and is the **only** ordering control that
    means anything: `priority` 101–65535, relative only among prioritized
    constructors, and cross-module order is unspecified and not stable across
    link orders. A constructor is a procedure body, so a plain module-level
    `Dim` is invisible inside one. M21 implements this.
  - **Case-insensitive identity.** Canonical `key` = lowercase name including
    any type-suffix char (`foo$`, `i%`, …), which is what the symbol model keys
    on today. **In `fb` that is wrong and §12.1 tracks it**: fbc ignores
    identifier suffixes (warning 44) and aliases `foo`/`foo$` to one symbol.
    Suffixes are a `qb`/`fblite`/`deprecated` feature; `fb`'s suffix is the
    numeric-literal one (`100ul` is a `ULong`), so a diagnostic must not inherit
    the wrong half.
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
  5. **The module model is documented but not implemented** (FreeBASIC.md §9,
     M21). `Constructor`/`Destructor` are neither lexed nor parsed, a module's
     *main-ness* is not tracked, and the diagnostics that fall out of the model
     — module-level code outside the main module running before `main`, a module
     global read from a constructor without `Shared` (fbc `error 42`), two
     `Public` constructors colliding across modules (an fbc link error) — do not
     exist. Nothing knows that a `.bas` in an include closure may *be* the main
     module, and nothing can: an LSP never sees the build command line, so the
     project's **artifact kind** and **main module** are facts only the user can
     supply. M21 takes both as optional config with defaults that keep today's
     behavior.
  6. **Our diagnostic codes are our own, not fbc's.** fbc's whole catalog is
     data in its source — `error.bas` at tag `1.10.2`, 328 errors and 49
     warnings whose *array position is the printed number* (FreeBASIC.md §13)
     — so a reader who knows `error 42` learns nothing from our output. M21
     turns that table into generated, checked-in data with a staleness test
     rather than prose.
  7. **Diagnostics are not version-aware** (M21, second half). Nothing records
     which fbc version a finding assumes, there is no `fbc.path`/`fbc.version`
     config, and the two sites that locate `fbc` are independent:
     `findFbcExecutableDir` in `src/index.cpp` (used only for the include dir,
     behind a magic static that a config change would not invalidate) and a bare
     `std::system("fbc -version …")` in `tests/corpus_checks.cpp`. The measured
     basis for the shape: `error.bas` is byte-identical across 1.10.0–1.10.3,
     so versioning belongs on *behavior*, not on message identity.

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

### M21 — Module model, module constructors, and fbc's own codes

Make the module model (§9) something the server actually knows, and give the
diagnostics it implies fbc's numbers instead of ours. Gating milestone for M18.

**Core, in order** — each step is independently testable:

1. **Lex and parse `Constructor` / `Destructor`**, with optional `priority`.
   Reject the malformed forms the way fbc does: a non-empty parameter list
   (`error 1`), appearing on a `Declare` line (`error 3`), and `priority`
   outside 101–65535 (`error 189`). A `Static` UDT member procedure may be a
   module constructor and a non-`Static` one may not (`error 17`).
2. **A constructor is a procedure body**, so §8 applies inside one: a plain
   module-level `Dim` is invisible there and reading it is `error 42`. The fix
   is adding `Shared`, and we can offer it because both halves are known — this
   is a quick fix through the M12 provider table, not a session handler.
3. **Project type and main module, as optional config** — the two facts an LSP
   cannot derive (detailed below). Nothing requires them, and each has a
   default that keeps today's behavior.
4. **Module-level executable code outside the main module**, which step 3 makes
   stateable. The fix wraps the run in `Sub <name>() Constructor` — an explicit
   `Constructor` states *when* it runs, an implicit module body does not.
5. **Two `Public` constructors sharing a name across the include closure** is
   an fbc link error; report it while editing, case-insensitively. Say nothing
   about constructor *order*: fbc does not specify it, it is not stable across
   link orders (probed — two link orders of the same three modules interleave
   differently), so neither is any claim we make.
6. **The catalog as data.** Check in a generated table from `error.bas` at the
   pinned tag (FreeBASIC.md §13), the generator under `tools/`, and a test that
   fails when the checked-in table drifts from the script. A reader who knows
   `error 42` then learns something from our output.

**Step 3 in detail: what the config says, and what it defaults to.**

An LSP never sees the build command line, so two facts must be told to it or
guessed: what kind of artifact this is, and which module is `main`. Both go in
`freebasicd.toml`, both are optional, and both default to the conservative
answer:

| key | default | setting it buys |
|-----|---------|-----------------|
| `build.kind` | `"exe"` | `"dll"` / `"staticlib"` → no module is `main` |
| `build.main` | unset — unknown | names the main module of an exe |
| `fbc.path` | first `fbc` found on `PATH` | pins a different compiler |
| `fbc.version` | the version it reports | asserts the version findings assume |

**`kind` is the load-bearing one.** A dll or a static library has no process
entry point — probed, not assumed: `-lib` and `-dylib` builds carry module-level
statements with no diagnostic, emit them as load-time constructors
(`fb_ctor__*`, `__fb_DllMain_ctor`) with no `main` symbol, and run them before
the linking exe's `main` (FreeBASIC.md §9) — so in one of those projects every
module level is initialization, and step 4 can say that flatly. Only an exe
needs `build.main` on top, and only to sharpen it: with `main` set, step 4 can
say *"app.bas is the main module, so this level runs before it"*; with it unset
the finding is phrased against fbc's own default, which is the **first `.bas` on
the command line** (FreeBASIC.md §9) — "if this file is not the first source on
the link line, everything here runs before `main`". Both phrasings are honest —
they differ in how much they assert, not in what they report.

Defaults are what a project with no `freebasicd.toml` gets, so the no-config
case has to be *right*, not merely legal:

- `build.kind = "exe"` with `build.main` unset ⇒ today's behavior. A single-file
  project is an exe whose main is that file, and nothing changes.
- `fbc.path` falls back to the existing `PATH` search, so a machine with fbc
  installed diagnoses against that compiler with no setup at all, and
  `fbc.version` defaults to whatever it reports — correct for the compiler
  actually present, with zero configuration.

`build.main` resolves relative to the config file's own directory, like
`includePaths` today, and so is root-scoped like every setting since M11.
Contradictions are reported rather than silently resolved: a `build.main` naming
a file that is not in the index, or set at all on a non-`exe` `kind`, is a
configuration error worth saying out loud (AGENTS.md §Boundary discipline).

**Config shape.** The new keys go in `[build]` and `[fbc]` sub-tables rather
than as four more top-level keys. That is one helper in `src/settings.cpp`
(tomlplusplus indexes a sub-table the same way), and it is free *now* for a
reason that will not stay true: **no tag exists yet** (`git tag` is empty at
0.7.0), so reshaping `freebasicd.toml` costs nothing today and becomes a
breaking 0.x config change — a MINOR — the moment M18 ships. `README.md` and
`docs/install.md` document the keys and move with them.

**Second half, after the above is green** (gap 7): version awareness, behind
the `fbc.*` keys. A probe that compares the dotted number only, never the build
date `fbc -version` embeds; `findFbcExecutableDir` gains the `fbc.path`
override and stops being a once-only magic static, since a pin set by
`didChangeConfiguration` has to take effect; and `tests/corpus_checks.cpp`
resolves `fbc` through the same seam, so the corpus half cannot quietly test a
different compiler than the server diagnoses against. Findings carry
`since`/`until`, defaulting to "all 1.10.x" — most carry none, because
`error.bas` is byte-identical across 1.10.0–1.10.3 and what varies is
*behavior*, not message identity.

**Acceptance**

- `Constructor`/`Destructor` parse; each malformed form has a test naming fbc's
  code.
- The module-global-in-a-constructor diagnostic ships as a quick fix added
  through the M12 provider table — no `session.cpp` change (AGENTS.md §Quick
  fixes: adding a fix is one function plus one table row).
- Generated catalog table plus a staleness test, and our emitted codes are
  fbc's for the covered set.
- **No config key is required for any diagnostic to fire.** Each one either
  firms up a default or sharpens a phrasing, and a test asserts that a project
  with no `freebasicd.toml` reports exactly what it reports today — the
  no-config case is the default case, so it is the one that has to be right.
- Every divergence this bakes in is written into FreeBASIC.md §12, main-module
  unknowability above all.
- `ctest` green, changed files clang-format clean.

### M18 — Public release: first tag

**Gated on M21**: shipping a first tag with the module model documented but
undetected, and diagnostic numbers that do not match the compiler they stand in
for, would make the release the moment those become expensive to change.

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