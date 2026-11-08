#FreeBASIC LSP Server — Implementation Plan

> **This file is for work that is still to do.** What already shipped is in
> [`CHANGELOG.md`](CHANGELOG.md), which is the record of what landed, when, and
> what it cost — including the defects found on the way and the lessons worth
> keeping. Do not add a finished milestone here: add a dated entry to the
> changelog and delete the milestone's sketch from §5, leaving at most a pointer.
> The core engineering rules are in [`AGENTS.md`](AGENTS.md), their detailed
> form is in [`docs/development/`](docs/development/README.md), and the language
> facts are in [`FreeBASIC.md`](FreeBASIC.md).

## 1. State summary

  The project is `freebasicd` (renamed from `freebasiclsp` 2026-09-25), version
  0.7.0 under semantic versioning — the number is bumped only when a release
  ships, never in an ordinary feature or fix commit. `ctest` is 23/23 green.

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

  Milestones M1–M16, M19, and M20 are `done`; their entries, with dates and
  lessons, are in [`CHANGELOG.md`](CHANGELOG.md). The same is true of the
  unnumbered waves since: anonymous outline nodes and scope subtrees, fbc
  block-closer numbers, the false-positive clears, the preprocessor wave
  (defines/macros as symbols, branch-aware `#if`, 2026-10-09), and the
  code-identity migration (2026-10-10). What is left:

  | Milestone | Status |
  |-----------|--------|
  | M17 — FreeBASIC formatter (wave 2 complete 2026-11-02; summary in CHANGELOG.md) | done |
  | M21 — module model, constructors, and fbc's codes (complete 2026-11-06; summary in CHANGELOG.md) | done |
  | M18 — public release: editor setup docs, first tag | blocked on M24 and the VS Code extension |
  | M22 — fbc declaration-order parity: use-before-declaration (complete 2026-11-08; summary in CHANGELOG.md) | done |
  | M23 — fbc parser diagnostics: statements, placement, warnings (complete 2026-11-08; summary in CHANGELOG.md) | done |
  | M24 — resolve-layer type model + fbc semantic diagnostics (backlog) | backlog |
  | M25 — include-point parsing: the closure joins the name view (backlog) | backlog |
  | M26 — formatter wave 3: wrapping and the reserved options (backlog) | backlog |
  | M27 — lint diagnostics: the checks fbc does not have (backlog) | backlog |
  | M28 — inline assembly: parse the block (backlog) | backlog |

## 2. What exists (condensed)

  Module map — the stable shape. A new module is called out here when it lands
  (its changelog entry carries the design; this list carries the map):

  - `src/lexer.{h,cpp}` — tokenizer over the full FB surface (byte offsets,
    continuation-aware logical lines; a directive whose code tail is a fresh
    `_` joins the next physical line into one Preprocessor token). `Token`,
    `TokenKind`, `Lexer`.
  - `src/parser.{h,cpp}` — `parseDocument(src) -> ParseResult`
    (`roots`, `diagnostics`, `blockRanges`, `lang`); decl extraction, block
    matching, dialect detection, doc comments.
  - `src/preproc.{h,cpp}` — the free-standing preprocessor: the seeded
    `__FB_*` and `#define`/`#undef`/`#macro` tables, `#if`-expression
    evaluation (numbers and the string-valued `__FB_*`, `and`/`or`/`not`
    bitwise like fbc), the feed the parser drives (`PreprocFeedResult`:
    active, undecided, define to publish — an unknown `#if` identifier is
    undecidable and the whole chain is skipped, FreeBASIC.md §12.22), plus
    `firstLineSignature` (what a define/macro hover shows) and
    `normalizedValue`. A define is a `SymbolKind::Define` whose byte window
    `[name.beg, defineEnd)` `#undef`/redefinition closes.
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
  - `src/fbc_diagnostics.{h,cpp}` — the imported fbc diagnostic catalog as
    data: `fbcMessage(kind, number)` (name / text / `-w` level),
    `fbcMessageCount`, `fbcCode` (`fbc error: 42` / `fbc warning: 5`), and
    `fbcMessageDocsUrl` (the one wiki page a coded diagnostic links to as
    `codeDescription.href`). Generated from `tools/fbc_catalog.tsv` by
    `tools/gen_fbc_catalog.cpp` (the `fbc-catalog` target) into
    `src/fbc_diagnostics.inc`; `fbc_diagnostics_checks` byte-diffs the
    committed copy. Maps our output onto the compiler's own numbers, so a
    reader who knows `error 42` learns something from it.
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
  - `src/document_link.{h,cpp}` — include navigation (M16), LSP-agnostic and in
    byte offsets: `documentLinks(doc, resolve)` returns one `DocumentLinkItem`
    per `#include`/`#include once` filename literal the `IncludeResolver` seam
    accepts, in source order, with the range narrowed to the literal (quotes
    excluded), and `includeLiteralAt(doc, offset)` names the literal a cursor
    sits on, so `textDocument/definition` answers a click on an include with its
    header. Symbols are deliberately not linked — hover carries a keyword's wiki
    page, and a link on every keyword would underline the whole buffer to repeat
    it.
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
  `didOpen`/`didChange`/`didSave`/`willSave`/`didClose`, `publishDiagnostics`,
  `documentSymbol`, `hover` (symbols + member access + intrinsic signatures +
  keyword wiki links + preprocessor `#define`/`#macro` definitions),
  `foldingRange`, `definition` (symbols; an `#include` literal answers its
  header, M16), `references`,
  `documentHighlight`, `completion` (keywords + `END`-block snippets + in-scope
  symbols + intrinsic catalog + preprocessor defines/macros + context-aware UDT
  member filtering after `.`/`->`), `completionItem/resolve` (M16: attaches a
  declaration's `''` doc comment on demand, found by the `data` identity the
  item carries), `signatureHelp` (user declarations and
  built-in functions),
  `workspace/symbol` (aggregated across per-root indexes), `prepareRename`,
  `rename` (resolution-based workspace edits),
  `codeAction` (M12 quick fixes: missing-include retarget, missing block
  closer, reserved-word member rename, ignored keyword suffix — answering
  from the diagnostics the next publish would carry),
  `selectionRange` (M13 expand selection: token → statement → enclosing blocks →
  file, one chain per requested position),
  `prepareCallHierarchy` + `callHierarchy/outgoingCalls` + `callHierarchy/
  incomingCalls` (M13 call hierarchy: nodes are procedures, edges are resolved
  call sites merged per callee and per caller),
  `codeLens` + `workspace/executeCommand` (M13: a "N references" lens per
  declaration, counting the includers, whose click sends
  `freebasicd.showReferences` and gets the location list back),
  `documentLink` (M16: one link per `#include` filename literal that resolves,
  targeting the header file, the same navigation `definition` gives on that
  literal — an include opens a file, so the wiki-URL/keyword idea behind the
  original sketch is left to hover),
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
  - **Case-insensitive identity.** Canonical `key` = lowercase name, and in
    `fb` with a trailing type-suffix char dropped (`canonicalKey`,
    `language.h`), so `foo$`/`foo` are the one symbol fbc aliases them to
    (warning 44). Suffixes are a `qb`/`fblite`/`deprecated` feature; `fb`'s
    suffix is the numeric-literal one (`100ul` is a `ULong`), so a diagnostic
    must not inherit the wrong half. Two halves remain open (FreeBASIC.md
    §12.1): the ignored suffix is not warned on, and the preprocessor define
    table still keys a `#define` name with its suffix.
  - **Byte-offset core.** Lexer/parser/resolve work in byte offsets; UTF-16
    conversion happens only at the session boundary per file buffer. Any
    cross-file reply must convert against the *target* file's content.
  - **Blocks close exactly** (`END SUB`, `NEXT`, `WEND`, `END IF`, …) as
    verified against fbc 1.10.2 — enforced by `language.cpp` closer facts.

## 4. Gaps — what is yet needed

  1. Include-once *guard states* are not evaluated — `#include once` /
     `#pragma once` are processed as recorded metadata, not macros
     (FreeBASIC.md §12.6), and a header's `#ifndef` guard does not make a
     second `#include` of it a no-op. (Within-file `#ifdef`/`#ifndef`
     reachability *is* evaluated since the preprocessor wave; the include
     graph simply does not use guard states for dedupe.) `#inclib` is not
     treated as a source include. Force-disabling the fbc system include
     search (step ⑥) also remains open. (M25 evaluates the once-guards and
     dedupes the closure; the `#inclib` and step-⑥ items stay open.)
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
     work for the first release that ships a Windows binary. **Decided
     (2026-11-02): vendor libintl and build it on Windows**, replacing the
     downloaded bundle, so the installed tree carries its own localization
     instead of a DLL from outside the repo. Dropping gettext on Windows was
     rejected: an English-only Windows build is not acceptable even while
     all 29 catalogs are empty, because the plumbing has to be real in the
     shipped artifact for translations to land later without a packaging
     change.
  4. Pull diagnostics ships `full` | `unchanged`, and the per-item *delta* form
     is not implemented: LSP 3.17 defines a `DocumentDiagnosticReport` with
     `kind: "unchanged"` and an optional `relatedDocuments`, and the server answers
     `full` | `unchanged`. What is missing is `textDocument/diagnostic` with
     `previousResultId` and no item list, and no target editor asks for it. (The
     other feasible 3.17 extras are not: `codeLens` shipped with M13,
     `typeDefinition` / `implementation` / `typeHierarchy` shipped with M15 —
     `typeHierarchy/resolve` excepted, since prepare fills both lists — and
     `completionItem/resolve` shipped with M16.)
  5. **One diagnostic from the module model is still unimplemented.** The
     model itself shipped with M21 (complete 2026-11-06): `Constructor` /
     `Destructor` are lexed and parsed, `[build]` / `[fbc]` supply the
     artifact kind and main module the command line would have decided, and
     module-level code outside the main module and duplicate `Public`
     procedure collisions across a closure are both reported. What remains
     is a module global read from a module constructor without `Shared`
     (fbc `error 42`): a constructor is a procedure for §8 visibility, so
     the shared gate should fire there. It belongs to the resolve layer
     (M24).
  6. **The resolve layer's diagnostic codes are still our own, not fbc's.**
     The catalog is data (M21 step 5: `tools/fbc_catalog.tsv` →
     `src/fbc_diagnostics.inc`, byte-checked by `fbc_diagnostics_checks`) and
     the block-closer family already reports fbc's number and links its wiki
     page. The statement, placement, and warning families report fbc's
     numbers since M23 (complete 2026-11-08); the whole resolve layer still
     reports our codes — M24.
  7. **The name view and the single-file diagnostic gates see one file.** A
     document's name layer — `declAt`, `visibleSymbols`, plain-name completion —
     answers from the file's own parse, so a declaration that enters a program
     through `#include` (a header's `Extern`, `Dim Shared`, procedure, or type)
     is invisible to resolution and suggestions until a member walk reaches it.
     The single-file family gates cannot see it either: the M21 shared-gate
     shapes and the extern-in-`.bi` false reports of FreeBASIC.md §12.29 fire
     against a definition the compiler does see. (Diagnostics for included
     files already exist on the pull path — `relatedDocuments` +
     `workspace/diagnostic` — and push publishes open buffers only.) Closing
     this is M25.
  8. **Log messages are sent to the client inline, so any call site can send
     under `indexesMutex_`.** The M16 `window/logMessage` drop (2026-10-13)
     routes the existing stderr lines — the config-parse notes and the
     detected/workspace root lines — through `FreeBasicServer::logToClient`,
     which calls `endpoint().send(window/logMessage)` on the calling thread.
     That thread can be the background scan thread or a handler. The known
     offender was removed when `ensureWorkspaceIndex` was restructured for the
     M21 compiler probe: the root line and the settings notes are emitted
     outside the lock, so no current path holds `indexesMutex_` across a send —
     a slow client therefore cannot stall another handler's index snapshot.
     That is safe *today* — LspCpp serializes `send` under its own send mutex —
     but it is an argument no code enforces, which is why it is written down.
     The shape to build: one shared log FIFO, producers appending under a small
     log mutex and a single drain owning the send, which also collapses the
     interleaved `[freebasicd]` lines into one ordered stream and lets any
     thread log without a lock-ordering claim. Deferred deliberately; the
     direct send is not the final form.

## 5. Forward plan

  Milestones are independently shippable: each leaves `ctest` green and carries
  its own acceptance tests. A milestone that ships leaves this section: its entry
  goes to [`CHANGELOG.md`](CHANGELOG.md) and its sketch is deleted here. Commits
  happen per milestone (AGENTS.md).

### M22 — fbc declaration-order parity: use-before-declaration — complete 2026-11-08; summary in CHANGELOG.md

### M24 — Resolve-layer type model + fbc semantic diagnostics (backlog)

The largest diagnostic wave, and the one the parser alone cannot reach: fbc's
name and type checks run past the parse, and `src/resolve.cpp` emits no
diagnostics today. Its candidate set is Tier 4 of
[`research/fbc-diagnostics.md`](research/fbc-diagnostics.md). This milestone
gates M18. Build the type / overload / const-attribute model layered on
`ResolvedSymbol`, then judge a name, an argument list, a member access, and an
assignment against it and report fbc's numbers from M21's catalog:
`42 VARIABLENOTDECLARED`, `8 UNDEFINEDSYMBOL`, `4 DUPDEFINITION`, `20/58` type
mismatch, `181 ILLEGALASSIGNMENT`, `202 ILLEGALMEMBERACCESS`, `24
INVALIDDATATYPES`, `119 CONSTANTCANTBECHANGED`, `99/98` no-matching / ambiguous
proc, `1 ARGCNTMISMATCH`, `255 AMBIGUOUSSYMBOLACCESS`, `225–231 OVERRIDE*`, `71
INCOMPLETETYPE`, the type-dependent `54/28/63/73/120`, and the type-heavy
warnings (`1–5`, `10`, `13`, `40–43`, `49`).

- Resolve diagnostics get their own result (a field `documentDiagnostics`
  concatenates with the parse diagnostics) so the parse cache and the resolve
  pass stay separable — the index only parses.
- **The milestone fbc's `error 42` output for "use before declaration" (M22)
  depended on.** M22 shipped the declaration-order diagnostic (complete
  2026-11-08); M24 is the model that makes the rest of the semantic set
  honest, so schedule it with M22 in view.
- The semantic `warnings/` corpora are imported as goldens, not compiled
  against the external compiler at test time.

Acceptance: the covered set reports fbc's numbers and texts; a resolve gate
cannot leave a stale M14 `resultId`; `ctest` green, changed files clang-format
clean.

### M25 — Include-point parsing: the closure joins the name view (backlog, ordering undecided)

FreeBASIC.md §3 says cross-file visibility exists **only through the `#include`
closure** of a document. Today the *name* layer of a document — `declAt`,
`visibleSymbols`, plain-name completion, and the single-file diagnostic gates —
sees only the file's own parse, so a name a header contributes is invisible
until a member walk reaches it, and the M21 gate family reports declarations
the compiler sees through an include (the extern-in-`.bi` gap, §12.29). This
milestone makes the name view closure-aware and delivers included-file
diagnostics on both deliveries. **Ordering vs M22/M24 deliberately undecided:**
M25 supplies the include-point visibility M22's include-order rule needed, and
M24's resolve model (currently "the index only parses") would want it either
before or alongside; both orders are workable.

**Core, in order** — each step is independently testable:

1. **Locate and parse at the include point.** A document's include edges are
   resolved (`resolveIncludeTarget`) and each target's analysis is available
   before name resolution, so a fresh include never waits on the background
   scan: warm the closure synchronously for the requesting document (extend
   `ensureRequestClosure`, today member-access-only, to the name view) and
   keep it warm across `didChange` as edges re-resolve.
2. **The name view sees the closure.** `declAt`/`visibleSymbols`/plain-name
   completion gain the transitive include closure's module-level declarations,
   in include order, behind the includes that introduce them — so hover,
   definition, references, and completion reach header declarations, and the
   gates that keyed on "declared in this file" become closure-aware:
   `moduleSharedCandidate`/`hasSharedModuleRoot`/`declAt` see an
   `Extern`/`Dim Shared`/`Shared` declaration reached through an include,
   closing the §12.29 false-report gap. Cross-file replies still convert
   against the *target* file's content (§3 byte-offset rule).
3. **Evaluate include-once guards and dedupe the closure** (§4.1 first half):
   `#include once`, `#pragma once`, and `#ifndef` guard states make a second
   pull a no-op and dedupe the closure, so a header reached twice contributes
   one declaration set, not `duplicate-definition` noise.
4. **Diagnostics for included files, indexed-project files only.** The pull
   path already answers each transitive include via `relatedDocuments` and
   iterates every indexed file under `workspace/diagnostic`. This step (a)
   gates both to included files that are **indexed workspace entries** — an
   included header resolved through the fbc system folder (step ⑥) or an
   out-of-root include dir is served for resolution and suggestions but never
   diagnosed, because its parse is not the user's code and reporting it would
   leak off-project state into the report; and (b) delivers on **push
   best-effort**: when an open buffer's closure changes (open, edit,
   include-path change, watched-files rescan), publish diagnostics for the
   closed included indexed files, tombstoning a previously-reported file with
   an empty publish when it goes clean — clients may ignore diagnostics for
   documents they never opened, so best-effort is the honest capability to
   advertise, not a guarantee.

**Acceptance**

- A header declaration is suggestible, resolvable, and hoverable from an
  including document as soon as the include is on disk — no scan wait.
- The M21 family is silent for `Extern`/`Shared` names declared through the
  closure; the in-file controls still fire.
- A doubly-included header introduces each name once.
- Closed indexed headers' diagnostics arrive on pull (`relatedDocuments` +
  `workspace/diagnostic`) and on push; a system-folder or out-of-root header
  never ships diagnostics on either delivery.
- `ctest` green, changed files clang-format clean.

### M26 — Formatter wave 3: wrapping and the reserved options (backlog)

The formatter's next wave, carried over from M17 wave 2's plan (§9.7 of the
deleted `M17_PLAN.md`; the wave's summary is in [`CHANGELOG.md`](CHANGELOG.md)).
The option catalog behind the whole design is
[`research/formatter-design.md`](research/formatter-design.md).
Wave 2 deliberately stopped at trust: verbatim regions, honest config, true
range formatting. What is left is the work that changes where lines break,
plus the config fields the engine parses today and does not act on
(`Formatting.md` lists them as reserved):

- `ColumnLimit` wrapping, including `_` insertion, and `ContinuationStyle`
  WrapOnly / WrapAndJoin: an over-limit statement is split with a
  continuation, and existing continuations are kept, or joined and re-split.
- The `StatementSeparatorStyle` Split* forms: a colon-compound line split
  into one statement per line, the inverse of the compound handling the
  wave 2 follow-up fix made safe.
- `CallParentheses` / `NiladicCallParentheses`, the alignment options, and
  `NamespaceIndentation`: layout options the line model can express once a
  wrapping pass exists.
- `OneLineFormatOffRegex` and the `CommentPragmas` regexes: format-off
  regions selected by pattern, not by the pragma pair alone.
- Format-on-type, and dialect-aware `IntrinsicDollarSuffix`.
- `BreakBinaryOperators` and `AlignEscapedNewlines`, which decide where a
  wrapped line's operator and `_` sit.
- The `AllowShortIfStatementsOnASingleLine` and
  `AllowShortBlocksOnASingleLine` `Never` gates, which expand one-line
  blocks into block form; `SplitAll` pairs with them, per the research.
- The rest of the blank-line family: `SeparateDefinitionBlocks`,
  `EmptyLineBeforeAccessModifier`, `EmptyLineAfterAccessModifier`,
  `WrapNamespaceBodyWithEmptyLines`, and the `AtEndOfFile` and
  `AtStartOfBlock` halves of `KeepEmptyLines` (only `AtStartOfFile` is
  honored today).
- The indent and spacing fields whose behavior is currently fixed:
  `IndentCaseLabels`, `IndentAccessModifierSections`, `IndentExternBlock`,
  `IndentWrappedFunctionNames`, `BracedInitializerIndentWidth`,
  `ContinuationIndentWidth`, `FloatSuffixStyle`, `InsertLeadingZero`,
  `SpaceAfterPrintChannelHash`, `SpacesInParens` `InSubscripts` and
  `InCasts`, `SpacesInBracedInitializers`, `SpaceBeforeBracedInitializer`,
  and `RemoveRedundantParens`.
- The graphics remainder: `CoordPairSeparatorSpacing`,
  `GraphicsModeWordCase`, and `SpaceBetweenEmptyArguments`
  (`StepPrefixSpacing` already shipped).
- The comment and preprocessor remainder: `ReflowComments`,
  `SpacesInLineCommentPrefix`, `DocCommentPrefix`, `SortIncludes` with
  `IncludeBlocks`, and `DirectiveContinuationAlign`.
- The declaration rewrites: `CanonicalizeSpecifierOrder`,
  `PointerSpellingStyle`, `ConstQualifierPosition`, `DimTypePosition`,
  `TypeFieldDim`, `NextVariableStyle`, `CallKeywordStyle`, and
  `LetKeywordStyle`. These are in scope (decided 2026-11-02), and they are
  the wave's most delicate part: several reorder or respell tokens, which
  breaks the token-equivalence stance the shipped engine holds, so each
  lands behind its option with parse-equivalence tests, not just corpus
  idempotence.
- Three catalog options have no config field yet and join the schema when
  scheduled: `RemCommentStyle`, `PrintShortcutStyle`, and
  `IncludeCategories`.

Also outstanding from the research: the `ext` preset, measured from the
Extended Library project (Ebben's own style), planned alongside `FBdocs`
and `fbc` but never made. It is a preset file plus the measurement pass,
not engine work, and it belongs to this wave's tail.

Why it waits: wrapping needs a penalty and line-break model the engine does
not have, and it wants block ranges to break against. The parser already
computes `blockRanges` for its own consumers (§2), so the missing piece is
the model, not the ranges. The other precondition is already in place: the
corpus gate (`format_corpus_checks`, wave 2) exists, and every wrapping rule
lands against it, because a wrapper that is not idempotent over the corpus
is a defect the gate can name. Each option lands with engine tests and
corpus coverage of its shape, in the wave 2 pattern.

Acceptance: `ctest` green, changed files clang-format clean, and the corpus
idempotent under both presets with the new options exercised.

### M27 — Lint diagnostics: the checks fbc does not have (backlog)

The second diagnostics direction, distinct from M23 and M24. Those report
fbc's own findings with fbc's numbers; this milestone is the clang-tidy
lineage the project started from: checks for patterns fbc accepts silently
(shadowing, unused names, implicit-conversion hazards, dead code, and the
other classes that survive translation to FreeBASIC's semantics). The
mapping research is the first chunk of
[`research/diagnostic-tooling.md`](research/diagnostic-tooling.md): clang and
clang-tidy supply the check classes, Rubberduck VBA is the closest BASIC
relative with open analyzers to study, and fbc plus `FreeBASIC.md` are the
ground truth for what FreeBASIC's semantics actually are, so a check is
ported as a class and a set of trigger conditions, never as C++ behavior.

Most of the interesting checks need names and types resolved, so this
milestone stands on M24's resolve model; the parser-only classes can start
earlier. Each check lands with its trigger conditions stated against
`FreeBASIC.md`, a suppression story (the `suppress` routing and the per-line
comment form), and corpus goldens, in the M23/M24 pattern. This milestone
does **not** gate M18: the initial release bar is fbc parity (M21, M23,
M24), and the lint checks accrue after the tag.

### M28 — Inline assembly: parse the block (backlog)

`Asm` block bodies are opaque today: the parser skips their contents
wholesale, a stance landed with the fix for assembly being parsed as
FreeBASIC (an x86 `sub` instruction opened a phantom `Sub`). Nothing inside
a block exists for the server — no references, no highlighting, no
diagnostics — so a variable used only from assembly looks unused, and
rename or go-to-definition stop at the block's edge.

The goal is a real parser for the block's contents, in FreeBASIC's own
variant of Intel-syntax assembly: instructions and operands, registers,
labels, FreeBASIC comments, and references to the enclosing scope's
variables, parameters, and labels, resolved like any other reference.
That is the value: the block joins the program's name view instead of
sitting outside it.

**First pass, complete when** (scope set 2026-11-06): x86 only, in the
forms the GAS backend accepts (fbc assembles through GAS). Parsing,
reference resolution, and semantic highlighting for the block's contents;
diagnostics only where a probed acceptance boundary is crisp, in the
M23 pattern. arm64 is out of the first pass entirely.

**Probe first** — the wiki documents x86 only
([KeyPgAsm](https://www.freebasic.net/wiki/KeyPgAsm)) and leaves the rest
open, so each of these is established with the pinned fbc before it is
designed against, and the outcomes land in `FreeBASIC.md` as probed facts:

- Whether arm64 assembly is accepted at all, and if so in what syntax.
  (Out of the first pass either way; the answer decides whether a second
  pass exists.)
- The instruction-class boundaries inside x86: SSE instructions and
  privileged instructions outside DOS are known to fail in at least some
  forms, and no canonical source enumerates what works. Probe by class,
  not by instruction.
- Whether inline assembly survives the C-emitting backend (`-gen gcc`)
  at all — error, dropped, or passed through — since GAS is where the
  syntax normally lives. What the server should say about an asm block
  depends on which backend the code will meet, which it cannot see; the
  divergence, whatever it is, goes to `FreeBASIC.md` §12.
- The operand forms that reference FreeBASIC names (bare names, bracketed
  memory operands, size specifiers), since those are the forms the
  reference resolution has to cover.

This milestone does **not** gate M18.

### M18 — Public release: first tag

**Gated on M24**: the initial release bar is parity with fbc
diagnostics (decided 2026-11-02; M22 added to the gate 2026-11-06). M21
(complete 2026-11-06) delivered the module model and fbc's numbers for its
covered set, and M23 (complete 2026-11-08) the parser-reachable statement,
placement, and warning families; the resolve-layer semantic set (M24) is what remains of the parity the tag
claims. Shipping earlier, with the module model
documented but undetected and diagnostic numbers that do not match the
compiler they stand in for, would make the release the moment those become
expensive to change. The lint family (M27) is not part of the bar: it
accrues after the tag.

**Also gated on the VS Code extension** (below): a release whose largest
editor audience has no installable path to the server is a release in name
only.

Remaining of a milestone whose CI and install work landed 2026-09-25 and whose
`docs/editors/` wiring recipes plus `docs/install.md` landed 2026-10-02 (see
[`CHANGELOG.md`](CHANGELOG.md)):

- **VS Code: LSP support in the published extension.** The Marketplace
  extension Ebben already publishes (FreeBASIC syntax highlighting only, a
  separate project) gains a language client: it launches `freebasicd` and
  surfaces its diagnostics, navigation, and formatting, so a VS Code user
  installs one extension instead of following a wiring recipe. This repo's
  `docs/editors/` has no VS Code entry today, and the six recipes it does
  have (emacs, helix, kate, micro, neovim, vim) are all manual wiring. The
  extension work gates the tag.
- **Zed: a candidate, not a gate.** A Zed extension covers the same ground
  for that editor's audience. Decide before the tag freezes; ship it with
  the release if it is cheap, or let it follow.
- **A first tagged release**: `git tag` at the version the release notes claim,
  plus a CPack config if a downloadable artifact is wanted. Nothing else in this
  milestone needs to be invented for that. The changelog's `## [Unreleased]`
  becomes the released version heading with its date.
- Acceptance: a tagged version whose `CMakeLists.txt` number, README, and
  `CHANGELOG.md` heading agree, the diagnostics the server emits are fbc's
  at the parity M23 and M24 define, and the VS Code extension in the store
  launches the tagged server.

## 6. Not doing (soon)

- **QB / fblite / deprecated dialects** — current behavior (best-effort `fb`
  parse + `lang-mode` Information diagnostic) degrades gracefully; full dialect
  semantics is niche.
- **Debugger / DAP** — out of scope for a language server.
- **Recorded non-starters** (never scheduled): `moniker`, `linkedEditingRange`,
  `documentColor`/`colorPresentation`, the deprecated `declaration` alias —
  exercises for editors we do not target.

## 7. Cross-cutting engineering notes

- **Concurrency (implemented as-is):** LspCpp handler pool runs requests
  concurrently; the index is snapshot-based and mutex-guarded, responses build
  lock-free. New handlers must follow the same snapshot discipline
  (shared_ptr copies only; a result type that hands out a pointer into a
  snapshot must carry the pin). See
  [`docs/development/testing.md`](docs/development/testing.md),
  ThreadSanitizer.
- **Per-milestone acceptance:** `cmake --build` + `ctest` green, milestone
  deliverable complete, a dated `CHANGELOG.md` entry, commit on `main`, push only
  on request.