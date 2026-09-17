# FreeBASIC LSP Server — Implementation Plan

## 1. State summary

Repository `main`, clean working tree, `ctest` 7/7 green. LspCpp (vendored,
pinned `19150d12`) supplies framing/JSON-RPC/typed 3.17 messages; the language
layer is LSP-agnostic and byte-offset based. Full language reference (keyword
catalog, block closers verified against fbc 1.10.2, dialect and scope rules)
lives in `FreeBASIC.md`; this plan covers roadmap, architecture, and the
remaining work.

| Milestone | Status |
|-----------|--------|
| M1 — LspCpp bring-up (sync, capabilities, diagnostics push) | done |
| M2 — Lexer + parser language layer, dialects, fbc corpus | done |
| M3 — documentSymbol, hover, folding, definition, references, highlight, completion, signatureHelp | done |
| M4 — persistent workspace symbol index + `workspace/symbol` | done (2026-09: rev'd to an **in-memory-only** index — no on-disk cache) |
| M5 — workspace spine: occurrence projection + include graph | done |
| M5.5 — lifecycle: `initialized` + dynamic capability registration | done (2026-09: static/dynamic negotiated, registerCapability frame verified) |
| M6 — include resolution + watched files + missing-include diagnostics | done (2026-09: missing-include diagnostics, debounced watched-files rescan, `#pragma once` metadata) |
| M7 — cross-file definition / references / highlight / completion | done (2026-09: `resolveAcross` tiers, `Shared` storage gate, four cross-file handlers, two-file tests) |
| M8 — `prepareRename` + `rename` (workspace) | done |
| M9 — semantic tokens + inlay hints + highlight grammar | done (2026-09: full/delta + opt-in range tokens, block-closer/inferred-type hints, catalog-derived TextMate + vim grammars with a freshness gate) |
| M10 — intrinsic catalog + request-side parse cache | done (2026-09: content-addressed `AnalysisCache` behind a `ContentProvider` seam, plus a 247-row intrinsic catalog feeding completion/hover/signatureHelp) |
| M11 — configuration + workspace folders | next |
| M12 — code actions: quick fixes for missing includes + block closers | next |
| M13 — editor extras: selectionRange, callHierarchy, codeLens | next |
| M14 — pull diagnostics (backlog) | next |
| M15 — type/go-to + type hierarchy (backlog) | next |
| M16 — document links + completion resolve + polish (backlog) | next |
| M17 — FreeBASIC formatter (backlog, scope TBD) | next |
| M18 — public release: README / editor setup, CI (moved from M11) | next |

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
  `signatureParamLabels`, `statementPosition`).
- `src/analysis_cache.{h,cpp}` — `AnalysisCache`: content-addressed
  `ParseResult` + token vector per path (FNV-1a content hash as the identity),
  open-buffer entries exempt from FIFO eviction, `removePath` on close.
- `src/symbols.h` — shared model: `Symbol`, `SymbolKind`, `Diagnostic`,
  `ParseResult`, `SourceRange` (byte offsets), `toLowerChars`.
- `src/resolve.{h,cpp}` — same-file resolution: `resolveAt`, `occurrencesOf`,
  `visibleSymbols`, `innermostScope`, `parentOf`.
- `src/index.{h,cpp}` — `WorkspaceIndex`: per-workspace symbol index, purely
  in memory (nothing is ever written to disk), background scan + debounced
  watched-files rescan threads, immutable `IndexedFile` entries + snapshot
  reads. Helpers: `normalizePath`, `statFile`, `resolveIncludeTarget`.
- `src/utf16.{h,cpp}` — byte ↔ UTF-16 position conversion (session boundary).
- `src/session.{h,cpp}` — `FreeBasicServer` registers every handler, owns
  `WorkingFiles` + `WorkspaceIndex`, serves a content-addressed
  `AnalysisCache` (replacing per-request reparse), pushes diagnostics.
- `src/main.cpp` — stdio entry; `LanguageSession` + exit condition.

Implemented LSP methods: `initialize`/`shutdown`/`exit`, `didOpen`/`didChange`/
`didSave`/`didClose`, `publishDiagnostics`, `documentSymbol`, `hover` (symbols +
intrinsic signatures + keyword wiki links), `foldingRange`, `definition`,
`references`, `documentHighlight`, `completion` (keywords + `END`-block
snippets + in-scope symbols + intrinsic catalog), `signatureHelp` (user
declarations and built-in functions), `workspace/symbol`, `prepareRename`,
`rename` (resolution-based workspace edits).

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
2. `initialized` + dynamic capability registration landed (M5.5): a dynamic
   client is registered for `workspace/didChangeWatchedFiles` on `initialized`
   via `client/registerCapability`; a static client is served watchers in the
   `initialize` reply. The watcher handler and the debounced rescan landed in
   M6 (they fan into `WorkspaceIndex::watchedFilesChanged`); only
   `workspace/didChangeWorkspaceFolders` remains unhandled (single-root
   assumption, M11).
3. No README, editor-setup docs, CI matrix, or `didChangeConfiguration`.
4. Feasible 3.17 features are unimplemented and unadvertised: `selectionRange`,
   `callHierarchy`, `codeLens` (M13), and pull diagnostics (M14). None is
   required by the target editors; each ships as its own milestone.

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
  dirs, per-workspace include paths, and force-disabling the system search join
  later via §M11 `Settings.includePaths`.
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
  sweep) and in `visibleSymbols`: from inside any block, module-scope
  `Dim`-kind candidates require `shared`; at module level everything is
  visible. This fixes the §12.2 over-resolution whose exact rule cross-file
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
> correct at every call depth; only `full` results enter the delta cache (a
> `range` resultId is never a baseline, so a delta can never diff against a
> viewport-scoped set); and the grammar emitter is a shared
> `tools/grammar_emitter` module consumed by both the `gen_grammar` tool and
> `grammar_checks`, so the freshness gate byte-diffs by construction rather than
> regenerating into a temp dir.

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

> Re-scoped (2026-09): the release-facing deliverables (README, editor setup,
> CI) moved out to M18. M11 is now the server-configuration and multi-root
> milestone; the public-release polish ships last, after the feature work.

- **Configuration** — `workspace/didChangeConfiguration` + `Settings{
  includePaths, diagnosticsOn, semanticTokensOn, inlayHintsOn }`; few keys,
  fixed defaults, forward-compatible unknown-key ignore. The index honors
  `includePaths` on rescan, wiring the per-workspace include-search seam §M6
  already reserves. Config-driven watcher changes ride M5.5's
  `client/registerCapability` path (unregister old globs, register new).
- **Workspace folders** — `workspace/didChangeWorkspaceFolders`: added folders
  get their own `WorkspaceIndex` (keyed by normalized root), removed ones
  close/scan-drop; single-root behavior stays the default. Server-side
  settings apply per active folder.
- Files: `src/settings.{h,cpp}`, `session.{h,cpp}`, `index.{h,cpp}`, tests.
- Acceptance: a `didChangeConfiguration` with a new include path makes a
  previously-missing `#include` resolve on rescan; adding a folder to the
  workspace makes its symbols answer `workspace/symbol` and removing one drops
  them; single-root sessions behave exactly as before; `ctest` green.

### M12 — Code actions

> Promoted from the §6 "not doing (soon)" list (2026-09): thin while
> diagnostics were syntax-level only, now that M6 ships include diagnostics the
> two quick fixes below are cheap and high-value.

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

### M18 — Public release: README, editor setup, CI

> Moved out of M11 (2026-09): the user-facing and shipping artifacts land
> after the configuration/workspace-folder, code-action, and editor-extras
> milestones, at the end of the near-term plan. The
> `.github/workflows/ci.yml` scaffold is already committed; everything else
> here is new.

- `README.md`: build/test, capability table, position-encoding note, per-editor
  wiring (`docs/editors/` — neovim builtin LSP, minimal vscode client,
  emacs `lsp-mode`). Each wiring doc installs the M9 grammar (`editors/`) and
  turns on semantic tokens.
- `.github/workflows/ci.yml`: build + `ctest` on a Linux/macOS/Windows matrix
  (`checkout --recurse-submodules`); not enabled until the repo is pushed.
  Expect to fix Windows path handling in `index.cpp` defaults and any
  MSVC/LspCpp issues once it runs. The CI also regenerates the M9 grammar from
  the catalog (the M9 freshness gate) so a catalog edit cannot ship without a
  matching grammar update.
- Files: `README.md`, `docs/editors/`, `.github/`.
- Acceptance: README + wiring docs accurate end-to-end on all three editors;
  CI green on Linux/macOS/Windows once the repo is pushed and enabled.

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