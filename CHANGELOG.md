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

### 2026-10-09 — Block closers carry fbc's own error numbers

- **The block-structure diagnostics now report fbc's own number and link its
  wiki.** An unterminated `Sub` is `fbc error: 125` (fbc's `EXPECTEDENDSUB`)
  rather than a generic `unterminated-block`, a `Next` with no `For` is
  `fbc error: 107`, and every mapped diagnostic carries
  `codeDescription.href` = the `CompilerErrMsg` wiki page. The number is keyed
  on the *specific* closer the parser wanted — `13` for `NEXT`, `19` for
  `END TYPE`/`END UNION`, `45`/`60`/`74`/`95`/`121`/`124` for the rest,
  `125`–`130` for the procedure family, and `33 ILLEGALEND` for
  `END FOR`/`END WHILE` and the bare `end type`/`end union`/`end enum`/
  `end asm` — so a reader who knows `error 125` learns something from our
  output. The numbers came out of the compiler (`tools/fbc_catalog.tsv`, M21
  step 6) and were re-probed against the pinned `/usr/bin/fbc` for every
  spelling before they were wired in.
- **Two fields, on purpose.** `Diagnostic.code` stays the parser's coarse
  routing key (`unterminated-block`, `stray-closer`), which the quick-fix
  table and the corpus goldens key on; a new `Diagnostic.fbcError` carries the
  number, and `toLsDiagnostic` is the one place that turns it into the wire
  `code` + `codeDescription`. That is what keeps a single `Insert 'END SUB'`
  fix covering all six procedure spellings while the client still sees the
  exact number: a fix keyed on the reported string would need one row per
  block kind. The preprocessor blocks are deliberately unmapped — fbc's catalog
  covers neither `#if` nor `#macro`, so they keep our code and carry no link.
- `parser_checks` pins the mapping per closer kind (and the preprocessor
  zero); `session_code_actions_checks` pins the wire code **and** the href on
  the wire; the existing wire-code assertions moved to the numbers they now
  see. `ctest` 19/19, changed files clang-format clean.

**Deviation from the plan, and why.** The plan coupled this split with the
parser false-positive fixes in one wave; this ships the split alone. The
false-positive set turned out to be 148 files across a dozen shapes rather than
a tidy list (re-run against the pinned suite: 148 files fbc accepts and we
reject, all block-structure), which is an open-ended parser-correctness job,
while the split is bounded and only relabels diagnostics that already fire. The
coupling is preserved where it matters: the false-positive fixes stay ahead of
any *new* fbc-coded check, and they are the next wave.

### 2026-10-08 — The outline tells the truth: anonymous names, scope subtrees, a selection inside the range

- **A block written without a name now says so: `<anonymous enum>`,
  `<anonymous union>`, `<anonymous type>`** — fbc's own word for the construct,
  and ProPgTypeUnion's examples are all written that way. It goes into `name`
  only: `key` stays empty, because two bare `enum`s sharing one placeholder
  key would re-open the `duplicate definition: ''` collision that the
  2026-10-07 entry closed by skipping empty keys, and *empty* is what dedupe,
  the index, resolution, completion, and rename all read as "unnamed".
  **The defect was structural, not cosmetic.** `raygui.bi` has 19 bare enums
  and each one answered `documentSymbol` with `"name": ""`; Kate's
  `parseSymbol` keys its parent map by *name* and, for an entry with no
  `containerName`, defaults the parent to `index.find("")` — a key our empty
  names had just inserted, and since the last one inserted is the one found,
  each bare enum became the parent of the next: 19 empty names, 19 levels.
  **Lesson: to a client that maps by name, an absent name is not absence but
  a key it already holds, and it will build arbitrary structure on it.**
- **Scope subtrees are in the reply.** The converter recursed into `if`/`for`/
  `select` and dropped the node, so every block-local `dim` inside one was
  invisible in the outline. They are emitted as the **Namespace** kind and
  pruned when their subtree is empty. Namespace because Kate's symbol view
  pushes a variable under a *function* node through its "skip local variable"
  filter but hands a package-icon parent the whole subtree — the parent's kind
  decides whether a child survives the client's filter, so hiding a node and
  flattening it are not the same request — and pruned-when-empty because an
  `if` that declares nothing is structure the outline has no opinion about,
  while the entries that carry a local are exactly the ones worth the space.
- **`selectionRange` always lies inside `range`.** A nameless block left its
  selection at `{0,0}` — in `raygui.bi` that is 382 lines above the block, so
  an outline click bound the client to the wrong line. The default is now the
  block's own opener keyword, overwritten by the name token when there is one:
  one place, both shapes, with a recursive *selection ⊆ range* invariant over
  the whole nesting to keep it that way.
- **Anonymous containers inherit the enclosing access section, and member
  lookup and completion flatten through them** — the support these needed. A
  nameless record used to emit no symbol at all, so its fields floated into
  the parent container; giving it a node would have hidden them instead
  (`x.b1` resolving nowhere), so the member walk now recurses into a block
  that *publishes into its owner* — an anonymous `Type`/`Union`, a
  non-`Explicit` `Enum` — which is what ProPgTypeUnion means by "declared
  within the structure that nests it". The boundary is structural, not
  orthographic: a named block is a barrier for its whole subtree, pinned by
  `pQ3` (a named union holding an anonymous type: `y.b` is `error 18`)
  against `pQ4` (anonymous throughout: compiles), with `y.u.b` also
  `error 18` and `print y.a` compiling for a plain nested enum. Completion
  offers the same set, so the two paths cannot disagree. `FreeBASIC.md` §7
  carries the table, §12.20 the one form fbc answers and we do not (`y.e.a`,
  whose chain walk asks each intermediate member for its `as <type>` and an
  enum has none), and §12.21 the bigger find of the wave: **no implicit
  `This`** — a bare `b1` inside `Sub T.proc()` hovers as the procedure's own
  header and offers no member of the type it belongs to, where fbc compiles
  the wiki's example as written. Recorded rather than patched: that fix
  belongs with scope resolution, not the member chain.
- **A code lens no longer anchors on an unnamed declaration.** The old
  guard was the selection's zero width, which the opener-keyword selection
  above would have invalidated — every anonymous enum becoming a permanent
  "0 references" lens. The guard is now `key.empty()`, the same signal.
- Tests: `parser_checks` gains the recursive invariants (non-empty `name`,
  `selection ⊆ range`) over the ProPgTypeUnion nesting, plus access
  inheritance and two-bare-enums-still-do-not-collide; `resolve_checks` gains
  `TestAnonymousBlocksPublishIntoOwner`, pinning both halves of the boundary
  and the completion set that must agree with it; `session_core` gains
  `TestDocumentSymbolsNameAnonymousAndScopes`, which runs the wire reply and
  asserts no `"name":""`, the `<anonymous enum>`, its opener selection, and
  the `if` → `inner` local; `code_lens_checks` gains
  `TestUnnamedDeclarationsCarryNoLens`. A 70-file sweep (`drd/temp/inc/*.bi`
  plus 60 of the compiler's manual examples) reports 0 empty names and 0
  selection overruns, and none of the 19 anonymous enums in `raygui.bi`
  carries a lens.

- **A scope is an outline node only when it directly holds a declaration —
  and then one node deep.** The first cut emitted every block and branch its
  own node, so `engine.bas`'s update loop read `with → if → then → if → else
  → if → then → otherFloor`: six structural levels for two dims. `if` and
  `select` blocks never declare anything themselves (their only children are
  branches), so they are spliced out; a branch survives only when it directly
  declares, and its children lift recursively. The invariant, pinned in the
  converter: splicing moves no declaration — every emitted symbol keeps the
  parent it had.
- **A scope names the construct it belongs to: `if..then <scope>`,
  `if..else <scope>`, `select..case <scope>`.** The parser is the only layer
  that knows which block opened a branch (`openBranchScope` reads the owning
  block's opener), so the label is composed there; the display layer only
  appends the `<scope>` marker. A bare `then` with its `if` spliced away stays
  readable instead of floating with nothing to say where it came from.
- **A member implementation is listed as the source spells its identity:
  `T.proc`, not a bare `proc` under a second `T`.** fbc puts definitions at
  file level, so the outline's file roots used to show two `T`s — the type
  and its implementation masquerading as another `T`. `Symbol::ownerName`
  (source spelling only: `ownerKey` stays the lowercase lookup key, so
  resolution is untouched) qualifies the outline name, and `workspace/symbol`
  carries the same owner as `containerName` — the field Kate renders as
  `T::proc`.
- Tests: `session_core`'s scope test now asserts `if..then <scope>` on the
  wire and gains `TestDocumentSymbolsQualifyMemberImplementations` (the reply
  lists `T.proc` once, the type's bare `proc` declare once, and the
  implementation quoted with its qualifier); `session_type_hierarchy`'s edge
  test asserts the qualified outline name; `session_workspace` gains
  `TestWorkspaceSymbolQualifiesMemberImplementations`, pinning the
  `containerName` pair.

### 2026-10-07 — `Type As` spellings, branch scopes, and the false diagnostics they fanned out into

- **The `Type As` cascade is gone: `raylib.bi` 96 → 0 phantom diagnostics,
  `raygui.bi` 44 → 0, `rlgl.bi` 20 → 0.** One misread fed them all: `Type As
  <type> <name>` (raylib's binding style, a module-scope alias named *after*
  the type) was read as a record whose name was the `as` keyword, so a body no
  `end type` belonged to opened and every statement below parsed as a member
  list — reports multiplied down to EOF on a file that is 141 clean lines. All
  three probed spellings are now handled (fbc 1.10.2): module `Type As <type>
  <name>` registers the alias, nameless `Type As <type>` is `error 14` and
  opens **nothing** (pinned by a stray-closer assertion — a body it did open
  would swallow that closer), and in a record body `Type As <type>` is a
  *field named `type`* while `Type <name> As <type>` is an in-body alias:
  never a member, but it arms `error 238` like a nested record.
  **Lesson: a diagnostic count in the dozens on one file is a single root
  cause wearing N costumes — find the first wrong *block boundary* before
  reading any of the reports past it.**
- **Member capture stopped hunting in type tails.** After a member procedure's
  signature (`declare function f() as const zstring ptr`) and inside an
  alias's parameter list (`type cb as sub(byval a as long)`), the line holds
  type, not members — `ptr` and `byval` were being registered as fields (and,
  armed, dropped with a report). Capture is suppressed on those two tails.
- **Sibling branches are scopes now (probed): `dim p` in a `then` and again
  in its `else` compiles** — twice in one branch is still `error 4`, no
  branch sees a sibling's name across the split (`error 42`), each `case` of
  a `select` re-declares freely, and a branch `Dim` dies at the block's
  closer. Each branch is a `Scope` child under the block's own scope, split
  at `elseif`/`else`/`case` and closed at `end if`; before this, reasings.bi
  reported two false duplicate-`postFix` warnings from two `if` arms sharing
  one symbol table. **Lesson: a false `duplicate definition` means the scope
  granularity is wrong, not the dedupe — the fix is a Scope, not a wider
  key.**
- **A comma's meaning is set by the innermost bracket.** An initializer's
  braces separate *elements*: `{ lgt, lgt, lgt }` used to re-arm the name
  scan per element and register `lgt` three times (rlights.bi, 6 false
  duplicates). Both capture paths now track brace depth alongside paren depth,
  and only a depth-0 comma splits a declaration list.
- **Enum member *lists* parse (fbc-probed):** `a, b, c = 5, d` on one line,
  and a comma at the line end continuing the list on the next — which is how
  rlgl.bi writes its attribute enums, and a trailing comma used to be read as
  a body boundary that cascaded into an unterminated enum, a stray `#endif`,
  and 4 reports. Junk after a name (`a 1`, `a(3)`) still closes the body;
  recorded divergence: only the first name of a multi-name line registers
  (FreeBASIC.md §12.18, a miss — no diagnostic hangs on it).
- **An unnamed declaration has no name to duplicate.** Two bare `enum`s
  collided on the empty key (raylib.bi warned `duplicate definition: ''`
  twenty times); dedupe now skips empty keys. Probing the follow-up
  question — *can* two anonymous enums duplicate? — drew the real line: fbc
  accepts every cross-declaration of the same member (two enums, anonymous
  or not, or beside a module `Dim`) and raises `error 4` only inside one
  block, which still reports here because member keys are per-container and
  never empty; the cross-block refusal comes at the *use* (`error 255:
  Ambiguous symbol access`), which first-match resolution answers silently —
  the `Dim` wins, as fbc does, and the miss is tracked as FreeBASIC.md
  §12.19 with a test pinning the pick.
- Regression tests: 5 new `parser_checks` blocks (all spellings above plus
  branch re-dim and same-branch still-warns) and `resolve_checks`'
  `TestBranchScopesAreSiblings` (sibling branches resolve to *different* Dims,
  cross-branch and post-block uses resolve to nothing). `FreeBASIC.md` §7
  carries the `Type As` table and the enum-list grammar, §8 the branch-scope
  probes. The sweep that found all of this parses the twelve real
  `.bas`/`.bi` files of a user workspace — all now report 0 diagnostics.

### 2026-10-07 — The type half of a field declaration is a chain, not a word

- **`as integer ptr the_data` no longer reports `ptr` and no longer drops
  `the_data`.** The type after `As` is a chain — `As Integer Ptr p`,
  `As Integer Ptr Ptr m`, `As Integer Const Ptr c`, `As Const Integer c`,
  `Dim x As Integer Ptr`, and the type-first list `Dim As Integer Ptr a, b`,
  all probed against fbc 1.10.2 — and both member-capture paths consumed
  exactly *one* word of it, so the next word landed in the field-name
  position. `ptr` is one of the 16 words fbc refuses as a field name, so the
  never-field report fired on the **modifier**, while the field the line
  declares was dropped from completion, hover, and `t.the_data` resolution.
  Fixed at both seams: `skipStatement` (the bare `As` form) and
  `handleVarDecls` (`Dim`/`Const`/`Var`/`Local`/`Redim`), which now skip the
  whole chain while a declared name still follows — or all of it when the
  name came first (`dim x as integer ptr`).
- **The report keeps its anchor where fbc has one.** A chain with no field
  behind it (`as integer ptr`, `error 14: Expected identifier`) still reports
  on the dangling word, and a dangling `Const` (`error 273`) still reports
  too — the rule was always right about the *word* and wrong about the
  position, because the position is chosen by the type skip: when a report
  names a modifier, the type parser upstream of it is what to distrust, not
  the word table. The probe template is the dangling form for exactly this
  reason, and `FreeBASIC.md` §7 now records the chain shape the parser
  encodes (the `kNeverFieldNames` comment attributing `error 273` to
  `as integer ptr` was a typo for `as integer const`, corrected against the
  probe).

### 2026-10-07 — The keyword catalog rounded out, error 238 raised at close, and closer hints for UDT member procedures

- **Twelve missing reserved words join `kReserved` (353 → 365):** `__fastcall`,
  `__thiscall`, `cva_arg`, `cva_copy`, `cva_end`, `cva_start`, `defulng`,
  `dynamic`, `include`, `on`, `option`, `va_first`. The verification note in
  `language.h` now says which check does which job: a diff against fbc's own
  keyword table can *find* an omitted word, while the `dim <word>` probe only
  confirms an entry, so the catalog is closed in both directions now.
- **The member-name tables re-run over the completed catalog**, with the probe
  script fixed to extract underscore spellings it used to drop:
  `tools/probe_member_names.sh` printed **16 never-field, 119 conditional,
  135 enum-illegal, and 230 of 365 enum-legal** — the 112/128/225 the earlier
  wave recorded were counts of a smaller catalog, not of the language.
- **`error 238` is now raised at record-body close.** A record body is armed
  when it reads a trigger — any `Declare`, a member procedure, `Static`,
  `Const`, or a nested record/enum — and a conditional field name is refused
  when the body closes, with the member dropped as fbc drops it. The one
  divergence from fbc is recorded in FreeBASIC.md §12: fbc anchors its report
  on `end type`, this server anchors on the field word, where the rename goes.
  A capture-time-only check could never have been right, because the trigger
  arrives *after* the field; the field is refused at the close that commits it.
- **Eight runtime builtins get catalog rows** (`DylibFree`, `DylibLoad`,
  `DylibSymbol`, `Inp`, `Lpos`, `Out`, `Sleep`, `Wait`; 255 intrinsics total),
  so keyword completion offers the statement/call form and hover shows the
  fbc signature and wiki link instead of falling through to the bare keyword.
- **`function` and `sub` are now the *type* half of `As`**, which is how
  function-pointer fields are spelled: `as function() as integer p` had been
  registering a member named `function` and dropping `p`; fbc builds both forms
  (probed), and the record member path now reads them against `kBuiltinTypes`.
- **No closer hint for a member-procedure declaration inside a record.**
  `constructor()` and `property p()` inside a `Type` end at `end type` — a
  closer sentence there is fbc's `error 19` — while `sub`/`function` bodies
  are real blocks (FreeBASIC.md §12) and keep their `END SUB` / `END FUNCTION`.
  The inlay-hint and code-action surfaces now agree with the parser.
- A whole-catalog wiki-link audit (GET, not the 403'd HEAD) found exactly two
  dead links, and both now point at their real pages: `protected` →
  `KeyPgVisProtected`, and `defulng` → `KeyPgDefulng` (the one `def*` word
  whose page did not exist when the row landed; it has since been created,
  and the `def*` siblings' naming is kept).

### 2026-10-02 — Editor setup docs, and `editors/` moved under `docs/`

- `docs/install.md` — the end-user path: build, install to a prefix, get it on
  `PATH`, and check that it runs. It says plainly that running the binary in a
  terminal prints one line and waits, because that is what a stdio server does,
  and it explains the one install gotcha that is still true — the catalog
  directory is compiled in at configure time, so `--prefix` at install time has
  to match, or `FBLANG_LOCALEDIR` has to point at the catalogs.
- `docs/editors/` — an index plus one page each for Kate, Neovim, Emacs, Helix,
  and Vim. The index carries the feature matrix, because the honest answer to
  "what will I see" is per *client*: the server answers selection ranges, code
  lens, and call/type hierarchy, and four of the five editors have nowhere to
  draw them. Kate and Helix lead with the fewest possible steps (a settings
  dialog and two TOML tables); Vim's page is honest that it is a
  highlighting-only page, because Vim ships no LSP client at all.
- `cmake --install` copies the two generated grammars to
  `<prefix>/share/freebasicd/grammar/`, and `tools/check_install_tree.cmake`
  now checks for them. An installed server whose user has to clone the
  repository to get a syntax file is not an installed server.
- The generated grammars moved from `editors/` to `docs/grammar/`, and the
  README's generic stdio snippet now points at the per-editor pages.

Every claim in those pages was checked against the editor's own source rather
than from memory, and four of them would have been wrong otherwise: Eglot's
semantic tokens and call/type hierarchy exist in Emacs master but in neither 29
nor 30, so the index says "Emacs master only" instead of a flat yes; Kate
advertises the pull-diagnostics capability, which is the one thing that makes
this server stop pushing, and reading the server wrapper alone suggested it
never sent the request — it does, from `LSPDiagnosticProvider::onViewState` in
the plugin view; Helix has no FreeBASIC tree-sitter grammar and therefore cannot
read either grammar file this project generates, so its page opens with that
rather than pretending; and Vim 9 already detects FreeBASIC from file *content*,
falling back to `basic` only for a file too short to look like FreeBASIC. The
generalization: a missing call site is evidence about the files you grepped, not
about the code — grep the whole plugin before writing down what an editor does
not support.

### 2026-10-01 — Which reserved words may name a record or enum member

- The server's whole reserved-word catalog (353 words) was compiled against fbc
  1.10.2 — one `TYPE`, one `ENUM` and one "record with a member procedure" per
  word — by `tools/probe_member_names.sh`, and the answer is three sets, not
  one. `kNeverFieldNames` (16): the binary operators, `New`/`Delete` and the two
  pointer keywords, none of which can name a field in any body; fbc answers
  `error 14: Expected identifier` (`const` is `error 273`, read as a type
  modifier), and no spelling rescues one — not a type suffix, not `ALL CAPS`.
  `kConditionalFieldNames` (112): legal as a field name in a plain record, and
  `error 238` once the body also holds a member procedure, a `Static` field, a
  `Const` or a nested type. And the enum question, which needs no table of its
  own: `enum-illegal(128) == never-field(16) + conditional(112)`, leaving 225 of
  353 legal as an enum member name — the intrinsic and I/O statement words
  (`print`, `stop`, `data`, `input`, …) among them. The two shorter lists are
  the ones encoded, as the static asserts beside them keep them sorted, disjoint
  and inside the catalog. Full tables and provenance: FreeBASIC.md §7.
- Two diagnostics and one fix ride on it. `invalid-member-name` now covers a
  reserved word in the name position of a record field (`as integer and`, and
  the `dim and as integer` spelling, which asked the same question and got no
  answer) and of an enum member. A record's answer is a boundary plus the name;
  an enum's is the boundary it already had **plus** the name, because that
  boundary is load-bearing — it is where a missing `END ENUM` belongs and what
  the `unterminated-block` fix inserts at, so replacing it with fbc's
  stay-open `error 3` would have removed the closer's anchor. The quick fix
  appends `_` (`and_`, `sub_`), which needs no guess: a suffix is not part of
  the word, the same rule that makes `foo` and `foo$` two variables. It is a
  pure function of the diagnostic's own range and refuses a range that is not a
  bare word, since a client can ask for a code action against a buffer that
  moved on.
- **An enum body that is a reserved word stopped being a broken body.** `enum e /
  print / end enum` compiled clean in fbc and used to draw an unterminated enum
  plus a stray closer here — an enum member is `name` or `name = expr`, and the
  tail is judged too (`a 1`, `a(3)` and `print 1` are each fbc's `error 3`).
- **Three findings, none of them about the diagnostic.** (1) A line-leading
  `rem` at end of line lexed as a *keyword*: `isWhitespace` stops at the line
  ends, so `rem note` was a comment and a bare `rem` was not, and the lexer
  contradicted its own comment. That made `enum e / rem / end enum` a
  member-name error where fbc's complaint is `error 256` (the enum is empty) —
  one word, one lexer's intent, one wrong answer. (2) `rem` is not the exception
  to the enum rule the first probe reported it as: a probe body carrying a
  second member cannot tell "this line is a comment" from "this line is a
  member", and the enum *count* is what caught the difference. The rule lost an
  exception. (3) The audit that compared the parser against the transcript for
  all 353 words, on the question "does the parse declare this member", found
  three words fbc creates a member for and the parser dropped: `redim` and
  `local` in an enum body (a var-decl opener swallowed the line) and `as` in
  `as integer as` (a second type-introducer ate the name). A diagnostic wave
  that only asked "is the name legal" would have shipped all three; the member
  is the thing completion and hover offer. `parser_checks` now asks both
  questions for every word in the catalog.
- Deferred and recorded in FreeBASIC.md §12 rather than half-built: `error 238`
  (needs a `static` flag on `Symbol` and an end-of-block answer, not a word
  list) and `error 256` (an empty body is not counted).
- Lesson: **a probe's template is part of its result.** Two of the three
  findings came from a template that was one line short of the language, and
  neither showed up as a wrong count — only as a plausible special case. The
  count is the assertion that catches a template; make the probe print it, and
  make the test pin it.

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