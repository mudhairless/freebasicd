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
| M4 — persistent workspace symbol index + `workspace/symbol` | done (rev'd 2026: platform index dir, SHA-256-keyed per-file cache) |
| M5 — workspace spine: occurrence projection + include graph | done |
| M5.5 — lifecycle: `initialized` + dynamic capability registration | next |
| M6 — include resolution + watched files + missing-include diagnostics | next |
| M7 — cross-file definition / references / highlight / completion | next |
| M8 — `prepareRename` + `rename` (workspace) | next |
| M9 — semantic tokens + inlay hints | next |
| M10 — intrinsic catalog + request-side parse cache | next |
| M11 — README / editor setup, CI, configuration, workspace folders | next |
| M12 — editor extras: selectionRange, callHierarchy, codeLens | next |
| M13 — pull diagnostics (backlog) | next |
| M14 — type/go-to + type hierarchy (backlog) | next |
| M15 — document links + completion resolve + polish (backlog) | next |

## 2. What exists (condensed)

Module map — only new/modified modules are called out in §5; this is the
stable shape:

- `src/lexer.{h,cpp}` — tokenizer over the full FB surface (byte offsets,
  continuation-aware logical lines). `Token`, `TokenKind`, `Lexer`.
- `src/parser.{h,cpp}` — `parseDocument(src) -> ParseResult`
  (`roots`, `diagnostics`, `blockRanges`, `lang`); decl extraction, block
  matching, dialect detection, doc comments.
- `src/language.{h,cpp}` — reserved-word catalog, block-closer facts, wiki doc
  URLs, dialect detection helpers, `isSuffixChar`.
- `src/symbols.h` — shared model: `Symbol`, `SymbolKind`, `Diagnostic`,
  `ParseResult`, `SourceRange` (byte offsets), `toLowerChars`.
- `src/resolve.{h,cpp}` — same-file resolution: `resolveAt`, `occurrencesOf`,
  `visibleSymbols`, `innermostScope`, `parentOf`.
- `src/index.{h,cpp}` — `WorkspaceIndex`: per-workspace symbol index with a
  per-source-file JSON disk cache, background scan + debounced flusher
  threads, immutable `IndexedFile` entries + snapshot reads. Cache layout:
  each indexed `.bas`/`.bi` gets its own `<sha256Hex(normalized-path)>.json`
  inside a per-workspace subdir `<sha256Hex(normalized-root)>`, itself under
  the platform index dir (`~/.local/state/freebasiclsp/index` on Linux,
  `%LOCALAPPDATA%\freebasiclsp\index` on Windows,
  `~/Library/Application Support/freebasiclsp/index` on macOS; SHA-256 via the
  vendored `hash_sha256` submodule, pinned `ad118c6`). Helpers:
  `sha256Hex`, `normalizePath`, `workspaceKey`, `defaultCacheDir`,
  `cacheFileFor`, `statFile`.
- `src/utf16.{h,cpp}` — byte ↔ UTF-16 position conversion (session boundary).
- `src/session.{h,cpp}` — `FreeBasicServer` registers every handler, owns
  `WorkingFiles` + `WorkspaceIndex`, re-parses the buffer, pushes diagnostics.
- `src/main.cpp` — stdio entry; `LanguageSession` + exit condition.

Implemented LSP methods: `initialize`/`shutdown`/`exit`, `didOpen`/`didChange`/
`didSave`/`didClose`, `publishDiagnostics`, `documentSymbol`, `hover` (symbols +
keyword wiki links), `foldingRange`, `definition`, `references`,
`documentHighlight`, `completion` (keywords + `END`-block snippets + in-scope
symbols), `signatureHelp`, `workspace/symbol`.

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

1. `definition`/`references`/`highlight`/`completion` resolve **only within
   the open file** (`resolve.cpp` is single-`ParseResult`); the M4 index holds
   per-file symbol trees but nothing cross-file is wired.
2. `prepareRename` / `rename` not implemented; `renameProvider` not advertised.
3. `#include` / `#include once` directives are lexed but not resolved: no
   include edges, no missing-file diagnostics, no header symbol visibility.
4. No semantic tokens, no inlay hints (LspCpp bundles the types; unused).
5. Session re-parses the whole buffer on every request (`documentSymbol`,
   hover, folding, def/refs/highlight, completion all call `parseDocument`);
   `resolve.cpp` re-lexes on every call (`lexAll` per `resolveAt`).
6. No `initialized` handler / dynamic capability registration, and
   `workspace/didChangeWatchedFiles` unhandled: the index scans only at
   `initialize`, external `.bi` edits go unnoticed until restart, and there is
   no server→client `registerCapability` path (needed once M11 settings can
   change the watcher set). `workspace/didChangeWorkspaceFolders` is likewise
   unhandled (single-root assumption). (The M5-era staleness wart — unsaved
   buffers persisted to disk — is fixed.)
7. No README, editor-setup docs, CI matrix, `didChangeConfiguration`, or
   built-in intrinsic-function completion catalog.
8. Feasible 3.17 features are unimplemented and unadvertised: `selectionRange`,
   `callHierarchy`, `codeLens` (M12), and pull diagnostics (M13). None is
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
  a `moduleScope` flag), `persisted` flag.
- **Inverted projections** under the index mutex, maintained incrementally by
  `upsert`/`remove`: `byKey_` (lowercase key → sites across files) and
  `outInc_` (file → direct includes), plus `transitiveIncludes(file)` with a
  cycle guard.
- **Disk cache v3** (bump from the M4-revamped per-file v2 layout; discarded
  and rebuilt — warm-start only, acceptable).
- **Buffer isolation:** open-buffer entries are `persisted=false` — served to
  live queries but never written by the flusher and never trusted by scan's
  mtime/size cache-hit. Fixes the §4.6 staleness wart.
- Files: `symbols.h`, `resolve.{h,cpp}` (shared `analyze` + occurrence sweep,
  legacy ParseResult wrappers internally analyze-backed), `index.{h,cpp}`,
  `session.cpp` (open-buffer upserts go through `analyze`, `persisted=false`),
  `resolve_checks` + `index_checks` cases. **Delivery deviation:** `parser.cpp`
  untouched (include extraction uses the public `preprocessorWord()` seam);
  `Storage`/`Shared` tagging deferred to M7 (`moduleScope` = "is a file root").
- Acceptance: `byKey_`/`outInc_` round-trip through the cache; transitive
  closure correct on diamond + cycle (`a.bi`↔`b.bi`); non-persisted entries
  never reach disk and never shadow scan hits; all 7 suites green.
- Risk: occurrence-vector memory for large workspaces (mitigate: sites only,
  no payload text; FB files are tiny). Residual: request-side re-analyze per
  call remains until the M10 parse cache.

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

- Include search policy: target resolved relative to the including file's dir,
  then workspace-root fallback (documented limitation; fbc `-i` dirs later via
  §M11 settings).
- `#include`/`#include once` edges are alive end-to-end; an unresolved include
  literal publishes an `include-not-found` `Error`.
- `workspace/didChangeWatchedFiles` registered (`**/*.{bas,bi}`) →
  debounced `index_->scan(true)` so external `.bi` edits converge.
- Files: `session.{h,cpp}`, new `src/includes.{h,cpp}` (search policy),
  `session_integration`, `index_checks`.
- Acceptance: opening `.bas` with a missing `.bi` publishes the diagnostic;
  touching a `.bi` on disk + watcher notification makes `workspace/symbol`
  return the new symbol without a restart; `#include once` not duplicated in
  the closure.
- Risk: LspCpp watched-file registration semantics — verify against vendored
  headers before coding.

### M7 — Cross-file definition / references / highlight / completion

All four consumer features share one new primitive in `resolve.cpp`:
`resolveAcross(parse, src, off)` — local scopes first (shadowing wins), then
module-scope  of each file in `transitiveIncludes`, then `byKey_` workspace
fallback for names that resolve nowhere locally.

- `definition` returns the target file's URI + range (offset conversion per
  target buffer). `references`/`highlight` use `byKey_` filtered by the
  closure, def/usage sites joined back to real ranges. `completion` merges
  visible module-scope names from the closure (deduped, shadow-aware).
- Open files resolve from `WorkingFile`; closed files from index snapshots
  (mtime re-validated).
- Files: `resolve.{h,cpp}`, `session.{h,cpp}`, `resolve_checks`,
  `session_integration` (two-file frames).
- Acceptance: def at a call site in `main.bas` lands in `lib.bi`; a local dim
  shadowing a header global still resolves locally; a procedure-local name in
  a `.bi` never resolves from `.bas`; references enumerate only closure files.

### M8 — `prepareRename` + `rename` (workspace)

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

### M9 — Semantic tokens + inlay hints

Independent UX wins; LspCpp types confirmed present (`td_semanticTokens_full`,
`td_inlayHint`).

- **Semantic tokens** (`src/semantic_tokens.{h,cpp}`): legend + full (and
  delta) from the cached token stream — `TokenKind` → `lsSemanticTokenType`
  (keyword, string, number, comment/preprocessor, operator/symbol); identifier
  classification via the symbol tree (known decl → kind) with a plain-variable
  fallback. Ship exact lexer kinds first, refine classifiers later.
- **Inlay hints** (`src/inlay_hints.{h,cpp}`), small scope: "expected closer"
  hints at block openers (`END SUB`, `NEXT`, `WEND`…) from `blockRanges` +
  `language.cpp` closer facts; optional inferred `AS type` on `dim` without a
  declared type.
- Files: new modules + `session.cpp` (2 handlers + capabilities) +
  `session_integration`.
- Acceptance: a fixture yields correct keyword/operator/string token spans
  with UTF-16 (non-ASCII) offsets; block opener offers its closer hint.
- Risk: token-type string spellings must match the 3.17 legend exactly;
  delta-encoding correctness (mitigate: full first, delta second).

### M10 — Intrinsic catalog + request-side parse cache

- **Intrinsic catalog** in `language.cpp` (pattern: the `keywordDocsUrl`
  per-word table): ~200 intrinsics (`Left$`, `Mid`, `Print`, `Val`, `CInt`,
  `Space$`, …) as `{ name, kind, signature, wikiSuffix }`; completion merges
  with keyword items, hover + `signatureHelp` consume it. Name-collision
  handling (`Left`/`Left$`) and statement-vs-expression position filtering.
- **Parse cache** in `session`: cache `ParseResult` + token vector per open
  document keyed by content (WorkingFiles version); invalidate on
  `didChange`. Removes the §4.5 repeated-full-parse across 8 handlers and the
  `lexAll`-per-`resolveAt` tax.
- Files: `language.{h,cpp}`, `session.{h,cpp}`, tests (`language_checks`,
  `session_integration`).
- Acceptance: known-intrinsic completion item carries the right signature +
  wiki link; two sequential requests on an unchanged buffer served from the
  same cached parse (identical result, no reparse observable).

### M11 — README / editor setup, CI, configuration, workspace folders

- `README.md`: build/test, capability table, position-encoding note, per-editor
  wiring (`docs/editors/` — neovim builtin LSP, minimal vscode client,
  emacs `lsp-mode`).
- `.github/workflows/ci.yml`: **scaffolded** — build + `ctest` on a
  Linux/macOS/Windows matrix (`checkout --recurse-submodules`); not enabled
  until the repo is pushed. Expect to fix Windows path handling in
  `index.cpp` defaults and any MSVC/LspCpp issues once it runs.
- `workspace/didChangeConfiguration` + `Settings{ includePaths, cacheDirOverride,
  diagnosticsOn, semanticTokensOn, inlayHintsOn }`; index honors `includePaths`
  on rescan. Few keys, fixed defaults, forward-compatible unknown-key ignore.
  Config-driven watcher changes ride M5.5's `client/registerCapability` path
  (unregister old globs, register new).
- Workspace folders: handle `workspace/didChangeWorkspaceFolders` — added
  folders get their own `WorkspaceIndex` (keyed by normalized root), removed
  ones close/scan-drop; single-root behavior stays the default. Server-side
  settings apply per active folder.
- Files: README, `.github/`, `src/settings.{h,cpp}`, `session.cpp`,
  `index.{h,cpp}`, tests.
- Acceptance: CI green on all three OSes; a `didChangeConfiguration` with a new
  include path makes a previously-missing `#include` resolve; adding a folder
  to the workspace makes its symbols answer `workspace/symbol`.

### M12 — Editor extras: selectionRange, callHierarchy, codeLens

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

### M13 — Pull diagnostics (backlog)

`textDocument/diagnostic` + `workspace/diagnostic` + `workspace/diagnostic/refresh`
(3.17) as a client-negotiated alternative to pushed `publishDiagnostics`
(LspCpp types in `protocol_3_18.h`). Sets `DiagnosticOptions.interFileDependencies = true`
— driven by the M6 include-edge graph — so pull diagnostics can surface
`relatedDocument` reports. Push stays the default; only worth building if a
target editor prefers pull.

### M14 — Type/go-to + type hierarchy (backlog)

`typeDefinition`, `implementation`, and typeHierarchy need inheritance facts
(`Type ... : base`, `Interface`, `Extends`) the parser does not emit yet. Land
the parser edges first, then reuse `byKey` + closure — otherwise identical in
shape and plumbing to M7.

### M15 — Document links + completion resolve + polish (backlog)

`documentLink` over keyword/wiki URLs (hover already carries them), optional
`completionItem/resolve` once the M10 catalog makes items heavy, advertised
`willSave`, `window/logMessage` + `$/progress`/`workDoneProgress` for long
scans, and small telemetry. Individually tiny; bundle as one polish drop.

## 6. Not doing (soon)

- **Formatting** — no community formatter standard for FreeBASIC; high effort,
  low payback.
- **Code actions** — thin while diagnostics are syntax-level only; revisit
  once M6 adds include diagnostics (quick-fix candidates then: "insert missing
  `#include`, `END` block closer").
- **QB / fblite / deprecated dialects** — current behavior (best-effort `fb`
  parse + `lang-mode` Information diagnostic) degrades gracefully; full dialect
  semantics is niche.
- **Debugger / DAP** — out of scope for a language server.
- **Recorded non-starters** (never scheduled): `moniker`, `linkedEditingRange`,
  `documentColor`/`colorPresentation`, the deprecated `declaration` alias —
  exercises for editors we do not target.
- **Scheduled but deferred** (each lives in §5 as a backlog milestone, M13–M15):
  pull diagnostics, type/go-to + type hierarchy, document links + completion
  resolve + protocol polish.

## 7. Cross-cutting engineering notes

- **Concurrency (implemented as-is):** LspCpp handler pool runs requests
  concurrently; the index is snapshot-based and mutex-guarded, responses build
  lock-free. New M5.5–M15 handlers must follow the same snapshot discipline
  (shared_ptr copies only).
- **Per-milestone acceptance:** `cmake --build` + `ctest` green, milestone
  deliverable complete, commit on `main`, push only on request.