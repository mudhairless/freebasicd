# FreeBASIC LSP Server — Implementation Plan

## 1. State summary

Repository `main`, clean working tree, `ctest` 7/7 green. LspCpp (vendored,
pinned `19150d12`) supplies framing/JSON-RPC/typed 3.17 messages; the language
layer is LSP-agnostic and byte-offset based. Full language reference (keyword
catalog, block closers verified against fbc 1.10.2, dialect rules) lives in
`AGENTS.md`; this plan covers roadmap, architecture, and the remaining work.

| Milestone | Status |
|-----------|--------|
| M1 — LspCpp bring-up (sync, capabilities, diagnostics push) | done |
| M2 — Lexer + parser language layer, dialects, fbc corpus | done |
| M3 — documentSymbol, hover, folding, definition, references, highlight, completion, signatureHelp | done |
| M4 — persistent workspace symbol index + `workspace/symbol` | done |
| M5 — workspace spine: occurrence projection + include graph | next |
| M6 — include resolution + watched files + missing-include diagnostics | next |
| M7 — cross-file definition / references / highlight / completion | next |
| M8 — `prepareRename` + `rename` (workspace) | next |
| M9 — semantic tokens + inlay hints | next |
| M10 — intrinsic catalog + request-side parse cache | next |
| M11 — README / editor setup, CI matrix, configuration | next |

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
  validated JSON disk cache, background scan + debounced flusher threads,
  immutable `IndexedFile` entries + snapshot reads. `normalizePath`,
  `workspaceKey`, `defaultCacheDir`, `statFile`.
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

The full reference lives in AGENTS.md. These are the rules the forward plan
engineers around:

- **Module model.** A `.bas` file is one program; `.bi` files are shared
  headers. Cross-file visibility is **module-scope only** (top-level `dim`,
  `const`, `type`, `sub`/`function` facts) and exists **only through the
  `#include closure`** of a document. Procedure-local names never cross a file
  boundary; a header never sees the `.bas` that included it.
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
6. Hidden staleness wart: an open buffer parses to `IndexedFile` with **disk**
   `mtime`/`size`, so `flushNow` can persist unsaved-buffer symbols; and
   `workspace/didChangeWatchedFiles` is unhandled (index scans only at
   `initialize`), so external `.bi` edits go unnoticed until restart.
7. No README, editor-setup docs, CI matrix, `didChangeConfiguration`, or
   built-in intrinsic-function completion catalog.

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
- **Disk cache v2** (v1 discarded and rebuilt — warm-start only, acceptable).
- **Buffer isolation:** open-buffer entries are `persisted=false` — served to
  live queries but never written by the flusher and never trusted by scan's
  mtime/size cache-hit. Fixes the §4.6 staleness wart.
- Files: `symbols.h`, `parser.cpp`, `resolve.cpp` (extract `analyze`),
  `index.{h,cpp}`, `index_checks` + new corpus cases.
- Acceptance: `byKey_`/`outInc_` round-trip through the cache; transitive
  closure correct on diamond + cycle (`a.bi`↔`b.bi`); non-persisted entries
  never reach disk and never shadow scan hits; all 7 suites green.
- Risk: occurrence-vector memory for large workspaces (mitigate: sites only,
  no payload text; FB files are tiny).

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

### M11 — README / editor setup, CI, configuration

- `README.md`: build/test, capability table, position-encoding note, per-editor
  wiring (`docs/editors/` — neovim builtin LSP, minimal vscode client,
  emacs `lsp-mode`).
- `.github/workflows/ci.yml`: build + `ctest` on a Linux/macOS/Windows matrix
  (`checkout --recurse-submodules`); expect to fix Windows path handling in
  `index.cpp` defaults once it runs.
- `workspace/didChangeConfiguration` + `Settings{ includePaths, cacheDirOverride,
  diagnosticsOn, semanticTokensOn, inlayHintsOn }`; index honors `includePaths`
  on rescan. Few keys, fixed defaults, forward-compatible unknown-key ignore.
- Files: README, `.github/`, `src/settings.{h,cpp}`, `session.cpp`,
  `index.{h,cpp}`, tests.
- Acceptance: CI green on all three OSes; a `didChangeConfiguration` with a new
  include path makes a previously-missing `#include` resolve.

## 6. Not doing (soon)

- **Formatting** — no community formatter standard for FreeBASIC; high effort,
  low payback.
- **Code actions** — thin while diagnostics are syntax-level only; revisit
  once M6 adds include diagnostics.
- **QB / fblite / deprecated dialects** — current behavior (best-effort `fb`
  parse + `lang-mode` Information diagnostic) degrades gracefully; full dialect
  semantics is niche.
- **Debugger / DAP** — out of scope for a language server.

## 7. Cross-cutting engineering notes

- **Concurrency (implemented as-is):** LspCpp handler pool runs requests
  concurrently; the index is snapshot-based and mutex-guarded, responses build
  lock-free. New M5–M9 handlers must follow the same snapshot discipline
  (shared_ptr copies only).
- **Per-milestone acceptance:** `cmake --build` + `ctest` green, milestone
  deliverable complete, commit on `main`, push only on request.