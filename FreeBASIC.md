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

## 1. Identifiers

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
- Max identifier name of a namespace/type can cross modules; normal names are
  module-local unless declared `shared`/`common` (see §8).

## 2. Type suffixes and dialect gating

Suffix chars on identifiers, attached at the end of the bare name:

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

`Boolean`, `Byte/UByte`, `Short/UShort`, `Integer/UInteger`, `Long/ULong`,
`LongInt/ULongInt`, `Single`, `Double`, `String`, `WString`, `ZString`,
`Object`, `Any`, `Pointer`/`Ptr`. `C*`-prefixed numeric-conversion functions
(`CInt`, `CLng`, `CDbl`, `CSng`, `CUInt`, `CLongInt`, `CBool`, `CUByte`, …)
parse their argument as the named type.

## 4. Literals

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

## 8. Scope and visibility

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

Identifier lookup order `(wiki, ProPgIdentifierLookup; partial page)`:
innermost scope → enclosing scopes → current namespace/type, then members →
base types along the `Extends` chain → module (shared/common-shared only in
procedures).

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
   root whenever the resolution site is inside a block. The remaining
   divergence is tier-3 lenient cross-file resolution: a name the requesting
   file's include closure does not declare still resolves to a workspace root
   of the same key (a header that is not included yet). Real fbc would treat
   such a name as an undeclared symbol; we resolve it as a convenience, so
   references/definitions can point outside the closure until the include is
   added.
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
