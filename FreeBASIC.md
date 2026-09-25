# FreeBASIC Language Reference

Single source of truth for FreeBASIC language facts used by this project.
Consolidates the FreeBASIC wiki (`https://www.freebasic.net/wiki/CatPgProgrammer`
and its `ProPg*` pages) plus probe-verified behavior from the system
`fbc` 1.10.2 compiler. Older copies of this knowledge lived in `AGENTS.md` and
in git history (`9fec877` "Record verified FreeBASIC syntax facts"); do not
regress to them — this file wins.

Provenance tags:
- `(wiki)` — from FBWiki `ProPg*` pages; not yet probe-verified.
- `(fbc)` — verified against `fbc` 1.10.2

The keyword catalog (~250 words from `CatPgFullIndex`) is **data in
`src/language.cpp`**, not prose: `kReserved` holds the set, plus per-word wiki
doc URLs and block-closer facts (block closures in §7 are `(fbc)`).

Documentation Table of Contents: https://www.freebasic.net/wiki/DocToc

## 1. Identifiers

Documentation: https://www.freebasic.net/wiki/ProPgIdentifierRules
Documentation: https://www.freebasic.net/wiki/ProPgIdentifierLookup

- Charset `[A-Za-z0-9_]`; first char must be a letter or `_`; a digit is not
  allowed first `(wiki)`.
- **Case-insensitive**. Canonical lookup key = lowercase name, **including**
  any trailing type-suffix char (see §2). `(fbc)` — fbc treats `Foo` and `foo`
  as the same name.
- Length > ~128 chars is truncated `(wiki)` — never rely on the tail of a long
  name.
- `.` is **never** part of an identifier (member access / ellipsis operator);
  periods inside names are allowed only in `qb`/`fblite` `(wiki)`. `@` is the
  address-of operator, not a suffix.
- **Reserved words are valid member names** (and valid type names):
  `type Mytype / as string name / end type` compiles clean under
  `fbc 1.10.2 -w all`, as do `len`, `mid`, `dim`, `tuple`, `erase` as member
  names and `type name` used as a type. `ptr`/`const` are the exceptions —
  fbc rejects them as member names (`error`: "expected member name"). Keyword
  *variable* names stay illegal (`dim name` → fbc error); only the member-name
  slot (and the type-name slot) opens the keyword set. The lexer therefore
  must treat a reserved word as a name in those positions.
- Max identifier name of a namespace/type can cross modules; normal names are
  module-local unless declared `shared`/`common` (see §8).

## 2. Type suffixes and dialect gating

Suffix chars on identifiers, attached at the end of the bare name (only in ):

| suffix | type | sizeof (this linux-x64 `fbc`) |
|--------|------|-------------------------------|
| `$` | String | 24 |
| `%` | **Integer** | 8 |
| `&` | Long | 4 |
| `!` | Single | 4 |
| `#` | Double | 8 |

`(fbc)` — probed in `fblite` via `Dim x%` + `sizeof()`.

- Suffixes are **meaningful only in `qb`/`fblite`/`deprecated` dialects**.
  In `fb` mode fbc 1.10.2 emits
  `warning 44: Suffix ignored in 'x%'` and treats `foo` and `foo$` as the
  **same symbol** (probe: `Dim foo As Integer` + `Dim foo$ As String` →
  warning 44 + `error 4: Duplicated definition, foo`). `(fbc)`
- Default-typed `Dim q` (no `As`) is rejected in `fb`:
  `error 147: Default types or suffixes are only valid in -lang deprecated or
  fblite or qb` `(fbc)`.
- `Integer`/`UInteger`/`LongInt` sizes are **platform-dependent**: on this
  64-bit build Integer=8, Long=4, LongInt=8; on 32-bit targets Integer=4.
  Never hardcode sizes in resolution/semantic code.

## 3. Built-in types

Documentation: https://www.freebasic.net/wiki/CatPgStdDataTypes
Documentation: https://www.freebasic.net/wiki/TblVarTypes

`Boolean`, `Byte/UByte`, `Short/UShort`, `Integer/UInteger`, `Long/ULong`,
`LongInt/ULongInt`, `Single`, `Double`, `String`, `WString`, `ZString`,
`Object`, `Any`, `Pointer`/`Ptr`. `C*`-prefixed numeric-conversion functions
(`CInt`, `CLng`, `CDbl`, `CSng`, `CUInt`, `CLongInt`, `CBool`, `CUByte`, …)
parse their argument as the named type.

## 4. Literals

Documentation: https://www.freebasic.net/wiki/ProPgLiterals

- Integer: decimal; radix prefixes `&H`/`&h` hex, `&O`/`&o` or `&` octal,
  `&B`/`&b` binary `(wiki)`. Integer size suffixes: `%` Integer, `L`/`&` Long,
  `U` UInteger, `UL` ULong, `LL` LongInt, `ULL` ULongInt `(wiki)`.
- Float: digits with `.` and/or exponent `E`/`D`; default is Double; suffix
  `!`/`F` = Single, `#`/`D` = Double. `1.5`, `1e-5` `(wiki)`.
- String: `"..."` with doubled `""` as an escaped quote `(fbc)`; `!"..."`
  escaped strings (C-style `\""`, `\n`, `\t`, …); `$"..."` literal strings
  where `""` is *not* a quote escape `(wiki)`.
- `...` is the variadic-parameter marker; `?` is a `PRINT` shortcut.

## 5. Comments and metacommands

Documentation: https://www.freebasic.net/wiki/ProPgComments
Documentation: https://www.freebasic.net/wiki/CatPgCompilerSwitches

- Single-line: `'` to EOL and `REM` to EOL (a statement, usable mid-line after
  code/`:`). `'` inside a string is not a comment `(fbc)`.
- Multi-line: `/' ... '/`, **nestable and paired** — matches at the innermost
  balance, then the next `'/` closes the opener `(fbc)`; probe `mlc.bas` ran
  printing `ab`/`c`/`after`, `mlc2.bas` errored on the text left outside the
  balanced pair.
- Metacommands are `$`-keywords written as the first tokens *of a comment*:
  `'$LANG: "qb"`, `rem $LANG: "qb"`, `$DYNAMIC`, `$STATIC`, `$INCLUDE`…
  A bare `$` statement is a syntax error; a leading `''` (two quotes)
  suppresses metacommand parsing in the comment `(wiki)`.
- Doc comments: `/''` (multi-line comment start) and `''` lines directly above a declaration → hover
  text. **This is this server's convention, not a FreeBASIC language feature.**
  `''` in fb really only means "may not hold a metacommand".

## 6. Lines, continuation, separators, labels

Documentation: https://www.freebasic.net/wiki/ProPgLabels
Documentation: https://www.freebasic.net/wiki/ProPgLineContinuation
Documentation: https://www.freebasic.net/wiki/ProPgLineSeparator

- Line continuation: trailing `_` (whitespace-tolerant), must not follow an
  identifier/word without a space. Statements split on `:`. `$`/`#` lines
  cannot continue.
- Line labels: identifier followed by `:` at statement position — `GOTO`/
  `GOSUB` targets; distinct from inline `:` statement separators.
- `.bas`/`.bi` sources may open with a UTF-8 BOM, or UTF-16/32 LE/BE BOM
  `(wiki)`.

## 7. Blocks and closers

Keyword blocks closed by `END <keyword>` — `SUB/FUNCTION/PROPERTY/OPERATOR/
CONSTRUCTOR/DESTRUCTOR ... END <same>`, `TYPE/UNION/ENUM ... END <same>`,
`NAMESPACE ... END NAMESPACE` (**no `MODULE` keyword**), `SCOPE ... END SCOPE`,
`IF ... END IF`, `SELECT CASE ... END SELECT`, `WITH ... END WITH`,
`EXTERN ... END EXTERN`, `ASM ... END ASM`. `(fbc)`; enforced as data in
`language.cpp`.

Non-`END` closures: `FOR ... NEXT` (closed by `NEXT`, no `END FOR`);
`WHILE ... WEND` (**`WEND` only** — `END WHILE` is rejected by fbc);
`DO ... LOOP`; preprocessor `#IF..#ENDIF` and `#MACRO..#ENDMACRO`.

- `END` **alone** or with a numeric parameter is the END statement (terminate program), not a closer.
- Single-line `IF...THEN` takes no closer.
- `EXIT`/`CONTINUE` take a block-target keyword (`EXIT FOR`, `CONTINUE DO`…).

### Record and enum bodies (`TYPE` / `UNION` / `ENUM`)

Documentation: https://www.freebasic.net/wiki/CatPgUserDefTypes

A record or enum body is a **member list, not a statement list** — the only
things its grammar accepts are a member declaration, a nested record/enum, or
an access section. A procedure body is the opposite: it accepts any statement.
That asymmetry is why a missing closer's correct position is block-kind
dependent — see "Where a missing closer belongs" below.

Legal `TYPE`/`UNION` body members `(fbc)`:

| member | example |
|---|---|
| field, `Dim` **optional** | `x As Single` / `Dim x As Single` |
| static field | `Static s As Integer` |
| type constant | `Const c = 1` |
| member procedure declaration | `Declare Sub`, `Declare Function`, `Declare Constructor`, … |
| access section | `Public:` / `Private:` / `Protected:` (next subsection) |
| nested record or enum | `Type … End Type`, `Union … End Union`, `Enum … End Enum` |

`Dim` is optional because "variables are created in UDTs much the same way
variables are created normally, except that the Dim keyword is optional"
`(wiki)`. Both spellings declare a field fbc resolves through `p.member`
`(fbc)`.

Legal `ENUM` body members: `name`, `name = expr`, `name(…) = expr`.

Anything else in the body is a hard error, and fbc anchors the missing closer
on the offending statement `(fbc)`:

| body | statement | fbc |
|---|---|---|
| TYPE/UNION | `y = 1.5` | `error 17: Syntax error, found '=' in 'y = 1.5'` + `error 19: Expected 'END TYPE' or 'END UNION' in 'y = 1.5'` |
| TYPE/UNION | `print 1` | `error 17: Syntax error in 'print 1'` |
| TYPE/UNION | `sub foo()` | `error 17: Syntax error, found 'foo' in 'sub foo()'` |
| TYPE/UNION | `if 1 then` | `error 17: Syntax error in 'if 1 then'` |
| TYPE/UNION | `dim shared g As Integer` | `error 17: Syntax error, found 'g'` |
| TYPE/UNION | `redim preserve q(3)` | `error 63: Expected array, found 'q'` |
| TYPE/UNION | `foo()` | `error 9: Expected expression, found ')'` |
| ENUM | `dim c As Integer` | `error 3: Expected End-of-Line` + `error 74: Expected 'END ENUM'` |
| ENUM | `sub foo()` | `error 3: Expected End-of-Line, found 'sub'` + `error 74: Expected 'END ENUM', found 'sub' in 'end sub'` |

`Field = n` (field alignment) and `Extends t` are **opener-line** modifiers, not
body members: `type t / Field = 4 / x As Single / End Type` is `error 17:
Syntax error, found '=' in 'field = 4'`, while `type t Field = 4` and
`type b Extends a` both compile `(fbc)`.

**A record must declare at least one data field** `(fbc)`. `Type`/`Union`/`Enum`
with no field is `error 256: An ENUM, TYPE or UNION cannot be empty`, and
**none** of a nested record/enum, a `Declare`d member procedure, an access
section, a `Const`, or a `Static` field counts toward it — only a plain data
field does. So a record whose body is *only* nested types is rejected, while
the same nesting plus one field compiles.

**A field's declared type must already be declared** — there are no forward type
references. `type a / b As b / End Type` written before the later
`type b / … / End Type` is `error 14: Expected identifier, found 'b' in
'b as b'` (the `Dim` spelling fails identically). A field naming a different,
already-declared type is fine: `type a / x As Single / End Type` then
`type b / Dim q As a / End Type` compiles `(fbc)`.

**By-value recursion is illegal** `(fbc)`: a field whose type is the type
enclosing it is `error 88: Recursive TYPE or UNION not allowed`. `Ptr` is the
workaround, and an array does not help — inside `type point`, `Dim p As Point`
and `Dim p As Point(10)` both give `error 88`, while `Dim p As Point Ptr`
compiles. Note the corollary, because it matters for error recovery: since
`Dim p As Point` inside `type point` can never be a field, a buffer that
contains one cannot be read as an unclosed record whose body continues past it.

#### Where a missing closer belongs

fbc ends a record/enum body at the **first statement the body grammar cannot
accept** and names that statement in the message (`error 19` / `error 74`
above); likewise it ends a control block at the first closer that does not
match (`error 13: Expected 'NEXT', found 'end'` for a `FOR` met by `end sub`,
`error 125: Expected 'END SUB' in '<stmt>'` for a procedure met by a statement
that is illegal inside it) `(fbc)`. So the closer belongs immediately **before**
that statement, not at the end of the file. Procedure bodies are the exception
that proves the rule: their grammar accepts everything, so there the end of the
buffer really is the best available guess. See §12.15 for what this parser does
today.

### Access sections

Documentation: https://www.freebasic.net/wiki/KeyPgVisPrivate
Documentation: https://www.freebasic.net/wiki/KeyPgVisPublic
Documentation: https://www.freebasic.net/wiki/KeyPgVisProtected

`Public:` / `Private:` / `Protected:` are valid **only inside a `TYPE` body**
`(fbc)`. fbc rejects them everywhere else: a `Union` body gives `error 17:
Syntax error, found 'public' in 'public:'`, an `Enum` body `error 3: Expected
End-of-Line, found 'public'`, module level `error 17: Syntax error, found ':'
in 'public:'`, and inside a procedure `error 61: Illegal inside functions,
found 'private'`. The wiki records them as "new to FreeBASIC" and "available
only in the `-lang fb` dialect" `(wiki, see §11)`.

A section gates every member declaration after it until the next section, and
members are **`Public:` by default** when no section has been seen `(wiki)` —
`type t / n As Integer / End Type` then `v.n` compiles `(fbc)`. Reaching a
non-public member from outside the type is `error 202: Illegal member access,
found 'nome' in 'print v.nome'` `(fbc)`. The wiki puts the permitted scope as
"only from inside a member procedure of their Type or Class" (plus, for
`Protected`, "classes which are derived from this Type or Class"), and adds
that "seen from inside such a member procedure, it is as if the protected
member is in fact public … regardless of the object on which the access
operator is applied" `(wiki)`. Whether a *derived* type's ordinary code (as
opposed to its member procedures) may read an inherited `Protected` member was
not probed here; treat the wiki sentence as the source until it is.

## 8. Scope and visibility

Documentation: https://www.freebasic.net/wiki/ProPgVariableScope

Four storage categories `(wiki)`, visibility probe-verified `(fbc)`:

| declaration | visible where |
|-------------|---------------|
| `Dim` (local, innermost block) | declaring block + nested blocks only; **not** outside it |
| `Dim` at module level (no `Shared`) | module-level code at/after the declaration, incl. `SCOPE` blocks; **NOT inside procedures** |
| `Dim Shared` at module level | everywhere in the module, **including procedures** |
| `Common name` at module level | other modules at module level; **NOT inside procedures** |
| `Common Shared name` at module level | everywhere, including procedures in all modules |

Probe results:
- `Dim total` + procedure write → `error 42: Variable not declared, total`
  (`vis.bas`); with `Dim Shared` the program ran and printed `2` (`vis2.bas`).
- `Common Shared m` visible in a `Sub` → printed `5` (`mcs.bas`).
- `Dim outer` at module level is visible inside a `SCOPE` block; `Dim inner`
  inside the `SCOPE` is **not** visible after it → `error 42: Variable not
  declared, inner` (`sc.bas`).

So: **scope blocks nest and inherit; procedure bodies do not see module-level
plain `Dim`/`Common`** — only `Shared`/`Common Shared` module names plus their
own locals and enclosing-in-procedure block names.

Type-member visibility (`Public:`/`Private:`/`Protected:`) is *not* in this
table — it is a record-body construct, documented under §7 Access sections.

Identifier lookup order `(wiki, ProPgIdentifierLookup; partial page)`:
innermost scope → enclosing scopes → current namespace/type, then members →
base types along the `Extends` chain → module (shared/common-shared only in
procedures).

### Enums (`Enum ... End Enum`)

Documentation: https://www.freebasic.net/wiki/KeyPgEnum

An enum declares a type *name* and a set of constant *members* (module
scope). Being constants, members resolve like module-level `Const` — visible
at module level, inside procedures, inside `SCOPE` blocks, and (from a
header) in every includer. Probe-verified with fbc 1.10.2:

- Plain enum members are ordinary module-scope constants: bare `member`
  compiles at module level (`plain1.bas`), inside procedures (`plain2.bas`),
  inside `SCOPE` blocks (`sc1.bas`), and from included headers
  (`cross.bas`).
- `Enum <name> Explicit` gates each member behind qualified `Name.member`
  access: bare `member` compiles nowhere (`explicit2.bas`; `cross2.bas`
  shows the gate holds for header enums too), while qualified access
  compiles for both forms (`explicit1.bas`, `qual1.bas`).
- The enum *name* is a module-level type; qualified `Name.member` is the
  only reference valid for explicit enums and optional for plain ones.
- An enum name may itself be a reserved word — `enum color` compiles even
  though `color` is the graphics intrinsic, and `color.green` is a normal
  qualified access (`qual1.bas`).
- `Explicit` is reserved globally: `dim explicit` → `error 4`, and a bare
  `print explicit` → `error 3` (`resv1.bas`, `resv2.bas`).
- Module declaration order applies to members: a bare usage *before* the
  `Enum` block is `error 42` (`order1.bas`), and a module `Dim green`
  shadowing an enum member `green` wins regardless of which block came
  first (`order2.bas`, `order3.bas` both print 2).
- What may appear in an `Enum` body, and where a missing `End Enum` belongs,
  is in §7 (Record and enum bodies).

## 9. Module model

- A program is one or more `.bas` files; the first file is the main module
  (`-m name` overrides). `.bi` files are headers pulled in **textually** via
  `#include`.
- Cross-module sharing exists only at module scope, and only for
  `shared`/`common`/`common shared` names. Procedure-local names never cross a
  file boundary.
- `#include` inserts the header at the point of the directive: the header
  sees the includer's module-level `shared` declarations made *before* it, and
  the includer sees the header's module-level declarations from the inclusion
  point onward.
- Once-guards: `#include once`, `#pragma once`, or the classic
  `#ifndef guard / #define guard / #endif` pattern.
- Paths accept both `/` and `\`; identifier/name case sensitivity follows the
  host filesystem when opening files `(wiki)` (the compiler itself is
  case-insensitive).

## 10. Preprocessor and macros

Documentation: https://www.freebasic.net/wiki/CatPgPreProcess

- Preprocessor lines start with `#` at line start (not continuable):
  `#include [once]`, `#inclib`, `#define`, `#undef`, `#if/#elseif/#else/
  #endif`, `#ifdef/#ifndef`, `#assert`, `#error`, `#lang`, `#libpath`, `#line`,
  `#pragma`, `#cmdline`, `#print`, `#macro/#endmacro` `(wiki)`.
- `#define` names (and macros) are scoped: visible from the definition to the
  end of the block/file; `namespace` does **not** affect define visibility
  `(wiki)`.
- Macros: `#define id(params) body`; arguments substituted unmodified; `##`
  concatenates adjacent tokens. Multi-line bodies use `#macro` `(wiki)`.

### Conditional compilation

- `#if`/`#elseif`/`#else`/`#endif` over numeric compile-time constants
  (arithmetic/comparison/`and`/`or`/`not`), including `#define`'d values and
  built-in `__FB_*` compilation constants; skipped branches are not parsed for
  syntax `(wiki, ProPgConditionalCompilation)`.

## 11. Dialects

Documentation: https://www.freebasic.net/wiki/CompilerDialects

- Dialects: `fb` (default), `fblite`, `qb`, `deprecated`. **This server
  implements only `fb`**; other dialects get a best-effort `fb` parse plus one
  `lang-mode` Information diagnostic.
- Set via the `#lang "name"` directive (recommendation: before the first
  declaration) or the `$lang` metacommand (`'$lang: "qb"` / `rem $lang: "qb"`).
  `$lang` overrides `-lang` on the command line but is ignored (with a warning)
  under `-forcelang`; `-forcelang` is a compiler flag, never visible in source.
  The parser records the active dialect word in `ParseResult.lang`. `(fbc)`
- Dialect differences that matter to the lexer/parser:
  - identifiers with periods (qb/fblite; §1), suffix types (§2),
    default-typed `Dim q` (§2), `LongInt` not accepted in `qb`
    (probe: `error 14: Expected identifier`), QB 64-K `String` limits, legacy
    control-flow syntax — all gated to non-`fb` dialects.
  - Type access sections (`Public:`/`Private:`/`Protected:`) are "available
    only in the `-lang fb` dialect" `(wiki, §7 Access sections)`.
  - Deftype directives (`DEFINT`/`DEFLNG`/`DEFSNG`/`DEFSTR`/`DEFBYTE`/…)
    set the implicit default type per first letter; suffix throws override
    `DEFxxx`. Without `Option Explicit`, undeclared-but-referenced variables
    are implicitly declared `(wiki, ProPgImplicitdeclarations)`.

## 12. Known divergences in this implementation (as of now)

Deliberate model simplifications / bugs, tracked here so docs never drift back
to "the language is what the lexer does":

1. **Suffix identity in `fb` mode.** The symbol model keys identifiers with
   their suffix (`foo$` ≠ `foo`), correct for qb/fblite but wrong for `fb`,
   where fbc 1.10.2 ignores the suffix (warning 44) and aliases both to one
   symbol. The parser records the dialect; the resolve/session layer must
   collapse suffix-distinct keys in `fb`.
2. **Module-level visibility: fixed at M7, one cross-file leniency remains.**
   `dim`-kind module declarations are only visible to module-level code; inside
   procedures only `Shared` module declarations (plus `Const` and locals)
   resolve (§8, probed: fbc error 42). Since M7 the parser tags module-level
   `Shared` var declarations (`Symbol.shared`), and
   `declAt`/`visibleSymbols`/`resolveAcross` skip a plain module `Dim`/`Common`
   root whenever the resolution site is inside a **procedure body** — its
   control blocks included, and control blocks at module level
   (SCOPE/IF/FOR/...) inherit module scope, so plain module dims stay visible
   inside them (probe: `Dim outer` at module level is visible inside a `SCOPE`
   block). The remaining
   divergence is tier-3 lenient cross-file resolution: a name the requesting
   file's include closure does not declare still resolves to a workspace root
   of the same key (a header that is not included yet). Real fbc would treat
   such a name as an undeclared symbol; we resolve it as a convenience, so
   references/definitions can point outside the closure until the include is
   added. The member-access type lookup (`findTypeDecl`, used by `expr.member`
   hover/definition through the base variable's and each intermediate member's
   declared type) takes the same tier-3 `byKey` fallback, so a member chain
   keeps resolving when its type's header sits outside the requesting file's
   closure.
3. **Multi-line comments (`/' ... '/`) are not lexed** (nestable, §5). Lexer
   must treat them as comment tokens before real-world `.bas` files parse
   cleanly.
4. **`''` doc comments** are our LSP convention; document as such (§5), never
   as a language feature.
5. **Type sizes** must not be hardcoded (§2): Integer=8 here, 4 on 32-bit.
6. **Include-once divergence.** The M5 include graph treats a header as
   included once per *path*, closing diamonds and cycles by visited set.
   Real `fbc` runs `#include`/`#include once`/`#pragma once`/`#ifndef` guard
   macros faithfully, so a path reached twice under different guard states can
   legally be processed twice (and `#ifndef`-guarded headers can self-include
   to form an include-once guard). We do not evaluate guard macros yet; a
   self-include is treated as a cycle and terminated like `#include once`.
   Since M6, `#pragma once` and `#include once` are recorded as metadata
   (`IndexedFile.pragmaOnce`, the edge's `once` flag) but not enforced.
7. **`.name`/`..name` shadow escape hatch is unmodeled** (wiki KeyPgDim,
   "Differences from QB" / dialect notes): a block-local variable shadowing a
   module global can be referenced with a dot prefix (`.SomeSymbol`, and in a
   `With` block `..SomeSymbol`). The lexer treats `.` as member access and
   resolution always picks the innermost scope, so "shadowing wins" is
   absolute in this implementation. Correct for code that does not use the
   prefix; recorded so resolution never silently "fixes" the divergence.
8. **Inlay-hint inferred types come from the identifier suffix even in `fb`
   mode** (§2 suffix table), where fbc ignores suffixes (warning 44). The
   `dim x$` → `As String` hint is **cosmetic display only** and has no effect
   on resolution.
9. **No `DEFINT`-family default-type inference for `dim x`.** A bare `dim x`
   (no suffix, no `AS`) gets no inferred-type hint; the QB-only default-type
   machinery (§2 `error 147` in `fb`) is out of scope.
10. **Semantic-token `range` is served only to clients that request it** via
    `textDocument.semanticTokens.requests.range`; other clients fall back to
    `full`. This is standard LSP behavior (the server never advertises a
    provider a client cannot call), not a server limitation.
11. **Vim block-comment `/'...'/` nesting is approximated** (vim region
    semantics); the generated TextMate grammar nests via `begin/end` pairs.
    Grammar-side limitation only.
12. **Module-level declaration ordering is unmodeled — enum members resolve
    order-insensitively.** fbc injects enum member constants at the `Enum`
    block's position in the module: a bare usage *before* the block is
    `error 42` (§8 Enums, `order1.bas`), and a colliding module `Dim`
    shadows the member from the `Dim` onward (`order2.bas`, `order3.bas`,
    both print 2). This implementation resolves enum members as
    order-insensitive module candidates everywhere, and module roots
    (Dims/Consts/procedures) always beat a same-named plain-enum member, so
    the shadowing outcome matches fbc when the `Dim` follows the member,
    while pre-`Enum` bare usages resolve here as a convenience instead of
    erroring (declared decision: no source-order gating at module level).
13. **`dim T.m` static-member syntax captures a phantom variable.** In `fb`,
    `dim map.m as integer` is `error 147` (fbc 1.10.2: "Default types or
    suffixes are only valid in -lang deprecated or fblite or qb, found
    '.'"). The dim handler registers the name token before the dot (`map`)
    with the full `.m` text surviving in its signature, so the fixed-array
    spelling can never legally appear under `-lang fb` — the phantom module
    var is harmless, but recorded so docs never claim the syntax is
    supported. (The qb/fblite reading of `.` as a type suffix is the same
    hazard as divergence #1; only `fb` is implemented.)
14. **Type and variable names share one key, so a same-named `dim` loses to
    its type in member completion.** FreeBASIC keys identifiers
    case-insensitively but keeps type and variable names in separate
    namespaces: fbc 1.10.2 compiles `dim position as Position` (probe) and a
    later `position.x` uses the *variable*. This implementation's
    module-scope resolution returns the first same-key root, so the earlier
    `Type` root wins and an M19 `position.` completion resolves as if the
    type were the base (empty member set). The M19 fixtures sidestep the
    collision with a distinct variable name (`dim p as Position`), and
    static-member completion (`T.counter`) is out of scope entirely — type
    names never complete their members today.
15. **A record body swallows the rest of the file until its `END` is typed.**
    §7 (Record and enum bodies) records that a record body is a member list,
    not a statement list, and that fbc ends the body at the first statement it
    cannot accept — naming that statement in `error 19` / `error 74`. This
    parser has no member-grammar check: while a `Type`/`Union`/`Enum` block is
    open it treats *every* line as a potential member, so after a deleted
    `End Type` the rest of the file is parsed inside the record. The visible
    damage is not just a missing closer: module-level code after the record is
    captured as its fields, so a later use of the same name raises a spurious
    `duplicate-definition`, and the record's `blockRanges` entry (folding) runs
    to end-of-source. For `blocks_type.bas` with `end type` deleted, `dim p as
    point` is captured as a field of `point` and `duplicate-definition: 'p'`
    is published on the following line. It also makes the `unterminated-block`
    quick fix append its closer at end-of-buffer, since that is the only
    insertion point the parse exposes. Fixed by adding the member-grammar
    predicate to `language.cpp` and having the parser record each block's
    logical end; until then, treat a record-body boundary as unmodelled.
