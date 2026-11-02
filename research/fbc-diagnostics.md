# fbc-derived diagnostic candidates

> Research notes, October 2026: the ranked candidate tiers behind the
> fbc-parity diagnostics milestones (M23, M24), with the implementation
> changelog of the steps that have landed at the top. Findings were
> probed against fbc 1.10.2. Originally kept out of tree as
> `FBLSP_FBCDIAG.md`.


**Probe chunk 6 — the ranking deliverable.** Inputs: `diagnostic-tooling.md`
(the research log alongside this file), chunks 1–5, plus `FreeBASIC.md` §11–§13. This is planning only; nothing here is
implemented. The name "chunk 6" is FUTURE.md's own §6 ("Ranking — fold 1–5 into
a ranked candidate list: message, phase, whether our lexer/parser/resolve can
detect it today, and what it would cost"). It is **not**
`~/FBLSP_CHUNK6_PROMPT.md`, a generated prompt for the keyword gaps that was
implemented in `17e96e0` / `aa4c5e4`.

The point of this document is to let a future implementer pick a diagnostic,
know its fbc number and message text, know which of our layers can produce it,
and know exactly which file/function to touch — without re-reading the 2,225-line
research log or re-probing the compiler.

---

## Implementation changelog

Progress on the waves below, newest first. This section lives here (outside the
project repo) so the repo references only its own files; the repo-side durable
record is `PLAN.md` (still to do) and `CHANGELOG.md` (landed).

### Step 1 — fbc catalog as data + code identity (done)

- Landed the §6.1 foundation. `tools/extract_fbc_catalog.py` reads `error.bas`
  at the pinned tag and writes the checked-in snapshot `tools/fbc_catalog.tsv`
  (328 errors, 49 warnings; warning 37 `AMBIGIOUSLENSIZEOF` keeps fbc's own
  empty `@""`, pinned by the test rather than "fixed"). `tools/fbc_catalog.cpp`
  is the shared emitter, `tools/gen_fbc_catalog.cpp` plus the `fbc-catalog`
  target write the generated `src/fbc_diagnostics.inc`, and
  `src/fbc_diagnostics.{h,cpp}` is the only reader: `fbcMessage()` (number →
  name/text/level), `fbcMessageCount()`, `fbcCode()`, `fbcMessageDocsUrl()`.
- `tests/fbc_diagnostics_checks.cpp` byte-diffs the committed include against a
  fresh emission through the same emitter (a negative test confirms it fails on
  drift) and pins the identity a fbc user recognizes: `fbcCode(Error, 42) ==
  "fbc error: 42"`, error 42 `VARIABLENOTDECLARED`, error 14
  `EXPECTEDIDENTIFIER`, warning 12 level 1, warning 40 level 0 (off by
  default), warning 49 `UPCASTDISCARDSINITIALIZER`. `ctest` 19/19, new files
  clang-format clean.
- The catalog is imported into the repo, not read from the compiler tree, so no
  test depends on the external fbc source (answer #6's rule applied to the
  catalog). The archive in §0 still carries the research scripts; the repo now
  owns the canonical extractor.
- Deferred to Step 2: the session-side link. When the first diagnostic is
  emitted with a `fbc `-prefixed code, the session sets
  `codeDescription.href = fbcMessageDocsUrl()` for it (the
  `CompilerErrMsg` wiki page).

### Step 2a — block-closer family onto fbc numbers + session link (done)

- Split the block-structure diagnostics onto fbc's numbers using the Step 1
  catalog, and landed the Step 1 deferred session link. `src/language.cpp`
  owns the mapping (`fbcExpectedCloserError`, `fbcCloserWithoutOpenerError`):
  - expected closer: `13 EXPECTEDNEXT` (for), `19 EXPECTEDENDTYPE`
    (type/union), `29 EXPECTEDLOOP` (do), `30 EXPECTEDWEND` (while),
    `32 EXPECTEDENDIF`, `35 EXPECTEDENDSELECT`, `45 EXPECTEDENDASM`,
    `60 EXPECTEDENDWITH`, `74 EXPECTEDENDENUM`, `95 EXPECTEDENDSCOPE`,
    `121 EXPECTEDENDNAMESPACE`, `124 EXPECTEDENDEXTERN`, and `125`–`130` for
    the procedure family. Every spelling was re-probed with the pinned
    `/usr/bin/fbc` before it was wired in.
  - closer without opener: `106`–`115` (`loop`/`next`/`wend`/`end with`/
    `end if`/`end select`/`end sub|function|ctor|dtor|operator|property`/
    `end scope`/`end namespace`/`end extern`), `116`/`117`/`118`
    (`elseif`/`else`/`case`), and `33 ILLEGALEND` for `end type`/`end union`/
    `end enum`/`end asm` (probed: fbc calls a bare `end type` an illegal
    `END`, not a closer-without-opener) plus `end for`/`end while`.
- `Diagnostic` grew an `int fbcError` field. The coarse `code`
  (`unterminated-block`, `stray-closer`) stays the quick-fix and corpus
  routing key; `toLsDiagnostic` turns `fbcError` into the wire
  `code = fbcCode(...)` and `codeDescription.href = fbcMessageDocsUrl()`.
  This is the design that keeps one `Insert 'END SUB'` fix row covering the
  whole procedure family — a fix keyed on the reported string would need one
  row per block kind. Preprocessor blocks stay unmapped (fbc's catalog has
  neither `#if` nor `#macro`).
- Tests: `parser_checks` pins the mapping per kind (and the preprocessor
  zero); `session_code_actions_checks` pins the wire code and the href; the
  existing wire assertions moved to the numbers they now see. `ctest` 19/19,
  format clean.

### Step 2b — §5 false-positive sweep (done)

- Landed the gate before any *new* fbc-coded check. Re-running the pinned suite
  (compile every file the parser flags with `/usr/bin/fbc -lang fb -c`, keep
  the ones it accepts) puts the **fb-dialect error false positives at 0, was
  100 files**; the 33 files that still report error-severity diagnostics all
  fail fbc. The sketch's 11 causes were in fact ~two dozen shapes
  (`function = expr` result assignment, a self-typed `Declare` parameter,
  `virtual`/`const` prefixes, `static as`/bitfield members, `/'…'/` block
  comments, a bare `extern`, statement-position `type<T>(…)`, `_` + trailing
  comment, one-line `if…end if`, bare `endif`, a suffixed `constructor()` call
  in a body, `dim T.x`, and more). That open-endedness is why 2b was decoupled
  from 2a; the coupling itself held — no fbc-coded check landed before this.
- The dedupe set is built from storage-class knowledge instead of "the first
  word is the definition": `extern`+`dim`, alias+alias, `const`+`const`, a
  reopened `namespace`, the one active `#if` branch, and
  `type<T>`/`constructor(` in expression position stop being duplicates while
  the real duplicate (`dim a` twice) still warns. `src/symbols.h` carries the
  two flags that buy it (`declOnly`, `aliasType`).
- **A suffix on a reserved word is now a warning with a fix.** The lexer keeps
  the token the bare keyword and records the skipped char's byte offset in
  `Token.suffixBeg`; the parser's `advance()` (the only token consumer) emits
  `keyword-suffix` on that byte, suppressed inside `#if`/`#macro` bodies; the
  `removeKeywordSuffix` provider deletes exactly it as a `CodeAction`. The
  merged combined-assign is the trap: `AND%=` is one token whose raw slice is
  `"and%="` because the skipped `%` sits between the base and the `=`, so the
  message is built from the source slice, never the token text, and
  `isCombinedAssignKeyword` strips the suffix before comparing. Identifier
  suffixes (`foo$`) fold in and never warn; `#` is never a keyword suffix
  (PRINT#1). Warning-only files left (both fbc warning 44, both true
  positives): `warnings/suffix-fb.bas` (496), `quirk/keyword-suffix.bas` (1).
  The transport is still our own `keyword-suffix` code — mapping it to fbc's
  number 44 is M23's remaining warning work.
- Tests: `parser_checks` gained `TestStep2bParsingFixes` (each cleared shape
  inline in the test, the real duplicate as control) and
  `TestKeywordSuffixWarning`; `lexer_checks` the block-comment / `_`+comment /
  `_`-vs-`#` rules; `code_actions_checks` the two suffix-fix cases;
  `session_code_actions_checks` the publish → codeAction → re-parse wire path.
  `ctest` 19/19, format clean.
- Cleanup: test and source comments no longer cite an external project tree —
  the shapes they described are inline in the tests, so a future reader can
  open what they name (the repo's rule that provenance stays in its own files).

### Step 2c — map the pre-existing diagnostics onto fbc's numbers (done, 2026-10-10)

- The remaining Tier 0 codes — the ones the doc's §3 table already assigns —
  now carry fbc's number on the wire, every one re-probed against
  `/usr/bin/fbc` 1.10.2 before wiring:
  - `keyword-suffix` → **warning 44** SUFFIXIGNORED, `unterminated-string` →
    **warning 12** NOCLOSINGQUOTE. These forced the warning-kind plumbing:
    `Diagnostic` gained `FbcMessageKind fbcKind` (default Error) next to
    `fbcError`, and `toLsDiagnostic` renders `fbc warning: N` instead of
    hardcoding the Error catalog. A field, not a derivation from `severity` —
    the §4 Tier 6 gate may render a mapped warning as a Hint, and the
    duplicate case below needs the split.
  - `duplicate-definition` → **error 4** DUPDEFINITION. Severity stays Warning
    (a re-declaration is ordinary mid-edit; pinned in session_core_checks), so
    the wire is `fbc error: 4` at `severity: 2`. The doc's §6.3 severity rule
    (fbc error → LSP Error) is deliberately not applied here — recorded
    FreeBASIC.md §12.25.
  - `include-not-found` → **error 23** FILENOTFOUND (set in
    `unresolvedIncludeDiagnostics`, code_actions.cpp; the fix still keys on
    the routing `code`).
  - `invalid-member-name` splits by trigger: **14** EXPECTEDIDENTIFIER (a
    field fbc refuses outright), **238** KEYWORDFIELDSNOTALLOWEDINCLASSES (a
    conditional field in a TYPE that holds member functions), **3** for an
    enum member (fbc anchors its report on the name — probed:
    `error 3: Expected End-of-Line, found 'next'`).
- **`bad-continuation` was probed a false positive and is deliberately not
  mapped.** §3 suggested `3 EXPECTEDEOL`, but the trigger shape does not
  match: fbc *accepts* `print 1 _ 2` (`_` is a joiner anywhere; `print 1 _` +
  newline + `print 2` also compiles, joined), while fbc's `error 3` belongs to
  a continuation-joined *statement* (`dim a as integer _` + newline +
  `print 1`), which this parser joins silently. Mapping 3 onto our trigger
  would ship fbc's number on a diagnostic fbc would not raise. Recorded
  FreeBASIC.md §12.26; deciding whether to kill the diagnostic is future work.
- Wire tests moved from routing codes to fbc codes (`session_core`,
  `session_settings`, `session_code_actions`, `session_pull_diagnostics`);
  `parser_checks` pins each number and the warning kind; `code_actions_checks`
  pins `include-not-found`'s `fbcError == 23`. `ctest` 19/19, format clean.

### Step 3a — the `-w` warning level as a config key (done, 2026-10-10)

- **The model is fbc's own threshold**, read straight from
  `compiler/src/compiler/error.bas`: `errReportWarnEx` gates with
  `if level < env.clopt.warninglevel then exit sub`, and `error.bi` names
  `FB_WARNINGMSGS_LOWEST_LEVEL = 0`, `DEFAULT_LEVEL = 1`,
  `HIGHEST_LEVEL = 3`. So a warning shows iff its catalog `level >=
  warningLevel`; the catalog's own `level` column is the only copy of that
  number, read at delivery rather than duplicated into each `Diagnostic`.
- **The vocabulary is fbc's `-w` option names** (`OPT_W` in `fbc.bas`,
  ~2461-2520), matched case-sensitively like the compiler's own option parser,
  one name per value: `all`/`constness`/`funcptr` → 0, `pedantic`/`param`/
  `escape`/`next`/`suffix`/`upcast` → 1, `none` → 4. `4` is `-w none` (the top
  catalog level is 3). Absent, unknown, wrong case, or a non-string value →
  default `4`. The strings are English only, never translated (like fbc's).
- **Two deliberate divergences from fbc, both recorded FreeBASIC.md §12.27.**
  (1) The default is `none`, not fbc's driver default of level 1: the server
  reports no warnings until a root opts in. (2) `-w error` is **not** accepted
  — a mapped warning is the language's own warning and stays `Warning` on the
  wire, and a problem this server diagnoses as an error already renders as
  one, so `warningLevel = "error"` is an unknown string → `none`. (The first
  user draft had `-w error` upgrade severity; the user dropped the option.)
- **The gate lives at delivery, not in the parser.** `session.cpp`'s
  `DiagnosticsGate`/`gateFor`/`gatedDiagnostic` drop a mapped warning whose
  level misses the threshold as the payload is built; the parser still records
  every diagnostic (it is a fact about the bytes). The gate is a `Settings`
  field (`warningLevel`, with `operator==`), so `didChangeConfiguration`
  re-reads it like the rest and a flip re-publishes the open buffers
  (`warningGateChanged` joins `flippedOn`/`includeChanged`).
- **The gate folds into the M14 `resultId`** alongside `diagnosticsOn`: a
  flip drops or re-adds warnings, and a client caching by id must be told the
  report changed. Because the parse is deterministic, the fold makes an
  invalid level *return* the id to its `none` value — the pull test pins that.
- **`-w error` in the route:** unrelated to the `error` severity of fbc
  *errors*. Warning 44 / 12 keep `severity: 2` at every accepted level.
- Tests: `settings_checks` (vocabulary + every fallback), `session_settings_checks`
  (push none→all→error(invalid)→bogus), `session_pull_diagnostics_checks`
  (resultId fold + return-to-none), `session_code_actions_checks` (the suffix
  quick-fix sandbox now sets `warningLevel = "all"`, since the fixed warning is
  off by default). `ctest` 19/19, format clean.
- **Defect on the way, the recorded class.** The new push test's `didOpen`
  frame was missing a closing brace, so the JSON never parsed, no publish
  arrived, and the negative assertion passed *vacuously*. Same lesson as the
  `R"(","value":"")"` truncations, in a test written after it was recorded.
- Next warning work: `26`, `32`, `33`, `25`, `46`, `15` now inherit the gate
  for free; Tier 1 new checks (starting `62 ILLEGALINSIDESELECT`) are the other
  axis.

### Step 3b — `suppress`: a routing-key suppression list (done, 2026-10-10)

- **What it is.** `freebasicd.toml`'s `suppress = ["bad-continuation", ...]`
  drops a diagnostic by its own identifier — `Diagnostic::code`, the stable
  routing key quick fixes register on, the key the wire code leads with
  (`keyword-suffix, fbc warning: 44`; see Step 3c). Same delivery
  seam as the `-w` gate: the parser still records the diagnostic, the payload
  obeys the config.
- **Value is a set.** Entries are ASCII-lowercased and the list is sorted and
  deduped at parse time (`settings.cpp`), so `["B","a"]` and `["a","b"]` are
  the same value — they compare equal via `operator==`, answer the same M14
  `resultId`, and a case-only edit is idempotent (no re-publish). This keeps
  the `resultId` fold order- and case-independent for free, rather than
  requiring a commutative hash.
- **Both delivery paths funnel through the gate.** The push path built parse
  diagnostics with `convertDiagnostics` then appended the index's include edges
  with `appendIncludeDiagnostics` — the latter called `toLsDiagnostic` directly
  and so bypassed any gate. It now takes the `DiagnosticsGate`, so
  `include-not-found` (an index answer, not a parse answer) is suppressed by
  the same list as the parser's codes; the push and pull paths share
  `documentDiagnostics`.
- **Fold + reconcile.** The list joins `diagnosticsOn` and `warningLevel` in
  `diagnosticsResultId`, and `onDidChangeConfiguration`'s `gateChanged`
  (`fresh.suppress != old.suppress`) joins the push re-publish set / pull
  refresh.
- **Test design caught a wrong probe.** The push test keeps a survivor
  diagnostic in every publish, so a negative assertion cannot pass vacuously.
  That survivor immediately exposed the first draft's claim that a dangling
  `sub foo()` is `fbc error: 126` — it is **125** (EXPECTEDENDSUB); 126 is
  EXPECTEDENDFUNCTION for a `function`. Exactly the "a negative assertion needs
  a positive one beside it" lesson from Step 3a, this time earning its keep.
- **Future work (user's ask, recorded not built):** a per-line source comment
  `' ignore: <key>` and `' ignore-next-line: <key>` (clang-tidy style; FB
  comment forms are `'` and `REM`). It is the same gate with a line/range
  predicate — the comment is a language fact, so it comes out of the parse,
  and the comment-derived suppressions must fold into `resultId` too. **With
  it, a whitelist counterpart in the config** (an allow-list that keeps only
  the named routing keys when non-empty) — the inverse gate. Recorded in
  PLAN.md M23 and README.
- Tests: `settings_checks` (parse/case/order/set equality, non-array,
  mixed types), `session_settings_checks` (push default→key→two keys, include
  edge covered, survivor kept), `session_pull_diagnostics_checks` (resultId
  fold + stability). `ctest` 19/19, format clean.

### Step 3c — the wire code always carries our identifier (done, 2026-10-10)

- **The rule (user's).** If a diagnostic has an fbc number, its wire code is
  `<our routing key>, fbc error: <n>` (or `fbc warning: <n>`); if it has none,
  the code is the routing key alone. So a key-only code (`bad-continuation`)
  and a composed one (`include-not-found, fbc error: 23`) are both addressable
  by our identifier.
- **Why it is not cosmetic.** `suppress` (and the planned whitelist and comment
  forms) key on `Diagnostic::code`. Before this a mapped diagnostic's wire code
  was *only* `fbc error: N`, so our identifier never reached the client and the
  config key named a string the client could not see — the two had to be
  reconciled. Composing makes the code the client sees and the key the config
  uses the same string.
- **Implementation.** `toLsDiagnostic` (`session.cpp`) builds `d.code` then
  appends `", " + fbcCode(kind, n)` when `fbcError != 0`; `fbcCode` is
  unchanged and is still just the fbc half. No `resultId` change — the code
  format is a payload detail, not part of the identity.
- **Test convention.** A general test asserts our identifier (`"code":"<key>"`,
  a prefix match because the key leads); only a mapping test looks for the fbc
  number. `session_core_checks` pins the composed
  `"code":"duplicate-definition, fbc error: 4"` as that one mapping test, and
  `fbc_diagnostics_checks` keeps the `fbcCode` unit-level numbers.
- **Gotcha.** A prefix assertion must drop the closing quote
  (`"code":"keyword-suffix`) or it will never match the composed string; a
  `PollRequest` needle written with the closing quote stalls on budget. This
  bit the first draft of the `-w all` pull needle.

### Step 3d — `suppress` accepts the composed wire code (done, 2026-10-10)

- **The problem Step 3c created.** Once the wire code *is* the composed string,
  it is the string a client shows and a user copies into `suppress` — and a
  routing key never contains a comma, so the pasted
  `include-not-found, fbc error: 23` matched no key and suppressed nothing,
  silently. Composing the identity and then demanding the un-composed one is
  the inconsistency to remove.
- **The rule (user's).** At load time, reduce a `suppress` entry to the text
  before the first comma (trailing blanks trimmed), so the paste works. Do not
  rewrite the config file; instead **log a warning** naming the entry and the
  key it should be, and let the user clean it up. `parseSettings` stays pure:
  it fills a `std::vector<SuppressNote>{entry, key}` out-param, and
  `settingsForDirLogged` (`session.cpp`) turns each note into one stderr line
  through `trf` (new msgid, pot regenerated).
- **Still set semantics.** The suffix is dropped from the *value*, so the
  composed form is `operator==`-equal to the bare key: a suffix-only edit is
  idempotent, moves no `resultId`, and publishes nothing.
- Tests: `settings_checks` (`TestSuppressWireSuffix`: reduced keys, notes,
  whitespace trim, bare-vs-composed equality); `session_settings_checks`
  (`TestDidChangeConfigurationSuppressWireCode`: composed entry over the wire,
  the named diagnostic dropped, a survivor kept, the file on disk untouched).
  The warning itself is a log line the suite does not capture; `i18n_checks`
  covers the message's presence in the pot.

---

## 0. Provenance, tooling, confidence

- **Pin: fbc `1.10.2` = commit `8e1023e3`**, the release matching `/usr/bin/fbc`
  (`FreeBASIC Compiler - Version 1.10.2 (2026-09-27), built for linux-x86_64`).
- **The local clone at `~/Projects/freebasic/compiler` is on `1.10.3-451`
  (`5714d10ad`), not 1.10.2.** Its catalog has *four more warnings* and different
  wording in places. Never read the catalog from the clone; always re-extract
  the tag read-only:
  ```
  cd ~/Projects/freebasic/compiler
  git archive 1.10.2 | tar -x -C /tmp/opencode/fbc-1.10.2
  ```
  The clone is never checked out or modified by this.
- **Catalog is data**: `src/compiler/error.bas` holds two brace-delimited arrays
  whose 1-based position **is** the printed message number (`FB_ERRMSG_OK` = 0;
  `error number = enum position − 1`). `error.bas` is byte-identical across
  1.10.0–1.10.3, so message *identity* (number, name, text, `-w` level) needs no
  per-version data.
- **Regeneration scripts used for this document are archived** (with a README,
  a one-shot `run.sh`, and snapshots of the generated data) at
  **`~/Projects/fbcdiag-tools.tar.gz`**; extract it anywhere and run
  `./run.sh` after exporting the pinned tag. The scripts themselves:
  - `fbcdiag_extract.py` → `fbcdiag.tsv` (kind, number, name, text, level,
    reference count, referencing files, phases).
  - `fbcdiag_classify.py` → per-message implementation bucket.
  - `fbcdiag_appendix.py` → Appendix A/B/C below.
  Each reads `FBC_SRC` (default `/tmp/opencode/fbc-1.10.2/src/compiler`) and
  excludes the catalog's own `/'FB_ERRMSG_X '/` comment spelling when counting
  references. A copy also lives in `/tmp/opencode/fbcdiag-tools/`, which will be
  lost; the archive is the durable location.
- **Confidence**: the catalog tables (Appendices A/B) are mechanical and exact.
  The *bucket* per message is derived from which `src/compiler/*.bas` files
  reference the name (filename → phase, per the phase map in §2), so it is a
  good line up our lexer/parser/resolve split but not a per-site judgement.
  **Per-message trigger conditions were not re-derived here** — chunk 2's
  `by_message.md` is gone. Regenerate it before writing a check that depends on
  an exact guard.
- **Known discrepancy in the research log (not in project docs)**: chunk 5
  (line ~2062) says "the 53-number warning catalog" and cites warning 50. The
  pinned 1.10.2 catalog has **49 warnings** (Appendix B), and its highest number
  is 49 `UPCASTDISCARDSINITIALIZER`. The 53 came from the later clone tree.
  `FreeBASIC.md` §13 and `PLAN.md` both correctly say 49. Treat any "53" in
  FUTURE.md chunk 5 as an error; the warning-coverage ratios there (21 covered /
  32 uncovered of 53) should be recomputed against 49.

---

## 1. What the research established (inputs 1–5)

### 1.1 The catalog (chunk 1)

- **328 errors, 49 warnings.** Numbering hypothesis holds 25/25 against codes
  fbc prints. `error 0` is `FB_ERRMSG_OK` (not a message).
- The warning tuple's **first field is the `-w` level**, not the number.
- **Level semantics are a floor, and the research's first reading was
  backwards.** `errReportWarnEx`: `if level < warninglevel then exit sub`, with
  `FB_WARNINGMSGS_DEFAULT_LEVEL = 1`. So *higher* `-w N` shows *fewer*:
  `-w all`=0, `-w none`=4, `-w 3`=3 only. **Level 0 is off by default**; 45 of
  49 are on by default. Level histogram: 0→4, 1→29, 2→12, 3→4. The four
  level-0 warnings are `40 CONSTQUALIFIERDISCARDED`, `41 RETURNTYPEMISMATCH`,
  `42 CALLINGCONVMISMATCH`, `43 ARGCNTMISMATCH`.
- **Text truncation**: `error 146`/`147` store only `"Only valid in -lang"` /
  `"Default types or suffixes are only valid in -lang"`; the dialect list is
  appended at report time by `getNotAllowedMsg`. A tool that quotes the stored
  text must not expect the suffix.
- **One alias**: `error 14` has two constants, `FB_ERRMSG_EXPECTEDVAR` and
  `FB_ERRMSG_EXPECTEDIDENTIFIER` (equal). Key on the number or accept both
  names, never on the constant alone.
- **Raise-frequency** (sites): 17 `SYNTAXERROR` 106, 24 `INVALIDDATATYPES` 86,
  14 `EXPECTEDIDENTIFIER` 61, 9 `EXPECTEDEXPRESSION` 59, 4 `DUPDEFINITION` 45,
  20 `TYPEMISMATCH` 43, 7 `EXPECTEDRPRNT` 42. A quarter of call sites are
  name/type checks — the half needing resolve.
- 87 of 328 error texts mention `Expected`/`Illegal`/`Syntax`; 21 name a block
  closer.

### 1.2 Trigger conditions, the phase map, and the eight shapes (chunk 2)

**The eight shapes that carry a message number to a reporter** — only shape 1 is
visible to a naive grep:

| # | shape | example | sites |
|---|---|---|---|
| 1 | constant at the call site | `errReport( FB_ERRMSG_EXPECTEDDO )` | 1071 |
| 2 | declared **default argument** | 1-arg `errReportNotAllowed(...)` defaults to `FB_ERRMSG_ONLYVALIDINLANG` (146) | 26 |
| 3 | pass-through wrapper sub | `hParamError(...)` fans out to 11 messages | 15 |
| 4 | `byref` out-param set in callee | `symbCalcProcMatch` sets 5 `OVERRIDE*` | 8 |
| 5 | `#macro` body | 8 report macros | many |
| 6 | local var + `select case`, reported once | `cCompStmtGetTOS` picks the "X without Y" message | 22 |
| 7 | sub's **return value** is the message | `astNewCONV` returns a message | 6 |
| 8 | **default parameter value** carries the message | `parser-decl-proc-params.bas:99` | 5 |

**Phase map** (research's canonical site counts; our layer on the right):

| phase (files) | sites | our layer |
|---|---|---|
| lex (`lex`, `parser-quirk-string`, `parser-inlineasm`, `parser-quirk-data`) | 57 | lexer |
| pp (`pp*`, `parser-comment`) | 78 | preprocessor |
| parser/decl (`parser-decl-*`) | 221 | parser |
| parser/proc (`parser-proc`, `parser-proccall*`) | 160 | parser |
| parser/expr (`parser-expr-*`, `parser-assignment`) | 141 | parser |
| parser/compound (`parser-compound*`) | 91 | parser |
| parser other (`parser-quirk-*`, `parser-identifier`, …) | 193 | parser |
| ast (`ast-*`) | 54 | resolve |
| symb (`symb-*`, `reg`) | 19 | resolve |
| intrinsics (`rtl*`: REDIM/ERASE/OPEN/SEEK lowering) | 28 | parser (statement-specific) |
| ir/emit (`ir-*`, `emit_x86`) | 10 | **not reachable** |
| driver (`fbc`, `fb`) | 31 | **not reachable** |

The research summary "806 of 1086 sites are plain syntax" means the five
`parser/*` rows (221+160+141+91+193 = 806). Lex (57), pp (78) and intrinsics
(28) are equally parser-reachable but were counted separately; ast+symb = 73 is
the semantic set; ir+driver = 41 is out of reach. The row sums are ~1083, not
1086 — a small internal inconsistency in the research log, harmless at this
granularity.

**Two tables worth lifting**, kept in the compiler's own order
(`parser-compound.bas` `cCompStmtCheck` / `cCompStmtGetTOS`):
expected-closer (`IF→32`, `SELECT→35`, `SCOPE→95`, `WITH→60`, `NAMESPACE→121`,
`EXTERN→124`, `DO→29`, `WHILE→30`, `FOR→13`, proc keywords → `125`–`130`); and
closer-without-opener (`DO→106`, `EXTERN→…`, `FOR→107`, `IF→110`, `SCOPE→…`,
`SELECT→111`, `WHILE→108`, `WITH→…`, `NAMESPACE→…`).

**Error recovery** (do not copy verbatim; explains why fbc looks sparse):
report-and-continue (`hMatchLPRNT`, `hMatchCOMMA`); report-and-skip-to-sync
(`hMatchRPRNT`); report-and-abandon-statement (`hExitError`); fabricate-and-continue
(`hMatchFileNumberExpression`). And `errReportEx` prints **at most one error per
statement** — a delivery policy, not a language rule.

### 1.3 Dialect gating (chunk 3)

**The catalog has no dialect-specific message.** The dialect is text appended at
report time to five messages (146/147/148/149/150), and only when a construct's
**feature bit** is off. Gating is a property of a *construct*, not a message.

- 4 dialects (`FB_LANG_FB` default, `_FB_DEPRECATED`, `_FB_FBLITE`, `QB`);
  **23 feature bits** in `FB_LANG_OPT`; `langTb()` (`fb.bas:31-105`) is the
  authoritative dialect→bit table; one test macro `fbLangOptIsSet` with 71 call
  sites (22 of 23 bits read).
- Groups: all four `QUIRKFUNC`; fb-only `OPEROVL`, `CLASS`, `AUTOVAR`,
  `SINGERRLINE`; fb+deprecated `SCOPE`; fb+deprecated+fblite `MT`, `NAMESPC`,
  `EXTERN`, `FUNCOVL`, `INITIALIZER`; all-but-fb `CALL`, `LET`, `PERIODS`,
  `NUMLABEL`, `IMPLICIT`, `DEFTYPE`, `SUFFIX`, `METACMD`, `OPTION`, `ONERROR`;
  all-but-fb-and-deprecated `GOSUB`. Dead bits: `ALWAYSOVL` (set by nobody, read
  twice) and `QUIRKFUNC` (set by all, read nowhere).
- **Three layers, three outcomes** for the same source: (a) bit+keyword present
  → `error 146/147/150`; (b) keyword absent in that dialect (`kwdTb`
  `KWD_OPTION_NO_QB`, 90 rows; `QB_ONLY`, 1 row `SCREEN`; `STRSUFFIX`, 12) → a
  **syntax error** (3/10); (c) silent semantic difference → **nothing**.
- **32 gated report sites**, of which 27 take the default and report 146; 5 name
  a different message (147×?, 148, 149, 150). **`148 SUFFIXONLYVALIDINLANG` and
  `149 IMPLICITVARSONLYVALIDINLANG` are referenced but unreachable** (an uncalled
  macro `hErrSuffix()`; a `= FALSE` *inside* a macro argument). This is a
  distinct, third kind of dead entry and is already recorded in
  `FreeBASIC.md` §13.
- **`DEFTYPE` and `INITIALIZER` are disjoint**: no dialect accepts `Dim a = 5`;
  `Dim a` (no `As`) is illegal only in `fb` (147); `Dim a As T = 5` is illegal
  in `qb` (146, "found '='"). `var x = v` is 150 in deprecated/fblite, and a
  layer-(b) syntax error in `qb` (`var` is `NO_QB`).
- **`#lang` / `$lang`**: module-level only (inside a proc = `error 61`);
  `-forcelang` → `warning 30`; per module; `$lang` is the one metacommand exempt
  from the `METACMD` gate. **Two disagreeing `#lang` directives hang fbc
  1.10.2** (infinite restart loop), so never model "apply every `#lang`".
- **`SINGERRLINE`** is a delivery format (line vs line+column), not a check.
- **`class … end class` is `error 3` in all four dialects** — FB has no class
  construct; `CLASS` gates member procedures. Message text still says "CLASS" 12
  times, which a quoting tool must reproduce verbatim.

### 1.4 Keyword/member tables (chunk 4)

The 12 missing `kReserved` words and 7 member-name rows are **already
implemented** (`17e96e0`, `aa4c5e4`); the tables now carry the post-gap counts
(`FreeBASIC.md` §7 says "all 365" reserved words; `language.h` member tables say
16 never / 119 conditional). Findings that remain relevant to diagnostics:

- `error 4` means "fbc has a symbol with this name", not "is a keyword" — it
  covers `kwdTb`, rtl builtins, `true`/`false`, `object`, and 75,427 distinct
  `declare` names in `inc/`. Absence from fbc is equally uninformative.
- `kBlockOpeners` (19) / `kCloserOnly` (3) have **no gap** against
  `parser-compound.bas`. But `constructor`/`destructor`/`property`/`operator`
  (and `sub`/`function`) open a block **only in implementation form**; a
  `declare` inside a `TYPE` takes no closer. A word-keyed `BlockRow` cannot
  express this — it is a field the row needs, not a new row.
- `kIntrinsics` (247) is not a keyword table: 34 of its rows are not reserved,
  and `drawstring` is a synthesized key for the two-token `Draw String`
  statement, not a fbc symbol.
- `kwdTb` has 247 rows but 246 distinct names (`screen` has two rows).
- `FB_TOKEN` has 264 ids; the 17 with no keyword row are exactly the non-word
  tokens.

### 1.5 External corpus: fbc's test suite (chunk 5)

2,493 `.bas` across 43 dirs; tags `COMPILE_ONLY_FAIL` 1,261, untagged 801,
`COMPILE_ONLY_OK` 355, `COMPILE_AND_RUN_OK` 45, `MULTI_MODULE_TEST` 25,
`FBCUNIT_COMPATIBLE` 4, `COMPILE_AND_RUN_FAIL` 1. `syntax/` (24), `warnings/`
(69–72), `unix-tty/` (16) and `fbcunit/` (28) are in **no** dirlist, so their
tags are advisory.

- **Set A = 50 tagged-OK files our parser rejects**, all 100% block-structure
  (`unterminated-block` / `stray-closer` / `closer-mismatch`). With 202 untagged
  rejects that also pass fbc, **252 false positives** total. (234 of 252 are a
  block-structure combination; the rest are `bad-continuation` 13 and
  `invalid-member-name` 3.)
- **Set B = 1,037 in-suite FAIL-tagged files we accept with zero errors**,
  split by fbc's own codes: **815 semantic-only** (resolve opportunity), **137
  with ≥1 pure-grammar code** (parser-reachable misses), **83 type-dependent
  "Expected X"** (borderline: fbc's parser consults type info), 4 with no fbc
  code.
- Set B grammar histogram: 17 Syntax error ×37, 67 Too many expressions ×19,
  14 Expected identifier ×16, **62 Statement between SELECT and first CASE ×15**,
  7 Expected ')' ×15, 3 Expected EOL ×14, 9 Expected expression ×8, 10 Expected
  '=' ×4, 68 Expected explicit result type ×4, 289 Expected sub ×2.
- `tests/warnings/`: 67 reproducer `.bas` + 2 `.bi` + `test.sh`; `test.sh` runs
  `fbc -maxerr inf -w constness -target <t> -r -m <file>` and `sed`s
  `file.bas(N) warning L(L):` to a tabbed line, committing 5×67 goldens. 58
  interleave `#print` markers, so the golden order **is** the trigger condition.
  21 of the 49 numbers have a reproducer; 19 of 67 goldens are target-dependent.
- Our parser rejects 9 of the `warnings/` files, all from §5 causes below.
- **`fbc` prints diagnostics to stdout, not stderr**, and its include flag is
  `-i <path>`, not `-I`.

---

## 2. Phase map → implementation buckets

Every one of the 328 errors is assigned to exactly one bucket from which
`src/compiler/*.bas` files reference it (Appendix C lists the codes per bucket):

| bucket | count | meaning | our layer | detect today? |
|---|---|---|---|---|
| `parser` | 263 | ≥1 reference in `lex`/`pp`/`parser-*`/`rtl*` | lexer + parser | mostly yes, construct-by-construct |
| `resolve` | 29 | only `ast-*`/`symb-*` references | resolve | no type model yet |
| `dialect` | 5 | the `*ONLYVALIDINLANG` family (146–150) | parser (gate) + config | gate, not a check |
| `dead` | 18 | no reference, or referenced-but-unreachable (148/149) | — | never |
| `unreachable` | 13 | only `ir-*`/`emit_*`/driver references | — | never |
| `warning` | 49 | the warning catalog | mixed | partly |

The `resolve` count of 29 **messages** is smaller than the research's "73
semantic sites" (many messages are referenced from several sites); the `parser`
bucket of 263 dominates because most catalog entries are syntax/declaration
checks. This is the central cost fact: **~85% of the catalog is parser-reachable
from the machinery we already have.**

---

## 3. Current project diagnostic inventory (Tier 0)

Our codes today are our own strings, not fbc numbers. Everything is emitted by
`parser.cpp` into `ParseResult.diagnostics` (`symbols.h:165`), plus
`include-not-found` from `code_actions.cpp` (shared with the publish path) and
the `lang-mode` note. No diagnostic comes from `lexer.cpp`, `resolve.cpp`, or
`index.cpp`.

| our code | sev | emitted at | fbc number(s) it would map to |
|---|---|---|---|
| `unterminated-block` | Error | parser.cpp:137, 455 | the expected-closer family 13/29/30/32/35/60/95/121/124/125–130 |
| `stray-closer` | Error | parser.cpp:825, 836, 1677, 1746, 1794, 1801 | closer-without-opener 106/107/108/110/111/… |
| `closer-mismatch` | Error | parser.cpp:1718 | `33 ILLEGALEND` / mismatched closer |
| `invalid-end` | Error | parser.cpp:1654 | `33 ILLEGALEND` |
| `duplicate-definition` | Warning | parser.cpp:368 | `4 DUPDEFINITION` |
| `invalid-member-name` | Error | parser.cpp:419, 496, 655, 1564 | `238` (conditional field) / `14` (never field) |
| `bad-continuation` | Error | parser.cpp:573 | `3 EXPECTEDEOL` (continuation form) |
| `meta-directive` | Information | parser.cpp:112 | `error 146` for non-`$lang` metacommands in fb |
| `lang-mode` | Information | parser.cpp:182 | `146/147/148/149/150` + the `#lang` rules |
| `doc-slash` | Information | parser.cpp:100 | (our convention, no fbc equivalent) |
| `include-not-found` | Error | code_actions.cpp:341 | `23 FILENOTFOUND` |

Four quick fixes are registered (`code_actions.cpp:301`):
`unterminated-block`→insert closer, `invalid-member-name`→rename,
`include-not-found`→retarget; adding a fix is one function + one row, never a
`session.cpp` edit (M12).

**Implication for §6**: the first structural job is not a new check but a
**code-identity migration** — split these coarse codes into fbc-numbered
messages where the research has mapped them (esp. the closer family), so a user
who knows `error 42` (or a fix that keys on a code) sees fbc's number.

---

## 4. Ranked candidate diagnostics

Ranking = user value first, then cost. "Cost" is rough engineering size for
*this codebase*, not fbc table size. Source touchpoints are in §9.

### Tier 1 — parser-only, cheap, high value

These need nothing but our lexer/parser and a place to report.

1. **Block-closer family, split into fbc numbers.** We already detect the three
   shapes but collapse them into generic codes. Emit the *specific* message:
   expected closer (`13/29/30/32/35/60/95/121/124/125–130`), closer without
   opener (`106/107/108/110/111`), `33 ILLEGALEND`. Data is already in
   `language.cpp` (`kBlockOpeners`/`kCloserOnly`, `expectedCloserAt`,
   `blockForOpener`); the work is keying the report on the opener token. This is
   also the family behind **all 50 set-A false positives** — but note those are
   *bugs in the opener detection*, not missing diagnostics; see §5.
2. **`62 ILLEGALINSIDESELECT`** — a statement (especially `dim`/`type`/`var`)
   between `SELECT CASE` and the first `CASE`. 15 set-B files; purely syntactic;
   confirmed miss. Cheap and it explains a whole `compound/` cluster.
3. **`34 EXPECTEDCASE`, `118 CASEWITHOUTSELECT`, `111 ENDSELECTWITHOUTSELECT`,
   `246 EXPECTEDSELECT`, `242 ILLEGALOUTSIDESELSTMT`, `250 NOENCLOSEDSELSTMT`,
   `323 INVALIDCASERANGE`** — the rest of the `select` family. All token-keyed.
4. **`EXPECTEDNEXT 13` / `NEXTWITHOUTFOR 107` / `FORNEXTVARIABLEMISMATCH 283` /
   warning `26 NEXTVARMEANINGLESS`** and the `DO/LOOP` `WHILE/WEND` pair
   (`29/30/106/108`). Partly covered by Tier 1.1; the variable-mismatch and the
   meaningless-`NEXT`-variable warning are new and cheap.
5. **`EXIT`/`CONTINUE` outside a legal block**: `251 INVALIDEXITSTMT`,
   `252 INVALIDCONTINUESTMT`. The parser already tracks the block stack.
6. **The punctuator family** (`SYNTAXERROR 17` 105 sites, `EXPECTEDIDENTIFIER
   14`, `EXPECTEDEXPRESSION 9`, `EXPECTEDRPRNT 7`, `EXPECTEDLPRNT 6`,
   `EXPECTEDEOL 3`, `EXPECTEDEQ 10`, `EXPECTEDCOMMA 16`, `EXPECTEDRBRACKET 66`,
   `EXPECTEDGT 309`). We already report some of these indirectly; the win is a
   single `expected(token)` helper in `parser.cpp` that names the missing token.
   **Caution**: fbc prints one error per statement; we should dedupe and never
   cascade a missing token into a flood.
7. **`11 EXPECTEDCONST`** for a `const` with no expression
   (`const/const-decl-missing-expr.bas`, an fbc-fail we accept) — purely
   syntactic.
8. **Trailing-keyword / malformed declaration tails**: `17 SYNTAXERROR`
   (`sub f( ) byref`), `68 EXPECTEDRESTYPE` (`functions/return-byref-bad-syntax`),
   `17` for `sub UDT.ctor( ) constructor`. Token-level.
9. **`67 TOOMANYEXPRESSIONS`** (set B ×19) and **`289 EXPECTEDSUB`** — parse-level
   shape checks.
10. **`127/129/130/…` mismatched proc closers** (`end sub` where
    `end constructor` belongs) — data already in the `BlockRow` change (§1.4).

### Tier 2 — statement-context / placement (parser, needs a recorded context)

1. **`61 ILLEGALINSIDEASUB`** — a construct legal only at module level used
   inside a procedure (e.g. `#lang`, `Constructor` on the wrong line, `Type`
   inside a body depending on form). 7 reference sites.
2. **`96 ILLEGALINSIDEASCOPE`** and **`44 ILLEGALOUTSIDECOMP`** — e.g.
   `Case`/`Exit Select`/`Continue` outside their compound. The parser has the
   block stack; the report needs to name the enclosing construct.
3. **`105 BRANCHCROSSINGDYNDATADEF`** / warning **`14 BRANCHCROSSINGLOCALVAR`**
   — a `Goto`/branch crossing a local variable/array/UDT definition. Needs
   label + declaration knowledge, but not full types.

### Tier 3 — dialect gates (only meaningful once a non-`fb` dialect is modeled)

We parse only `fb`, and a `#lang`/`$lang` to another dialect today yields one
`lang-mode` note plus a best-effort `fb` parse. So most dialect work is
**deferred**, but the candidate list is:

1. **`146 ONLYVALIDINLANG`** for the 27 gated constructs (metacommands,
   `Scope`, `Namespace`, `Extern`, member proc in a `TYPE`, `Operator`,
   `Overload`, `Gosub`/`Return`, `On Error`, `Call`, `Let`, numeric label,
   `Option`, `LPrint`), keyed on the **construct's feature bit**, with the
   dialect list computed the way `getNotAllowedMsg` does.
2. **`147 DEFTYPEONLYVALIDINLANG`** — untyped `Dim`/param/result in `fb`.
3. **`150 AUTOVARONLYVALIDINLANG`** — `var x = v`; and `146` for
   `Dim x As T = 5` in `qb`.
4. **Our own, more useful than a port: the two-`#lang` disagreement.** fbc hangs;
   the honest diagnostic is "only the first `#lang` applies, and it must be at
   module level". This is a language-model diagnostic, not an fbc number.
5. **"builtin not available in this dialect"** — the `rtl*.bas` flags
   (`NOQB` 135, `QBONLY` 11, `FBONLY` 4, `NOFB` 3, `MT` 13). A resolve-layer
   product (needs the builtin table + the active dialect), high value because
   fbc is silent until the compile fails.
6. **148/149 are dead** — do not implement.

### Tier 4 — resolve-dependent (the large effort; ~29 messages + warnings)

This is where fbc's name/type checks live, and where set B's **815 semantic
files** point. Our `resolve.cpp` emits **no diagnostics today**; delivering any
of these requires a type/overload/const-attribute model layered on
`ResolvedSymbol`. Natural pair with M21/M22.

| fbc # | name | set-B cost cluster |
|---|---|---|
| `42` | `VARIABLENOTDECLARED` | use-before-declaration (M22) |
| `8` | `UNDEFINEDSYMBOL` | |
| `4` | `DUPDEFINITION` | ×51 (semantic variant) |
| `20` / `58` | `TYPEMISMATCH` / `PARAMTYPEMISMATCHAT` | ×24 / ×22 |
| `181` | `ILLEGALASSIGNMENT` | ×92 (largest) |
| `202` | `ILLEGALMEMBERACCESS` | ×77 |
| `24` | `INVALIDDATATYPES` | ×70 |
| `119` | `CONSTANTCANTBECHANGED` | ×65 |
| `99` / `98` | `NOMATCHINGPROC` / `AMBIGUOUSCALLTOPROC` | ×32 |
| `1` | `ARGCNTMISMATCH` | |
| `206` / `204` | `NOACCESSTODTOR` / `NOACCESSTODEFAULTCTOR` | ×28 / ×21 |
| `255` | `AMBIGUOUSSYMBOLACCESS` | |
| `225`–`231` | `OVERRIDE*` (5 messages) | |
| `71` | `INCOMPLETETYPE` | |
| `18` | `ELEMENTNOTDEFINED` | |
| `272` | `INVALIDREFERENCETOLOCAL` | |
| `54` / `28` / `63` / `73` / `120` | `EXPECTEDDYNAMICARRAY` / `EXPECTEDPOINTER` / `EXPECTEDARRAY` / `EXPECTEDINDEX` / `EXPECTEDPERIOD` | the 83 type-dependent set |
| warnings `1`–`4`, `5`, `10`, `40`–`43` | passing ptr/scalar, implicit conversion, pointer fields, const qualifier, return-type/callconv/argcount | function-pointer/type model |

**Ordering note**: the resolve layer cannot be the first wave — it is the
biggest build and it depends on a type model that does not exist. Start with
Tier 1 (cheap, and fixes the false-positive bugs in §5), then Tier 2, then
resolve.

### Tier 5 — intrinsics / statement-specific (`rtl*.bas`, 15 messages)

REDIM/ERASE/OPEN/SEEK/CLOSE etc. fire on ordinary source, so they belong to the
parser, but each needs statement-specific state: `REDIM` on an already
dimensioned array, `SEEK` on a non-file number, `CLOSE` of an unopened file.
Examples in the catalog: `56 ARRAYALREADYDIMENSIONED` (dead), plus the
`intrinsics` bucket codes in Appendix C. Worth a later dedicated pass; low
volume, low risk.

### Tier 6 — warnings (`-w` model, 49)

Warnings are a separate delivery problem because fbc gates them by `-w` level
and 45 are on by default. Recommendations:

1. **Map fbc level → LSP severity**: level ≥ 2 or a genuine hazard → `Warning`;
   stylistic/level-1 → `Hint`/`Information`. Do **not** reproduce fbc's
   floor semantics; an LSP shows configured warnings.
2. **Do not start with the type-heavy warnings.** Cheap, parser-reachable ones:
   `12 NOCLOSINGQUOTE`, `44 SUFFIXIGNORED` (already effectively known),
   `26 NEXTVARMEANINGLESS`, `32 IFFOUNDAFTERELSE`, `33 SHIFTEXCEEDSBITSINDATATYPE`,
   `25 CONVOVERFLOW` (constant folding), `46 CMDLINEIGNORED` (pp),
   `15 NOEXPLICITPARAMMODE`. Resolve/type needed: `1`–`5`, `10`, `13`, `23/24`,
   `40`–`43`, `49`.
3. **The `warnings/` suite is the trigger-condition corpus**: 21 numbers have a
   golden reproducer; the 32 uncovered ones can be generated from the message
   texts. Target-pin `linux-x86_64` because 19/67 goldens are target-dependent.
4. **Config question**: whether to expose fbc's level model (one key like
   `diagnostics.warningLevel`) or a single on/off. Any gate that changes the
   payload must be folded into the M14 `resultId` hash.

### Tier 7 — language-model diagnostics fbc does **not** have (high value, M21)

These come from the module model research and are *not* in fbc's catalog:

1. **"module-level executable code outside the main module"** — the fix is to
   wrap in `Sub name() Constructor`. Runs silently before `main`, order
   unspecified and unstable across link orders.
2. **"module global read from a module constructor"** — the §8 rule biting in a
   new place (fbc `error 42`); fix = add `Shared`. Quick fix via M12.
3. **"two `Public` constructors sharing a name across the closure"** — an fbc
   link error, reportable at edit time (uppercased comparison).
4. **`priority` range (101–65535)** and **`Constructor` on a `Declare` line** →
   pure syntax checks tied to M21's lexing of `Constructor`/`Destructor`.
5. **No diagnostic for constructor order** — fbc does not specify it; do not
   imply we know it.

### Out of reach (13 messages)

Appendix C's `unreachable` bucket: all `ir-*`/`emit_*` and driver-only messages
(backend ABI, `-mt`/`-lang` mixing detected at link time, command-line option
errors such as `81 INVALIDCMDLINEOPTION`, `93 MISSINGCMDOPTION`). An LSP never
sees the command line or the linker, so these stay out. Version/pin config
(§7) is the only part of the driver space we can honestly touch.

---

## 5. Fix the false positives before adding checks

Adding diagnostics on top of a parser that rejects valid FreeBASIC makes the
server worse, not better. The 252 false positives (chunk 5) are **13 root
causes**, each reproduced minimally. These are parser bugs to fix *alongside*
Tier 1, and the external corpus is the acceptance test.

| # | cause | parser area | note |
|---|---|---|---|
| 1 | `function = expr` / `operator = expr` result-assignment parsed as a phantom block opener | procedure-body statement classification | largest family; internal corpus has **0** occurrences |
| 2 | self-type param in a `declare` inside a `TYPE` pops the type | TYPE-body `declare` parsing (~parser.cpp:1500–1560) | kills most of `visibility/` |
| 3 | bare `extern <decl>` marginal array treated as `Extern` block opener | `extern`/compound opener decision | all 7 `dim/*` A-files |
| 4 | `virtual sub …` / `const sub …` implementation prefixes not accepted | procedure-prefix handling | |
| 5 | statement-position `type<T>(...)` parsed as a type declaration | statement dispatch vs declaration | |
| 6 | non-declaration statement inside a `TYPE` body aborts the type | TYPE body loop | causes cascading stray closers |
| 7 | `static as integer y` field prefix → bogus dup-def + invalid-member | field parsing + `kConditionalFieldNames` interaction | |
| 8 | bitfield field `a : 7 as ulong` unsupported | field parsing | |
| 9 | `/'…'/` multiline comments not lexed | `lexer.cpp` comment scan | leaks a false `meta-directive` and opens blocks |
| 10 | `_` continuation honored while skipping a false `#if` region | preprocessor skip in lexer/parser | |
| 11 | `_` followed by a trailing comment | continuation handling (parser.cpp:573) | fbc allows it |
| 12 | UTF-16 source (disk corpus only) | — | **out of scope**: the LSP never reads UTF-16; do not "fix" the lexer for it |

The internal `tests/corpus/` (52 files) has **zero** occurrences of every one of
these trigger shapes, which is why it stayed green. Pointing `corpus_checks` at
fbc's suite (red-on-purpose, behind a flag) is the coverage statement the corpus
has never had.

---

## 6. Cross-cutting implementation architecture

1. **Catalog as data (M21 step 6).** Add a generator under `tools/` that reads
   `error.bas` at the pinned tag and emits a checked-in C++/data table
   (number, name, text, warning level); a test fails when the checked-in table
   drifts from the generator. Extend it to warnings. The `Diagnostic.code` field
   is an LSP `int | string`; emit **fbc numbers** for messages we map, and our
   own strings for conventions fbc has no number for (`doc-slash`). This is the
   migration that makes §3's codes useful to a fbc reader.
2. **Message catalog / i18n.** Every new message is a translatable literal via
   `fblang::trf` / `trn` in `src/i18n.h`; FreeBASIC keywords must not appear
   uppercase inside a translatable literal (they enter as `%s`); after a new or
   changed message regenerate `po/freebasicd.pot` (`po-template` target) or
   `i18n_checks` fails. This is a recurring, easily-forgotten step.
3. **Severity mapping.** fbc `error N` → LSP `Error`; warning → `Warning` or
   `Information` per §4 Tier 6. Keep `doc-slash`/`lang-mode` at `Information`.
4. **Gating and the M14 resultId.** `diagnosticsOn` already gates the whole
   payload; any new gate (warning level, family toggles) must be folded into
   `diagnosticsFnv`/`documentDiagnostics` (session.cpp:441–470), because a gate
   change that alters the payload must change the id or a client caches stale
   diagnostics.
5. **Quick fixes (M12).** A new code with an obvious fix is one provider
   function + one row in `quickFixProviders()` (`code_actions.cpp:301`), never a
   `session.cpp` change. Incoming candidates: add `Shared` (M21), insert
   `Declare` (M22), wrap module `Constructor` (M21), add the explicit `As T`
   for `147`, the existing insert-closer/rename/retarget.
6. **Cascades and dedupe.** Do not copy fbc's one-error-per-statement rule, but
   do not flood either: a single missing closer should yield one diagnostic, not
   one per subsequent line. The existing `unterminated-block` handling is the
   model.
7. **Where diagnostics live.** Parser diagnostics go in
   `ParseResult.diagnostics` (`symbols.h:209`). Resolve diagnostics have no home
   yet: either a post-parse pass appending to the same vector, or a separate
   field that `documentDiagnostics` concatenates. Prefer the latter so the
   parse cache and the resolve pass stay separable (the index only parses).
8. **Tests.** Parser checks in `tests/parser_checks.cpp` /
   `tests/grammar_checks.cpp`; LSP delivery in
   `tests/session_pull_diagnostics_checks.cpp`; corpus in
   `tests/corpus_checks.cpp`. A new session feature file must be registered in
   **both** `CMakeLists.txt` and `tests/session_integration.cpp` — the second
   fails silently if missed (grep the driver for the runner name and check the
   `[ RUN ]` output).

---

## 7. Version-aware diagnostics (the requirement, constrained)

- `error.bas` is **byte-identical across 1.10.0 → 1.10.3**, so message identity
  needs no version data. **Version awareness belongs on behavior**, not the
  catalog: what is accepted and what fires.
- Shape: per-diagnostic `since`/`until` (default "all 1.10.x"); most carry
  none. Config keys `fbc.path` / `fbc.version` under `[fbc]` in
  `freebasicd.toml` (M21 second half).
- Three existing seams, no new architecture: `findFbcExecutableDir`
  (`src/index.cpp`, currently a once-only magic static that a config change must
  invalidate), `Settings` (`src/settings.h`), and `tests/corpus_checks.cpp`'s
  bare `std::system("fbc -version")` — resolve it through the same seam or the
  corpus tests a different compiler than the server.
- Compare the **dotted number only**, never the build date or platform triple.
  Open: precedence when a pinned version cannot be found; root-scoped ranges.

---

## 8. Discrepancies to keep in mind

- **Research-only**: FUTURE.md chunk 5's "53-number warning catalog" / warning
  50 are wrong for 1.10.2 (49 warnings, max 49). Project docs (`FreeBASIC.md`
  §13, `PLAN.md`) are correct.
- **Research-only**: the phase-map site counts sum to ~1083, not the "1086"
  quoted; the "806 of 1086" figure means the five `parser/*` rows only.
- **Verified in the project tree**: `FreeBASIC.md` §13 already records the 18
  dead numbers **and** 148/149 as referenced-but-unreachable, and §7 records the
  member-name tables at 16/119 words. The 12 `kReserved` words and 7
  member-name rows are implemented (`17e96e0`, `aa4c5e4`). No project-doc
  correction is needed for those.
- **Verify during implementation**: the exact per-message guard for whichever
  message you implement (regenerate chunk 2's `by_message.md`); the guard is not
  reproduced in this document.

---

## 9. Source map (where changes will land)

| file / symbol | role for diagnostics |
|---|---|
| `src/lexer.cpp` (`isReservedWord` decision ~:300; comment/continuation scan) | keyword-vs-identifier; `/'…'/` and `_` bugs (§5 causes 9–11) |
| `src/language.cpp` `kBlockOpeners` (:658), `kCloserOnly` (:681), `expectedCloserAt` (:1647), `blockForOpener` (:1706) | block-closer data and lookup; needs the implementation-form field |
| `src/language.cpp` `kReserved` (:212), `kNeverFieldNames` (:596), `kConditionalFieldNames` (:609), `isReservedWord` (:1378), `isNeverFieldName` (:1453) | member-name legality; already fixed |
| `src/language.cpp` `kIntrinsics` (:754), `intrinsicFor` (:1484), `kBuiltinTypes` (:642) | intrinsics phase; dialect builtin availability |
| `src/language.cpp` `keywordDocsUrl` (:1415) | hover/wiki for new messages |
| `src/parser.cpp` `addDiagnostic` (:306) and the report sites (:100–:182, :368, :419–:496, :573, :655, :825–:836, :1563–:1801) | where parser diagnostics are emitted; the expected-token/closer split lands here |
| `src/parser.cpp` TYPE-body loop (~:1500–1560) | §5 causes 2, 6, 7, 8 |
| `src/resolve.cpp` / `src/resolve.h` | new home for Tier-4 diagnostics (currently emits none) |
| `src/symbols.h` `Diagnostic` (:165), `ParseResult::diagnostics` (:209) | the value type; add a resolve-diagnostics field |
| `src/session.cpp` `documentDiagnostics` (:441), `publishDiagnostics` (:3834), `settingsForDocument` (:1630), `isLegalIdentifier` (~:744), completion (:2840, :2955) | delivery, gating, resultId; completion filters affected by keyword tables |
| `src/code_actions.cpp` `quickFixProviders` (:301) | one function + one row per new fix |
| `src/settings.{h,cpp}` | `diagnosticsOn` and any new gate / `[fbc]` keys |
| `src/i18n.{h,cpp}`, `po/freebasicd.pot` | every new message text |
| `tools/` | the `error.bas` catalog generator + staleness test (M21 step 6) |
| `FreeBASIC.md` §12/§13 | divergences and provenance, kept in sync |
| `PLAN.md` §4/§5, `CHANGELOG.md` | milestone bookkeeping |

---

## 10. Open questions for the user

1. **Starting tier.** Recommended: Tier 1 + §5 false-positive fixes in one wave
   (they share the block-structure code and the external corpus proves both).
   Confirm, or pick Tier 6 (warnings) first.
   Answer: Follow recommendation
2. **Code identity.** Emit raw fbc numbers (`code: 42`) or a namespaced string
   (`"fbc:42"`)? The LSP type allows both; mixing must be deliberate.
   Answer: For these fbc derived diagnostics we will match the fbc code, maybe using
   a string form of `fbc error: 42` is the most useful, we can also link directly to
   the wiki page that covers errors.
3. **Warnings policy.** Mirror fbc's `-w` level as a config key, or ship a
   single on/off (and which level)? Default matters for noise.
   Answer: Our default should match fbc and have a config option to up the level
4. **Dialect scope.** Defer all `*ONLYVALIDINLANG`/dialect-gate work until the
   project actually models a non-`fb` dialect, or emit best-effort gates now
   under `#lang`?
   Answer: defer
5. **Resolve layer.** Is Tier 4 gated on M21/M22, or does it warrant its own
   type-model milestone? Set B's 815 semantic files are the value argument.
   Answer: this should probably be it's own milestone
6. **Corpus gate.** Land the fbc-suite `corpus_checks` extension red-on-purpose
   behind an env flag now (so §5 fixes are measurable), or after?
   Answer: we should import appropriate corpus checks into our tests not rely on 
   the external compiler source being there

---

## Appendix A — fbc error catalog (328 messages)

Source: `error.bas` at tag `1.10.2` (`8e1023e3`). Column *Bucket* is
this analysis's implementation class (see §3); *Refs* is the number of
`FB_ERRMSG_*` references in `src/compiler/*.bas` (0 = fbc never prints it).

| # | Code name | Bucket | Refs | Message |
|---|-----------|--------|------|---------|
| 1 | `ARGCNTMISMATCH` | parser | 23 | Argument count mismatch |
| 2 | `EXPECTEDEOF` | dead | 0 | Expected End-of-File |
| 3 | `EXPECTEDEOL` | parser | 8 | Expected End-of-Line |
| 4 | `DUPDEFINITION` | parser | 44 | Duplicated definition |
| 5 | `EXPECTINGAS` | parser | 2 | Expected 'AS' |
| 6 | `EXPECTEDLPRNT` | parser | 15 | Expected '(' |
| 7 | `EXPECTEDRPRNT` | parser | 40 | Expected ')' |
| 8 | `UNDEFINEDSYMBOL` | parser | 7 | Undefined symbol |
| 9 | `EXPECTEDEXPRESSION` | parser | 58 | Expected expression |
| 10 | `EXPECTEDEQ` | parser | 8 | Expected '=' |
| 11 | `EXPECTEDCONST` | parser | 14 | Expected constant |
| 12 | `EXPECTEDTO` | parser | 1 | Expected 'TO' |
| 13 | `EXPECTEDNEXT` | parser | 1 | Expected 'NEXT' |
| 14 | `EXPECTEDIDENTIFIER` | parser | 60 | Expected identifier |
| 15 | `EXPECTEDMINUS` | parser | 4 | Expected '-' |
| 16 | `EXPECTEDCOMMA` | parser | 10 | Expected ',' |
| 17 | `SYNTAXERROR` | parser | 105 | Syntax error |
| 18 | `ELEMENTNOTDEFINED` | parser | 2 | Element not defined |
| 19 | `EXPECTEDENDTYPE` | parser | 2 | Expected 'END TYPE' or 'END UNION' |
| 20 | `TYPEMISMATCH` | parser | 40 | Type mismatch |
| 21 | `INTERNAL` | parser | 9 | Internal! |
| 22 | `PARAMTYPEMISMATCH` | parser | 4 | Parameter type mismatch |
| 23 | `FILENOTFOUND` | unreachable | 6 | File not found |
| 24 | `INVALIDDATATYPES` | parser | 85 | Invalid data types |
| 25 | `INVALIDCHARACTER` | parser | 1 | Invalid character |
| 26 | `FILEACCESSERROR` | unreachable | 4 | File access error |
| 27 | `RECLEVELTOODEEP` | parser | 14 | Recursion level too deep |
| 28 | `EXPECTEDPOINTER` | parser | 13 | Expected pointer |
| 29 | `EXPECTEDLOOP` | parser | 1 | Expected 'LOOP' |
| 30 | `EXPECTEDWEND` | parser | 1 | Expected 'WEND' |
| 31 | `EXPECTEDTHEN` | parser | 2 | Expected 'THEN' |
| 32 | `EXPECTEDENDIF` | parser | 2 | Expected 'END IF' |
| 33 | `ILLEGALEND` | parser | 1 | Illegal 'END' |
| 34 | `EXPECTEDCASE` | parser | 2 | Expected 'CASE' |
| 35 | `EXPECTEDENDSELECT` | parser | 2 | Expected 'END SELECT' |
| 36 | `WRONGDIMENSIONS` | parser | 5 | Wrong number of dimensions |
| 37 | `BOUNDSDIFFERFROMEXTERN` | parser | 1 | Array boundaries do not match the original EXTERN declaration |
| 38 | `INNERPROCNOTALLOWED` | dead | 0 | 'SUB' or 'FUNCTION' without 'END SUB' or 'END FUNCTION' |
| 39 | `EXPECTEDENDSUBORFUNCT` | dead | 0 | Expected 'END SUB' or 'END FUNCTION' |
| 40 | `RETURNTYPEMISMATCH` | parser | 2 | Return type here does not match DECLARE prototype |
| 41 | `CALLINGCONVMISMATCH` | parser | 2 | Calling convention does not match DECLARE prototype |
| 42 | `VARIABLENOTDECLARED` | parser | 1 | Variable not declared |
| 43 | `VARIABLEREQUIRED` | dead | 0 | Variable required |
| 44 | `ILLEGALOUTSIDECOMP` | parser | 4 | Illegal outside a compound statement |
| 45 | `EXPECTEDENDASM` | parser | 2 | Expected 'END ASM' |
| 46 | `PROCNOTDECLARED` | dead | 0 | Function not declared |
| 47 | `EXPECTEDSEMICOLON` | parser | 3 | Expected ';' |
| 48 | `UNDEFINEDLABEL` | resolve | 1 | Undefined label |
| 49 | `TOOMANYDIMENSIONS` | parser | 1 | Too many array dimensions |
| 50 | `ARRAYTOOBIG` | parser | 3 | Array too big |
| 51 | `UDTTOOBIG` | resolve | 1 | User Defined Type too big |
| 52 | `EXPECTEDSCALAR` | parser | 3 | Expected scalar counter |
| 53 | `ILLEGALOUTSIDEAPROC` | parser | 1 | Illegal outside a CONSTRUCTOR, DESTRUCTOR, FUNCTION, OPERATOR, PROPERTY or SUB block |
| 54 | `EXPECTEDDYNAMICARRAY` | parser | 3 | Expected var-len array |
| 55 | `CANNOTRETURNFIXLENFROMFUNCTS` | parser | 2 | Fixed-len strings cannot be returned from functions |
| 56 | `ARRAYALREADYDIMENSIONED` | dead | 0 | Array already dimensioned |
| 57 | `ILLEGALRESUMEERROR` | dead | 0 | Illegal without the -ex option |
| 58 | `PARAMTYPEMISMATCHAT` | parser | 15 | Type mismatch |
| 59 | `ILLEGALPARAMSPECAT` | parser | 1 | Illegal specification |
| 60 | `EXPECTEDENDWITH` | parser | 1 | Expected 'END WITH' |
| 61 | `ILLEGALINSIDEASUB` | parser | 7 | Illegal inside functions |
| 62 | `ILLEGALINSIDESELECT` | parser | 2 | Statement in between SELECT and first CASE |
| 63 | `EXPECTEDARRAY` | parser | 5 | Expected array |
| 64 | `EXPECTEDLBRACE` | parser | 1 | Expected '{' |
| 65 | `EXPECTEDRBRACE` | parser | 2 | Expected '}' |
| 66 | `EXPECTEDRBRACKET` | parser | 4 | Expected ']' |
| 67 | `TOOMANYEXPRESSIONS` | parser | 2 | Too many expressions |
| 68 | `EXPECTEDRESTYPE` | parser | 3 | Expected explicit result type |
| 69 | `RANGETOOLARGE` | parser | 2 | Range too large |
| 70 | `FORWARDREFNOTALLOWED` | dead | 0 | Forward references not allowed |
| 71 | `INCOMPLETETYPE` | parser | 11 | Incomplete type |
| 72 | `ARRAYNOTALLOCATED` | parser | 1 | Array not dimensioned |
| 73 | `EXPECTEDINDEX` | parser | 3 | Array access, index expected |
| 74 | `EXPECTEDENDENUM` | parser | 2 | Expected 'END ENUM' |
| 75 | `CANTINITDYNAMICARRAYS` | parser | 1 | Var-len arrays cannot be initialized |
| 76 | `DYNAMICARRAYWITHELLIPSIS` | parser | 1 | '...' ellipsis upper bound given for dynamic array (this is not supported) |
| 77 | `ARRAYFIELDWITHELLIPSIS` | parser | 1 | '...' ellipsis upper bound given for array field (this is not supported) |
| 78 | `INVALIDBITFIELD` | parser | 1 | Invalid bitfield |
| 79 | `TOOMANYPARAMS` | parser | 1 | Too many parameters |
| 80 | `MACROTEXTTOOLONG` | dead | 0 | Macro text too long |
| 81 | `INVALIDCMDOPTION` | unreachable | 1 | Invalid command-line option |
| 82 | `DOSWITHNONX86` | unreachable | 1 | Selected non-x86 CPU when compiling for DOS |
| 83 | `GENGASWITHNONX86` | unreachable | 1 | Selected -gen gas\|gas64 ASM backend is incompatible with CPU |
| 84 | `GENGASWITHOUTINTEL` | unreachable | 1 | -asm att used for -gen gas, but -gen gas only supports -asm intel |
| 85 | `PICNOTSUPPORTEDFOREXE` | unreachable | 1 | -pic used when making executable (only works when making a shared library) |
| 86 | `PICNOTSUPPORTEDFORTARGET` | unreachable | 1 | -pic used, but not supported by target system (only works for non-x86 Unixes) |
| 87 | `CANTINITDYNAMICSTRINGS` | parser | 1 | Var-len strings cannot be initialized |
| 88 | `RECURSIVEUDT` | parser | 1 | Recursive TYPE or UNION not allowed |
| 89 | `RECURSIVEMACRO` | parser | 1 | Recursive DEFINE not allowed |
| 90 | `CANTINCLUDEPERIODS` | parser | 10 | Identifier cannot include periods |
| 91 | `EXEMISSING` | unreachable | 1 | Executable not found |
| 92 | `ARRAYOUTOFBOUNDS` | parser | 1 | Array out-of-bounds |
| 93 | `MISSINGCMDOPTION` | dead | 0 | Missing command-line option for |
| 94 | `EXPECTEDANY` | parser | 1 | Expected 'ANY' |
| 95 | `EXPECTEDENDSCOPE` | parser | 1 | Expected 'END SCOPE' |
| 96 | `ILLEGALINSIDEASCOPE` | parser | 6 | Illegal inside a compound statement or scoped block |
| 97 | `CANTPASSUDTRESULTBYREF` | dead | 0 | UDT function results cannot be passed by reference |
| 98 | `AMBIGUOUSCALLTOPROC` | resolve | 3 | Ambiguous call to overloaded function |
| 99 | `NOMATCHINGPROC` | parser | 5 | No matching overloaded function |
| 100 | `DIVBYZERO` | resolve | 3 | Division by zero |
| 101 | `STACKUNDERFLOW` | parser | 1 | Cannot pop stack, underflow |
| 102 | `CANTINITDYNAMICFIELDS` | dead | 0 | UDT's containing var-len string fields cannot be initialized |
| 103 | `BRANCHTOBLOCKWITHLOCALVARS` | dead | 0 | Branching to scope block containing local variables |
| 104 | `BRANCHTARGETOUTSIDECURRPROC` | resolve | 2 | Branching to other functions or to module-level |
| 105 | `BRANCHCROSSINGDYNDATADEF` | resolve | 1 | Branch crossing local array, var-len string or object definition |
| 106 | `LOOPWITHOUTDO` | parser | 1 | LOOP without DO |
| 107 | `NEXTWITHOUTFOR` | parser | 1 | NEXT without FOR |
| 108 | `WENDWITHOUTWHILE` | parser | 1 | WEND without WHILE |
| 109 | `ENDWITHWITHOUTWITH` | parser | 1 | END WITH without WITH |
| 110 | `ENDIFWITHOUTIF` | parser | 1 | END IF without IF |
| 111 | `ENDSELECTWITHOUTSELECT` | parser | 1 | END SELECT without SELECT |
| 112 | `ENDSUBWITHOUTSUB` | parser | 1 | END SUB or FUNCTION without SUB or FUNCTION |
| 113 | `ENDSCOPEWITHOUTSCOPE` | parser | 1 | END SCOPE without SCOPE |
| 114 | `ENDNAMESPACEWITHOUTNAMESPACE` | parser | 1 | END NAMESPACE without NAMESPACE |
| 115 | `ENDEXTERNWITHOUTEXTERN` | parser | 1 | END EXTERN without EXTERN |
| 116 | `ELSEIFWITHOUTIF` | parser | 1 | ELSEIF without IF |
| 117 | `ELSEWITHOUTIF` | parser | 1 | ELSE without IF |
| 118 | `CASEWITHOUTSELECT` | parser | 1 | CASE without SELECT |
| 119 | `CONSTANTCANTBECHANGED` | parser | 15 | Cannot modify a constant |
| 120 | `EXPECTEDPERIOD` | parser | 4 | Expected period ('.') |
| 121 | `EXPECTEDENDNAMESPACE` | parser | 1 | Expected 'END NAMESPACE' |
| 122 | `ILLEGALINSIDEANAMESPC` | parser | 1 | Illegal inside a NAMESPACE block |
| 123 | `CANTREMOVENAMESPCSYMBOLS` | parser | 3 | Symbols defined inside namespaces cannot be removed |
| 124 | `EXPECTEDENDEXTERN` | parser | 1 | Expected 'END EXTERN' |
| 125 | `EXPECTEDENDSUB` | parser | 2 | Expected 'END SUB' |
| 126 | `EXPECTEDENDFUNCTION` | parser | 2 | Expected 'END FUNCTION' |
| 127 | `EXPECTEDENDCTOR` | parser | 2 | Expected 'END CONSTRUCTOR' |
| 128 | `EXPECTEDENDDTOR` | parser | 2 | Expected 'END DESTRUCTOR' |
| 129 | `EXPECTEDENDOPERATOR` | parser | 2 | Expected 'END OPERATOR' |
| 130 | `EXPECTEDENDPROPERTY` | parser | 2 | Expected 'END PROPERTY' |
| 131 | `DECLOUTSIDENAMESPC` | parser | 5 | Declaration outside the original namespace |
| 132 | `EXPECTEDENDCOMMENT` | parser | 1 | No end of multi-line comment, expected ""'/"" |
| 133 | `TOOMANYERRORS` | parser | 1 | Too many errors, exiting |
| 134 | `EXPECTEDMACRO` | parser | 3 | Expected 'ENDMACRO' |
| 135 | `CANNOTINITEXTERNORCOMMON` | parser | 1 | EXTERN or COMMON variables cannot be initialized |
| 136 | `DYNAMICEXTERNCANTHAVEBOUNDS` | parser | 1 | EXTERN or COMMON dynamic arrays cannot have initial bounds |
| 137 | `ATLEASTONEPARAMMUSTBEANUDT` | parser | 3 | At least one parameter must be a user-defined type |
| 138 | `PARAMORRESULTMUSTBEANUDT` | dead | 0 | Parameter or result must be a user-defined type |
| 139 | `SAMEPARAMETERTYPES` | dead | 0 | Both parameters can't be of the same type |
| 140 | `SAMEPARAMANDRESULTTYPES` | parser | 1 | Parameter and result can't be of the same type |
| 141 | `INVALIDRESULTTYPEFORTHISOP` | parser | 1 | Invalid result type for this operator |
| 142 | `PARAMTYPEINCOMPATIBLEWITHPARENT` | parser | 2 | Invalid parameter type, it must be the same as the parent TYPE/CLASS |
| 143 | `VARARGPARAMNOTALLOWED` | parser | 2 | Vararg parameters are not allowed in overloaded functions |
| 144 | `ILLEGALOUTSIDEANOPERATOR` | parser | 3 | Illegal outside an OPERATOR block |
| 145 | `PARAMCANTBEOPTIONAL` | parser | 2 | Parameter cannot be optional |
| 146 | `ONLYVALIDINLANG` | dialect | 0 | Only valid in -lang |
| 147 | `DEFTYPEONLYVALIDINLANG` | dialect | 3 | Default types or suffixes are only valid in -lang |
| 148 | `SUFFIXONLYVALIDINLANG` | dialect | 1 | Suffixes are only valid in -lang |
| 149 | `IMPLICITVARSONLYVALIDINLANG` | dialect | 1 | Implicit variables are only valid in -lang |
| 150 | `AUTOVARONLYVALIDINLANG` | dialect | 1 | Auto variables are only valid in -lang |
| 151 | `INVALIDARRAYINDEX` | parser | 2 | Invalid array index |
| 152 | `OPMUSTBEAMETHOD` | parser | 1 | Operator must be a member function |
| 153 | `OPCANNOTBEAMETHOD` | parser | 1 | Operator cannot be a member function |
| 154 | `METHODINANONUDT` | parser | 1 | Method declared in anonymous UDT |
| 155 | `CONSTINANONUDT` | parser | 1 | Constant declared in anonymous UDT |
| 156 | `STATICVARINANONUDT` | parser | 1 | Static variable declared in anonymous UDT |
| 157 | `EXPECTEDOPERATOR` | parser | 2 | Expected operator |
| 158 | `DECLOUTSIDECLASS` | parser | 5 | Declaration outside the original namespace or class |
| 159 | `DTORCANTCONTAINPARAMS` | parser | 1 | A destructor should not have any parameters |
| 160 | `EXPECTEDCLASSID` | parser | 1 | Expected class or UDT identifier |
| 161 | `VARLENSTRINGINUNION` | resolve | 1 | Var-len strings cannot be part of UNION's or nested TYPE's |
| 162 | `DYNAMICARRAYINUNION` | resolve | 1 | Dynamic arrays cannot be part of UNION's or nested TYPE's |
| 163 | `CTORINUNION` | parser | 2 | Fields with constructors cannot be part of UNION's or nested TYPE's |
| 164 | `DTORINUNION` | resolve | 1 | Fields with destructors cannot be part of UNION's or nested TYPE's |
| 165 | `ILLEGALOUTSIDEACTOR` | parser | 4 | Illegal outside a CONSTRUCTOR block |
| 166 | `ILLEGALOUTSIDEADTOR` | parser | 2 | Illegal outside a DESTRUCTOR block |
| 167 | `STRUCTISNOTUNIQUE` | parser | 1 | UDT's with methods must have unique names |
| 168 | `PARENTISNOTACLASS` | parser | 1 | Parent is not a class or UDT |
| 169 | `CTORCHAINMUSTBEFIRST` | parser | 1 | CONSTRUCTOR() chain call not at top of constructor |
| 170 | `BASEINITMUSTBEFIRST` | parser | 1 | BASE() initializer not at top of constructor |
| 171 | `REDIMCTORMUSTBECDEL` | parser | 1 | REDIM on UDT with non-CDECL constructor |
| 172 | `REDIMDTORMUSTBECDEL` | parser | 1 | REDIM on UDT with non-CDECL destructor |
| 173 | `REDIMCTORMUSTHAVEONEPARAM` | parser | 1 | REDIM on UDT with non-parameterless default constructor |
| 174 | `ERASECTORMUSTBECDEL` | parser | 1 | ERASE on UDT with non-CDECL constructor |
| 175 | `ERASEDTORMUSTBECDEL` | parser | 1 | ERASE on UDT with non-CDECL destructor |
| 176 | `ERASECTORMUSTHAVEONEPARAM` | parser | 1 | ERASE on UDT with non-parameterless default constructor |
| 177 | `CANTUNDEF` | parser | 3 | This symbol cannot be undefined |
| 178 | `RETURNMIXEDWITHASSIGN` | parser | 1 | RETURN mixed with 'FUNCTION =' or EXIT FUNCTION (using both styles together is unsupported when returning objects with constructors) |
| 179 | `ASSIGNMIXEDWITHRETURN` | parser | 1 | 'FUNCTION =' or EXIT FUNCTION mixed with RETURN (using both styles together is unsupported when returning objects with constructors) |
| 180 | `MISSINGRETURNFORCTORRESULT` | parser | 1 | Missing RETURN to copy-construct function result |
| 181 | `ILLEGALASSIGNMENT` | parser | 6 | Invalid assignment/conversion |
| 182 | `INVALIDSUBSCRIPT` | parser | 1 | Invalid array subscript |
| 183 | `NODEFAULTCTORDEFINED` | parser | 6 | TYPE or CLASS has no default constructor |
| 184 | `RESULTHASNODEFCTOR` | resolve | 1 | Function result TYPE has no default constructor |
| 185 | `NOBASEINIT` | resolve | 1 | Missing BASE() initializer (base UDT without default constructor requires manual initialization) |
| 186 | `NEEDEXPLICITDEFCTOR` | resolve | 1 | Missing default constructor implementation (base UDT without default constructor requires manual initialization) |
| 187 | `NEEDEXPLICITCOPYCTOR` | resolve | 1 | Missing UDT.constructor(byref as UDT) implementation (base UDT without default constructor requires manual initialization) |
| 188 | `NEEDEXPLICITCOPYCTORCONST` | resolve | 1 | Missing UDT.constructor(byref as const UDT) implementation (base UDT without default constructor requires manual initialization) |
| 189 | `INVALIDPRIORITY` | parser | 2 | Invalid priority attribute |
| 190 | `PARAMCNTFORPROPGET` | parser | 1 | PROPERTY GET should have no parameter, or just one if indexed |
| 191 | `PARAMCNTFORPROPSET` | parser | 1 | PROPERTY SET should have one parameter, or just two if indexed |
| 192 | `EXPECTEDPROPERTY` | parser | 1 | Expected 'PROPERTY' |
| 193 | `ILLEGALOUTSIDEAPROPERTY` | parser | 4 | Illegal outside a PROPERTY block |
| 194 | `PROPERTYHASNOGETMETHOD` | parser | 2 | PROPERTY has no GET method/accessor |
| 195 | `PROPERTYHASNOSETMETHOD` | parser | 1 | PROPERTY has no SET method/accessor |
| 196 | `PROPERTYHASNOIDXGETMETHOD` | parser | 2 | PROPERTY has no indexed GET method/accessor |
| 197 | `PROPERTYHASNOIDXSETMETHOD` | parser | 1 | PROPERTY has no indexed SET method/accessor |
| 198 | `UDTINFORNEEDSOPERATORS` | parser | 7 | Missing overloaded operator:  |
| 199 | `EXPLICITCTORCALLINVECTOR` | parser | 1 | The NEW[] operator does not allow explicit calls to constructors |
| 200 | `VECTORCANTBEINITIALIZED` | parser | 2 | The NEW[] operator only supports the { ANY } initialization |
| 201 | `NEWCANTBEUSEDWITHFIXLENSTRINGS` | parser | 1 | The NEW operator cannot be used with fixed-length strings |
| 202 | `ILLEGALMEMBERACCESS` | parser | 21 | Illegal member access |
| 203 | `EXPECTEDSTMTSEP` | parser | 2 | Expected ':' |
| 204 | `NOACCESSTODEFAULTCTOR` | parser | 5 | The default constructor has no public access |
| 205 | `NOACCESSTOCTOR` | parser | 4 | Constructor has no public access |
| 206 | `NOACCESSTODTOR` | parser | 6 | Destructor has no public access |
| 207 | `NOACCESSTOBASEDEFCTOR` | resolve | 1 | Accessing base UDT's private default constructor |
| 208 | `NOACCESSTOBASEDTOR` | resolve | 1 | Accessing base UDT's private destructor |
| 209 | `ACCESSTONONSTATICMEMBER` | parser | 1 | Illegal non-static member access |
| 210 | `ABSTRACTCTOR` | parser | 1 | Constructor declared ABSTRACT |
| 211 | `VIRTUALCTOR` | parser | 2 | Constructor declared VIRTUAL |
| 212 | `ABSTRACTDTOR` | parser | 1 | Destructor declared ABSTRACT |
| 213 | `MEMBERCANTBESTATIC` | parser | 1 | Member cannot be static |
| 214 | `MEMBERISNTSTATIC` | parser | 2 | Member isn't static |
| 215 | `STATICMEMBERHASNOINSTANCEPTR` | parser | 1 | Only static members can be accessed from static functions and parameter initializers |
| 216 | `PRIVORPUBTTRIBNOTALLOWED` | parser | 1 | The PRIVATE and PUBLIC attributes are not allowed with REDIM, COMMON or EXTERN |
| 217 | `PROCPROTOTYPENOTSTATIC` | parser | 1 | STATIC used here, but not the in the DECLARE statement |
| 218 | `PROCPROTOTYPENOTCONST` | parser | 1 | CONST used here, but not the in the DECLARE statement |
| 219 | `PROCPROTOTYPENOTVIRTUAL` | parser | 1 | VIRTUAL used here, but not the in the DECLARE statement |
| 220 | `PROCPROTOTYPENOTABSTRACT` | parser | 1 | ABSTRACT used here, but not the in the DECLARE statement |
| 221 | `VIRTUALWITHOUTRTTI` | parser | 1 | Method declared VIRTUAL, but UDT does not extend OBJECT |
| 222 | `ABSTRACTWITHOUTRTTI` | parser | 1 | Method declared ABSTRACT, but UDT does not extend OBJECT |
| 223 | `OVERRIDINGNOTHING` | parser | 1 | Not overriding any virtual method |
| 224 | `ABSTRACTBODY` | parser | 1 | Implemented body for an ABSTRACT method |
| 225 | `OVERRIDERETTYPEDIFFERS` | resolve | 2 | Override has different return type than overridden method |
| 226 | `OVERRIDECALLCONVDIFFERS` | resolve | 2 | Override has different calling convention than overridden method |
| 227 | `IMPLICITDTOROVERRIDECALLCONVDIFFERS` | resolve | 1 | Implicit destructor override would have different calling convention |
| 228 | `IMPLICITLETOVERRIDECALLCONVDIFFERS` | resolve | 1 | Implicit LET operator override would have different calling convention |
| 229 | `OVERRIDEISNTCONSTMEMBER` | resolve | 1 | Override is not a CONST member like the overridden method |
| 230 | `OVERRIDEISCONSTMEMBER` | resolve | 1 | Override is a CONST member, but the overridden method is not |
| 231 | `OVERRIDEPARAMSDIFFER` | resolve | 2 | Override has different parameters than overridden method |
| 232 | `OPERATORCANTBESTATIC` | parser | 1 | This operator cannot be STATIC |
| 233 | `OPERATORCANTBEVIRTUAL` | parser | 1 | This operator is implicitly STATIC and cannot be VIRTUAL or ABSTRACT |
| 234 | `OPERATORCANTBECONST` | parser | 1 | This operator is implicitly STATIC and cannot be CONST |
| 235 | `PARAMMUSTBEANINTEGER` | parser | 2 | Parameter must be an integer |
| 236 | `PARAMMUSTBEAPOINTER` | parser | 2 | Parameter must be a pointer |
| 237 | `AUTONEEDSINITIALIZER` | parser | 3 | Expected initializer |
| 238 | `KEYWORDFIELDSNOTALLOWEDINCLASSES` | parser | 2 | Fields cannot be named as keywords in TYPE's that contain member functions or in CLASS'es |
| 239 | `ILLEGALOUTSIDEFORSTMT` | parser | 2 | Illegal outside a FOR compound statement |
| 240 | `ILLEGALOUTSIDEDOSTMT` | parser | 2 | Illegal outside a DO compound statement |
| 241 | `ILLEGALOUTSIDEWHILESTMT` | parser | 2 | Illegal outside a WHILE compound statement |
| 242 | `ILLEGALOUTSIDESELSTMT` | parser | 1 | Illegal outside a SELECT compound statement |
| 243 | `EXPECTEDFOR` | parser | 2 | Expected 'FOR' |
| 244 | `EXPECTEDDO` | parser | 2 | Expected 'DO' |
| 245 | `EXPECTEDWHILE` | parser | 2 | Expected 'WHILE' |
| 246 | `EXPECTEDSELECT` | parser | 1 | Expected 'SELECT' |
| 247 | `NOENCLOSEDFORSTMT` | parser | 2 | No outer FOR compound statement found |
| 248 | `NOENCLOSEDDOSTMT` | parser | 2 | No outer DO compound statement found |
| 249 | `NOENCLOSEDWHILESTMT` | parser | 2 | No outer WHILE compound statement found |
| 250 | `NOENCLOSEDSELSTMT` | parser | 1 | No outer SELECT compound statement found |
| 251 | `INVALIDEXITSTMT` | parser | 1 | Expected 'CONSTRUCTOR', 'DESTRUCTOR', 'DO', 'FOR', 'FUNCTION', 'OPERATOR', 'PROPERTY', 'SELECT', 'SUB' or 'WHILE' |
| 252 | `INVALIDCONTINUESTMT` | parser | 1 | Expected 'DO', 'FOR' or 'WHILE' |
| 253 | `ILLEGALOUTSIDEASUB` | parser | 3 | Illegal outside a SUB block |
| 254 | `ILLEGALOUTSIDEAFUNCTION` | parser | 4 | Illegal outside a FUNCTION block |
| 255 | `AMBIGUOUSSYMBOLACCESS` | parser | 1 | Ambiguous symbol access, explicit scope resolution required |
| 256 | `NOELEMENTSDEFINED` | parser | 2 | An ENUM, TYPE or UNION cannot be empty |
| 257 | `NONSCOPEDENUM` | parser | 3 | ENUM's declared inside EXTERN .. END EXTERN blocks don't open new scopes |
| 258 | `STATICNONMEMBERPROC` | parser | 1 | STATIC used on non-member procedure |
| 259 | `CONSTNONMEMBERPROC` | parser | 1 | CONST used on non-member procedure |
| 260 | `ABSTRACTNONMEMBERPROC` | parser | 1 | ABSTRACT used on non-member procedure |
| 261 | `VIRTUALNONMEMBERPROC` | parser | 1 | VIRTUAL used on non-member procedure |
| 262 | `INVALIDINITIALIZER` | dead | 0 | Invalid initializer |
| 263 | `NOOOPINFUNCTIONS` | parser | 1 | Objects with default [con\|de]structors or methods are only allowed in the module level |
| 264 | `NOSTATICMEMBERVARINNESTEDUDT` | parser | 1 | Static member variable in nested UDT (only allowed in toplevel UDTs) |
| 265 | `EXPECTEDUDT` | parser | 6 | Symbol not a CLASS, ENUM, TYPE or UNION type |
| 266 | `TOOMANYELEMENTS` | parser | 1 | Too many elements |
| 267 | `NOTADATAMEMBER` | parser | 1 | Only data members supported |
| 268 | `UNIONSNOTALLOWED` | parser | 1 | UNIONs are not allowed |
| 269 | `ARRAYSNOTALLOWED` | parser | 1 | Arrays are not allowed |
| 270 | `COMMONCANTBEOBJINST` | parser | 1 | COMMON variables cannot be object instances of CLASS/TYPE's with cons/destructors |
| 271 | `CLONECANTTAKESELFBYVAL` | parser | 2 | Cloning operators (LET, Copy constructors) can't take a byval arg of the parent's type |
| 272 | `INVALIDREFERENCETOLOCAL` | parser | 4 | Local symbols can't be referenced |
| 273 | `EXPECTEDPTRORPOINTER` | parser | 2 | Expected 'PTR' or 'POINTER' |
| 274 | `TOOMANYPTRINDIRECTIONS` | parser | 5 | Too many levels of pointer indirection |
| 275 | `DYNAMICARRAYSCANTBECONST` | parser | 1 | Dynamic arrays can't be const |
| 276 | `CONSTUDTTONONCONSTMETHOD` | resolve | 1 | Const UDT cannot invoke non-const method |
| 277 | `ELEMENTSMUSTBEEMPTY` | parser | 4 | Elements must be empty for strings and arrays |
| 278 | `NOGOSUB` | parser | 1 | GOSUB disabled, use 'OPTION GOSUB' to enable |
| 279 | `INVALIDLANG` | parser | 2 | Invalid -lang |
| 280 | `CANTUSEANYINITELLIPSIS` | parser | 1 | Can't use ANY as initializer in array with ellipsis bound |
| 281 | `MUSTHAVEINITWITHELLIPSIS` | parser | 1 | Must have initializer with array with ellipsis bound |
| 282 | `CANTUSEELLIPSISASLOWERBOUND` | parser | 1 | Can't use ... as lower bound |
| 283 | `FORNEXTVARIABLEMISMATCH` | parser | 1 | FOR/NEXT variable name mismatch |
| 284 | `OPTIONREQUIRESSSE` | unreachable | 1 | Selected option requires an SSE FPU mode |
| 285 | `EXPECTEDRELOP` | parser | 1 | Expected relational operator ( =, >, <, <>, <=, >= ) |
| 286 | `STMTUNSUPPORTEDINGCC` | parser | 2 | Unsupported statement in -gen gcc mode |
| 287 | `TOOMANYLABELS` | parser | 3 | Too many labels |
| 288 | `UNSUPPORTEDFUNCTION` | parser | 2 | Unsupported function |
| 289 | `EXPECTEDSUB` | parser | 2 | Expected sub |
| 290 | `EXPECTEDPPENDIF` | parser | 2 | Expected '#ENDIF' |
| 291 | `RCFILEWRONGTARGET` | unreachable | 2 | Resource file given for target system that does not support them |
| 292 | `OBJFILEWITHOUTINPUTFILE` | unreachable | 1 | -o <file> option without corresponding input file |
| 293 | `EXPECTEDCLASSTYPE` | parser | 2 | Not extending a TYPE/UNION (a TYPE/UNION can only extend other TYPEs/UNIONs) |
| 294 | `ILLEGALOUTSIDEAMETHOD` | parser | 2 | Illegal outside a CLASS, TYPE or UNION method |
| 295 | `CLASSNOTDERIVED` | parser | 3 | CLASS, TYPE or UNION not derived |
| 296 | `CLASSWITHOUTCTOR` | dead | 0 | CLASS, TYPE or UNION has no constructor |
| 297 | `TYPEHASNORTTI` | parser | 2 | Symbol type has no Run-Time Type Info (RTTI) |
| 298 | `TYPESARENOTRELATED` | parser | 1 | Types have no hierarchical relation |
| 299 | `TYPEMUSTBEAUDT` | parser | 2 | Expected a CLASS, TYPE or UNION symbol type |
| 300 | `CASTDERIVEDPTRFROMINCOMPATIBLE` | resolve | 1 | Casting derived UDT pointer from incompatible pointer type |
| 301 | `CASTDERIVEDPTRFROMUNRELATED` | resolve | 1 | Casting derived UDT pointer from unrelated UDT pointer type |
| 302 | `CASTDERIVEDPTRTOINCOMPATIBLE` | resolve | 1 | Casting derived UDT pointer to incompatible pointer type |
| 303 | `CASTDERIVEDPTRTOUNRELATED` | resolve | 1 | Casting derived UDT pointer to unrelated UDT pointer type |
| 304 | `EMPTYALIASSTRING` | parser | 2 | ALIAS name string is empty |
| 305 | `EMPTYLIBSTRING` | parser | 1 | LIB name string is empty |
| 306 | `OBJECTOFABSTRACTCLASS` | parser | 1 | UDT has unimplemented abstract methods |
| 307 | `CALLTOABSTRACT` | parser | 1 | Non-virtual call to ABSTRACT method |
| 308 | `PPASSERT_FAILED` | parser | 1 | #ASSERT condition failed |
| 309 | `EXPECTEDGT` | parser | 5 | Expected '>' |
| 310 | `INVALIDSIZE` | parser | 1 | Invalid size |
| 311 | `DIFFERENTALIASTHANPROTO` | parser | 1 | ALIAS name here does not match ALIAS given in DECLARE prototype |
| 312 | `VARARGONLYALLOWEDINCDECL` | parser | 1 | vararg parameters are only allowed in CDECL procedures |
| 313 | `VARARGNOTALLOWEDASFIRSTPARAM` | parser | 1 | the first parameter in a procedure may not be vararg |
| 314 | `CONSTCTOR` | parser | 1 | CONST used on constructor (not needed) |
| 315 | `CONSTDTOR` | parser | 1 | CONST used on destructor (not needed) |
| 316 | `NOBYREFFUNCTIONRESULT` | parser | 1 | Byref function result not set |
| 317 | `RESULTASSIGNOUTSIDEFUNCTION` | parser | 1 | Function result assignment outside of the function |
| 318 | `TYPEMISMATCHINBYREFRESULTASSIGN` | parser | 1 | Type mismatch in byref function result assignment |
| 319 | `ASMOPTIONGIVENFORNONX86` | unreachable | 1 | -asm att\|intel option given, but not supported for this target (only x86 or x86_64) |
| 320 | `MISSINGREFINIT` | parser | 1 | Reference not initialized |
| 321 | `INCOMPATIBLEREFINIT` | dead | 0 | Incompatible reference initializer |
| 322 | `ARRAYOFREFS` | parser | 1 | Array of references - not supported yet |
| 323 | `INVALIDCASERANGE` | parser | 1 | Invalid CASE range, start value is greater than the end value |
| 324 | `BYREFFIXSTR` | parser | 1 | Fixed-length string combined with BYREF (not supported) |
| 325 | `ILLEGALUSEOFRESERVEDSYMBOL` | parser | 1 | Illegal use of reserved symbol |
| 326 | `EXPECTEDCOMMAORSEMICOLON` | parser | 2 | Expected ',' or ';' |
| 327 | `EXPECTEDFILENUMBEREXPRESSION` | parser | 1 | Expected file number expression |
| 328 | `MALFORMEDSOURCEDATEEPOCH` | resolve | 1 | Malformed SOURCE_DATE_EPOCH environment variable |

## Appendix B — fbc warning catalog (49 messages)

`Lvl` is the `-w` threshold at which the warning is on; level 0 is off
by default (45 of 49 are on by default). Source: `error.bas` tag
`1.10.2`.

| # | Lvl | Code name | Refs | Message |
|---|-----|-----------|------|---------|
| 1 | 2 | `PASSINGSCALARASPTR` | 1 | Passing scalar as pointer |
| 2 | 2 | `PASSINGPTRTOSCALAR` | 3 | Passing pointer to scalar |
| 3 | 2 | `PASSINGDIFFPOINTERS` | 2 | Passing different pointer types |
| 4 | 2 | `SUSPICIOUSPTRASSIGN` | 1 | Suspicious pointer assignment |
| 5 | 1 | `IMPLICITCONVERSION` | 5 | Implicit conversion |
| 6 | 2 | `CANNOTEXPORT` | 1 | Cannot export symbol without -export option |
| 7 | 2 | `IDNAMETOOBIG` | 1 | Identifier's name too big, truncated |
| 8 | 2 | `NUMBERTOOBIG` | 14 | Literal number too big, truncated |
| 9 | 2 | `LITSTRINGTOOBIG` | 6 | Literal string too big, truncated |
| 10 | 1 | `POINTERFIELDS` | 4 | UDT with pointer, var-len string, or var-len array fields |
| 11 | 1 | `IMPLICITALLOCATION` | 1 | Implicit variable allocation |
| 12 | 1 | `NOCLOSINGQUOTE` | 2 | Missing closing quote in literal string |
| 13 | 1 | `NOFUNCTIONRESULT` | 1 | Function result was not explicitly set |
| 14 | 2 | `BRANCHCROSSINGLOCALVAR` | 1 | Branch crossing local variable definition |
| 15 | 1 | `NOEXPLICITPARAMMODE` | 1 | No explicit BYREF or BYVAL |
| 16 | 1 | `POSSIBLEESCSEQ` | 2 | Possible escape sequence found in |
| 17 | 1 | `PARAMSIZETOOBIG` | 1 | The type length is too large, consider passing BYREF |
| 18 | 2 | `PARAMLISTSIZETOOBIG` | 1 | The length of the parameters list is too large, consider passing UDT's BYREF |
| 19 | 2 | `ANYINITHASNOEFFECT` | 1 | The ANY initializer has no effect on UDT's with default constructors |
| 20 | 3 | `MIXINGMTMODES` | 1 | Object files or libraries with mixed multithreading (-mt) options |
| 21 | 3 | `MIXINGLANGMODES` | 1 | Object files or libraries with mixed language (-lang) options |
| 22 | 1 | `DELETEANYPTR` | 1 | Deleting ANY pointers is undefined |
| 23 | 3 | `HUGEARRAYONSTACK` | 1 | Array too large for stack, consider making it var-len or SHARED |
| 24 | 3 | `HUGEVARONSTACK` | 2 | Variable too large for stack, consider making it SHARED |
| 25 | 1 | `CONVOVERFLOW` | 3 | Overflow in constant conversion |
| 26 | 1 | `NEXTVARMEANINGLESS` | 1 | Variable following NEXT is meaningless |
| 27 | 1 | `CASTTONONPTR` | 2 | Cast to non-pointer |
| 28 | 1 | `RETURNMETHODMISMATCH` | 2 | Return method mismatch |
| 29 | 1 | `PASSINGPTR` | 4 | Passing Pointer |
| 30 | 1 | `CMDLINEOVERRIDES` | 2 | Command line option overrides directive |
| 31 | 1 | `DIRECTIVEIGNORED` | 1 | Directive ignored after first pass |
| 32 | 1 | `IFFOUNDAFTERELSE` | 1 | 'IF' statement found directly after multi-line 'ELSE' |
| 33 | 1 | `SHIFTEXCEEDSBITSINDATATYPE` | 1 | Shift value greater than or equal to number of bits in data type |
| 34 | 1 | `BYREFEQAFTERPARENS` | 1 | '=' parsed as equality operator in function argument, not assignment to BYREF function result |
| 35 | 1 | `OPERANDSMIXEDSIGNEDNESS` | 4 | Mixing signed/unsigned operands |
| 36 | 1 | `MISMATCHINGPARAMINIT` | 1 | Mismatching parameter initializer |
| 37 | 2 | `AMBIGIOUSLENSIZEOF` | 2 |  |
| 38 | 1 | `OPERANDSMIXEDTYPES` | 1 | Suspicious logic operation, mixed boolean and non-boolean operands |
| 39 | 1 | `REDEFINITIONOFINTRINSIC` | 2 | Redefinition of intrinsic |
| 40 | 0 | `CONSTQUALIFIERDISCARDED` | 2 | CONST qualifier discarded |
| 41 | 0 | `RETURNTYPEMISMATCH` | 2 | Return type mismatch |
| 42 | 0 | `CALLINGCONVMISMATCH` | 2 | Calling convention mismatch |
| 43 | 0 | `ARGCNTMISMATCH` | 23 | Argument count mismatch |
| 44 | 1 | `SUFFIXIGNORED` | 1 | Suffix ignored |
| 45 | 1 | `FORENDTOOBIG` | 1 | FOR counter variable is unable to exceed limit value |
| 46 | 1 | `CMDLINEIGNORED` | 3 | #cmdline ignored |
| 47 | 1 | `RESERVEDGLOBALSYMBOL` | 2 | Use of reserved global or backend symbol |
| 48 | 1 | `EXPECTEDDIGIT` | 1 | Expected digit |
| 49 | 1 | `UPCASTDISCARDSINITIALIZER` | 1 | Up-casting discards initializer(s) |

## Appendix C — codes grouped by bucket


### warning (49)

`PASSINGSCALARASPTR`, `PASSINGPTRTOSCALAR`, `PASSINGDIFFPOINTERS`, `SUSPICIOUSPTRASSIGN`, 
`IMPLICITCONVERSION`, `CANNOTEXPORT`, `IDNAMETOOBIG`, `NUMBERTOOBIG`, `LITSTRINGTOOBIG`, 
`POINTERFIELDS`, `IMPLICITALLOCATION`, `NOCLOSINGQUOTE`, `NOFUNCTIONRESULT`, 
`BRANCHCROSSINGLOCALVAR`, `NOEXPLICITPARAMMODE`, `POSSIBLEESCSEQ`, `PARAMSIZETOOBIG`, 
`PARAMLISTSIZETOOBIG`, `ANYINITHASNOEFFECT`, `MIXINGMTMODES`, `MIXINGLANGMODES`, `DELETEANYPTR`, 
`HUGEARRAYONSTACK`, `HUGEVARONSTACK`, `CONVOVERFLOW`, `NEXTVARMEANINGLESS`, `CASTTONONPTR`, 
`RETURNMETHODMISMATCH`, `PASSINGPTR`, `CMDLINEOVERRIDES`, `DIRECTIVEIGNORED`, `IFFOUNDAFTERELSE`, 
`SHIFTEXCEEDSBITSINDATATYPE`, `BYREFEQAFTERPARENS`, `OPERANDSMIXEDSIGNEDNESS`, 
`MISMATCHINGPARAMINIT`, `AMBIGIOUSLENSIZEOF`, `OPERANDSMIXEDTYPES`, `REDEFINITIONOFINTRINSIC`, 
`CONSTQUALIFIERDISCARDED`, `RETURNTYPEMISMATCH`, `CALLINGCONVMISMATCH`, `ARGCNTMISMATCH`, 
`SUFFIXIGNORED`, `FORENDTOOBIG`, `CMDLINEIGNORED`, `RESERVEDGLOBALSYMBOL`, `EXPECTEDDIGIT`, 
`UPCASTDISCARDSINITIALIZER`

### parser (263)

`ARGCNTMISMATCH`, `EXPECTEDEOL`, `DUPDEFINITION`, `EXPECTINGAS`, `EXPECTEDLPRNT`, `EXPECTEDRPRNT`, 
`UNDEFINEDSYMBOL`, `EXPECTEDEXPRESSION`, `EXPECTEDEQ`, `EXPECTEDCONST`, `EXPECTEDTO`, 
`EXPECTEDNEXT`, `EXPECTEDIDENTIFIER`, `EXPECTEDMINUS`, `EXPECTEDCOMMA`, `SYNTAXERROR`, 
`ELEMENTNOTDEFINED`, `EXPECTEDENDTYPE`, `TYPEMISMATCH`, `INTERNAL`, `PARAMTYPEMISMATCH`, 
`INVALIDDATATYPES`, `INVALIDCHARACTER`, `RECLEVELTOODEEP`, `EXPECTEDPOINTER`, `EXPECTEDLOOP`, 
`EXPECTEDWEND`, `EXPECTEDTHEN`, `EXPECTEDENDIF`, `ILLEGALEND`, `EXPECTEDCASE`, `EXPECTEDENDSELECT`, 
`WRONGDIMENSIONS`, `BOUNDSDIFFERFROMEXTERN`, `RETURNTYPEMISMATCH`, `CALLINGCONVMISMATCH`, 
`VARIABLENOTDECLARED`, `ILLEGALOUTSIDECOMP`, `EXPECTEDENDASM`, `EXPECTEDSEMICOLON`, 
`TOOMANYDIMENSIONS`, `ARRAYTOOBIG`, `EXPECTEDSCALAR`, `ILLEGALOUTSIDEAPROC`, 
`EXPECTEDDYNAMICARRAY`, `CANNOTRETURNFIXLENFROMFUNCTS`, `PARAMTYPEMISMATCHAT`, 
`ILLEGALPARAMSPECAT`, `EXPECTEDENDWITH`, `ILLEGALINSIDEASUB`, `ILLEGALINSIDESELECT`, 
`EXPECTEDARRAY`, `EXPECTEDLBRACE`, `EXPECTEDRBRACE`, `EXPECTEDRBRACKET`, `TOOMANYEXPRESSIONS`, 
`EXPECTEDRESTYPE`, `RANGETOOLARGE`, `INCOMPLETETYPE`, `ARRAYNOTALLOCATED`, `EXPECTEDINDEX`, 
`EXPECTEDENDENUM`, `CANTINITDYNAMICARRAYS`, `DYNAMICARRAYWITHELLIPSIS`, `ARRAYFIELDWITHELLIPSIS`, 
`INVALIDBITFIELD`, `TOOMANYPARAMS`, `CANTINITDYNAMICSTRINGS`, `RECURSIVEUDT`, `RECURSIVEMACRO`, 
`CANTINCLUDEPERIODS`, `ARRAYOUTOFBOUNDS`, `EXPECTEDANY`, `EXPECTEDENDSCOPE`, `ILLEGALINSIDEASCOPE`, 
`NOMATCHINGPROC`, `STACKUNDERFLOW`, `LOOPWITHOUTDO`, `NEXTWITHOUTFOR`, `WENDWITHOUTWHILE`, 
`ENDWITHWITHOUTWITH`, `ENDIFWITHOUTIF`, `ENDSELECTWITHOUTSELECT`, `ENDSUBWITHOUTSUB`, 
`ENDSCOPEWITHOUTSCOPE`, `ENDNAMESPACEWITHOUTNAMESPACE`, `ENDEXTERNWITHOUTEXTERN`, 
`ELSEIFWITHOUTIF`, `ELSEWITHOUTIF`, `CASEWITHOUTSELECT`, `CONSTANTCANTBECHANGED`, `EXPECTEDPERIOD`, 
`EXPECTEDENDNAMESPACE`, `ILLEGALINSIDEANAMESPC`, `CANTREMOVENAMESPCSYMBOLS`, `EXPECTEDENDEXTERN`, 
`EXPECTEDENDSUB`, `EXPECTEDENDFUNCTION`, `EXPECTEDENDCTOR`, `EXPECTEDENDDTOR`, 
`EXPECTEDENDOPERATOR`, `EXPECTEDENDPROPERTY`, `DECLOUTSIDENAMESPC`, `EXPECTEDENDCOMMENT`, 
`TOOMANYERRORS`, `EXPECTEDMACRO`, `CANNOTINITEXTERNORCOMMON`, `DYNAMICEXTERNCANTHAVEBOUNDS`, 
`ATLEASTONEPARAMMUSTBEANUDT`, `SAMEPARAMANDRESULTTYPES`, `INVALIDRESULTTYPEFORTHISOP`, 
`PARAMTYPEINCOMPATIBLEWITHPARENT`, `VARARGPARAMNOTALLOWED`, `ILLEGALOUTSIDEANOPERATOR`, 
`PARAMCANTBEOPTIONAL`, `INVALIDARRAYINDEX`, `OPMUSTBEAMETHOD`, `OPCANNOTBEAMETHOD`, 
`METHODINANONUDT`, `CONSTINANONUDT`, `STATICVARINANONUDT`, `EXPECTEDOPERATOR`, `DECLOUTSIDECLASS`, 
`DTORCANTCONTAINPARAMS`, `EXPECTEDCLASSID`, `CTORINUNION`, `ILLEGALOUTSIDEACTOR`, 
`ILLEGALOUTSIDEADTOR`, `STRUCTISNOTUNIQUE`, `PARENTISNOTACLASS`, `CTORCHAINMUSTBEFIRST`, 
`BASEINITMUSTBEFIRST`, `REDIMCTORMUSTBECDEL`, `REDIMDTORMUSTBECDEL`, `REDIMCTORMUSTHAVEONEPARAM`, 
`ERASECTORMUSTBECDEL`, `ERASEDTORMUSTBECDEL`, `ERASECTORMUSTHAVEONEPARAM`, `CANTUNDEF`, 
`RETURNMIXEDWITHASSIGN`, `ASSIGNMIXEDWITHRETURN`, `MISSINGRETURNFORCTORRESULT`, 
`ILLEGALASSIGNMENT`, `INVALIDSUBSCRIPT`, `NODEFAULTCTORDEFINED`, `INVALIDPRIORITY`, 
`PARAMCNTFORPROPGET`, `PARAMCNTFORPROPSET`, `EXPECTEDPROPERTY`, `ILLEGALOUTSIDEAPROPERTY`, 
`PROPERTYHASNOGETMETHOD`, `PROPERTYHASNOSETMETHOD`, `PROPERTYHASNOIDXGETMETHOD`, 
`PROPERTYHASNOIDXSETMETHOD`, `UDTINFORNEEDSOPERATORS`, `EXPLICITCTORCALLINVECTOR`, 
`VECTORCANTBEINITIALIZED`, `NEWCANTBEUSEDWITHFIXLENSTRINGS`, `ILLEGALMEMBERACCESS`, 
`EXPECTEDSTMTSEP`, `NOACCESSTODEFAULTCTOR`, `NOACCESSTOCTOR`, `NOACCESSTODTOR`, 
`ACCESSTONONSTATICMEMBER`, `ABSTRACTCTOR`, `VIRTUALCTOR`, `ABSTRACTDTOR`, `MEMBERCANTBESTATIC`, 
`MEMBERISNTSTATIC`, `STATICMEMBERHASNOINSTANCEPTR`, `PRIVORPUBTTRIBNOTALLOWED`, 
`PROCPROTOTYPENOTSTATIC`, `PROCPROTOTYPENOTCONST`, `PROCPROTOTYPENOTVIRTUAL`, 
`PROCPROTOTYPENOTABSTRACT`, `VIRTUALWITHOUTRTTI`, `ABSTRACTWITHOUTRTTI`, `OVERRIDINGNOTHING`, 
`ABSTRACTBODY`, `OPERATORCANTBESTATIC`, `OPERATORCANTBEVIRTUAL`, `OPERATORCANTBECONST`, 
`PARAMMUSTBEANINTEGER`, `PARAMMUSTBEAPOINTER`, `AUTONEEDSINITIALIZER`, 
`KEYWORDFIELDSNOTALLOWEDINCLASSES`, `ILLEGALOUTSIDEFORSTMT`, `ILLEGALOUTSIDEDOSTMT`, 
`ILLEGALOUTSIDEWHILESTMT`, `ILLEGALOUTSIDESELSTMT`, `EXPECTEDFOR`, `EXPECTEDDO`, `EXPECTEDWHILE`, 
`EXPECTEDSELECT`, `NOENCLOSEDFORSTMT`, `NOENCLOSEDDOSTMT`, `NOENCLOSEDWHILESTMT`, 
`NOENCLOSEDSELSTMT`, `INVALIDEXITSTMT`, `INVALIDCONTINUESTMT`, `ILLEGALOUTSIDEASUB`, 
`ILLEGALOUTSIDEAFUNCTION`, `AMBIGUOUSSYMBOLACCESS`, `NOELEMENTSDEFINED`, `NONSCOPEDENUM`, 
`STATICNONMEMBERPROC`, `CONSTNONMEMBERPROC`, `ABSTRACTNONMEMBERPROC`, `VIRTUALNONMEMBERPROC`, 
`NOOOPINFUNCTIONS`, `NOSTATICMEMBERVARINNESTEDUDT`, `EXPECTEDUDT`, `TOOMANYELEMENTS`, 
`NOTADATAMEMBER`, `UNIONSNOTALLOWED`, `ARRAYSNOTALLOWED`, `COMMONCANTBEOBJINST`, 
`CLONECANTTAKESELFBYVAL`, `INVALIDREFERENCETOLOCAL`, `EXPECTEDPTRORPOINTER`, 
`TOOMANYPTRINDIRECTIONS`, `DYNAMICARRAYSCANTBECONST`, `ELEMENTSMUSTBEEMPTY`, `NOGOSUB`, 
`INVALIDLANG`, `CANTUSEANYINITELLIPSIS`, `MUSTHAVEINITWITHELLIPSIS`, `CANTUSEELLIPSISASLOWERBOUND`, 
`FORNEXTVARIABLEMISMATCH`, `EXPECTEDRELOP`, `STMTUNSUPPORTEDINGCC`, `TOOMANYLABELS`, 
`UNSUPPORTEDFUNCTION`, `EXPECTEDSUB`, `EXPECTEDPPENDIF`, `EXPECTEDCLASSTYPE`, 
`ILLEGALOUTSIDEAMETHOD`, `CLASSNOTDERIVED`, `TYPEHASNORTTI`, `TYPESARENOTRELATED`, 
`TYPEMUSTBEAUDT`, `EMPTYALIASSTRING`, `EMPTYLIBSTRING`, `OBJECTOFABSTRACTCLASS`, `CALLTOABSTRACT`, 
`PPASSERT_FAILED`, `EXPECTEDGT`, `INVALIDSIZE`, `DIFFERENTALIASTHANPROTO`, 
`VARARGONLYALLOWEDINCDECL`, `VARARGNOTALLOWEDASFIRSTPARAM`, `CONSTCTOR`, `CONSTDTOR`, 
`NOBYREFFUNCTIONRESULT`, `RESULTASSIGNOUTSIDEFUNCTION`, `TYPEMISMATCHINBYREFRESULTASSIGN`, 
`MISSINGREFINIT`, `ARRAYOFREFS`, `INVALIDCASERANGE`, `BYREFFIXSTR`, `ILLEGALUSEOFRESERVEDSYMBOL`, 
`EXPECTEDCOMMAORSEMICOLON`, `EXPECTEDFILENUMBEREXPRESSION`

### resolve (29)

`UNDEFINEDLABEL`, `UDTTOOBIG`, `AMBIGUOUSCALLTOPROC`, `DIVBYZERO`, `BRANCHTARGETOUTSIDECURRPROC`, 
`BRANCHCROSSINGDYNDATADEF`, `VARLENSTRINGINUNION`, `DYNAMICARRAYINUNION`, `DTORINUNION`, 
`RESULTHASNODEFCTOR`, `NOBASEINIT`, `NEEDEXPLICITDEFCTOR`, `NEEDEXPLICITCOPYCTOR`, 
`NEEDEXPLICITCOPYCTORCONST`, `NOACCESSTOBASEDEFCTOR`, `NOACCESSTOBASEDTOR`, 
`OVERRIDERETTYPEDIFFERS`, `OVERRIDECALLCONVDIFFERS`, `IMPLICITDTOROVERRIDECALLCONVDIFFERS`, 
`IMPLICITLETOVERRIDECALLCONVDIFFERS`, `OVERRIDEISNTCONSTMEMBER`, `OVERRIDEISCONSTMEMBER`, 
`OVERRIDEPARAMSDIFFER`, `CONSTUDTTONONCONSTMETHOD`, `CASTDERIVEDPTRFROMINCOMPATIBLE`, 
`CASTDERIVEDPTRFROMUNRELATED`, `CASTDERIVEDPTRTOINCOMPATIBLE`, `CASTDERIVEDPTRTOUNRELATED`, 
`MALFORMEDSOURCEDATEEPOCH`

### dialect (5)

`ONLYVALIDINLANG`, `DEFTYPEONLYVALIDINLANG`, `SUFFIXONLYVALIDINLANG`, 
`IMPLICITVARSONLYVALIDINLANG`, `AUTOVARONLYVALIDINLANG`

### dead (18)

`EXPECTEDEOF`, `INNERPROCNOTALLOWED`, `EXPECTEDENDSUBORFUNCT`, `VARIABLEREQUIRED`, 
`PROCNOTDECLARED`, `ARRAYALREADYDIMENSIONED`, `ILLEGALRESUMEERROR`, `FORWARDREFNOTALLOWED`, 
`MACROTEXTTOOLONG`, `MISSINGCMDOPTION`, `CANTPASSUDTRESULTBYREF`, `CANTINITDYNAMICFIELDS`, 
`BRANCHTOBLOCKWITHLOCALVARS`, `PARAMORRESULTMUSTBEANUDT`, `SAMEPARAMETERTYPES`, 
`INVALIDINITIALIZER`, `CLASSWITHOUTCTOR`, `INCOMPATIBLEREFINIT`

### unreachable (13)

`FILENOTFOUND`, `FILEACCESSERROR`, `INVALIDCMDOPTION`, `DOSWITHNONX86`, `GENGASWITHNONX86`, 
`GENGASWITHOUTINTEL`, `PICNOTSUPPORTEDFOREXE`, `PICNOTSUPPORTEDFORTARGET`, `EXEMISSING`, 
`OPTIONREQUIRESSSE`, `RCFILEWRONGTARGET`, `OBJFILEWITHOUTINPUTFILE`, `ASMOPTIONGIVENFORNONX86`

