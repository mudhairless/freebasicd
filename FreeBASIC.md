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
- `(src)` — read out of the compiler's own source at tag `1.10.2` (§13)

The keyword catalog (~250 words from `CatPgFullIndex`) is **data in
`src/language.cpp`**, not prose: `kReserved` holds the set, plus per-word wiki
doc URLs and block-closer facts (block closures in §7 are `(fbc)`).

The compiler's own source is the authority behind every `error N` / `warning N`
code quoted in this file — §13 says where the catalog lives, how the numbering
works, and how to check a code against its text without installing anything.
The compiler includes numerous example of syntax and general usage under
the `~/Projects/freebasic/compiler/examples` directory. You should checkout a tagged
version release matching the installed version to use as newer versions/master
branch may support new features.

Documentation Table of Contents: https://www.freebasic.net/wiki/DocToc

An offline copy of the wiki is available in the directory: 
`~/Projects/freebasic/compiler/doc/manual/markdown` 
with the file DocToc.md being equivalent to the online table of contents. All pages
from the online wiki should be present as markdown files in this directory.
This offline copy is for your convenience and should not be expected to be available
on end users machines.

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

Suffix chars on identifiers, attached at the end of the bare name (only in the
`qb`/`fblite`/`deprecated` dialects):

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
- The suffix the modern default mode *does* have is the **numeric-literal** one,
  C-style: `100ul` is a `ULong` (sizeof 4 on this build), `1.5f` a `Single`
  (§4). Identifier suffixes and numeric-literal suffixes are separate features
  of separate dialects — `fb` has the latter only.
- Default-typed `Dim q` (no `As`) is rejected in `fb`:
  `error 147: Default types or suffixes are only valid in -lang deprecated or
  fblite or qb` `(fbc)`. The converse is the initializer: `Dim a As Integer = 5`
  is `error 146: Only valid in -lang fb or deprecated or fblite` in `qb`. The
  two gates are **disjoint**, so `Dim a = 5` is rejected by every dialect — §11.
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
`language.cpp`. **This list of opener words is complete** — note in particular
that there is **no `CLASS ... END CLASS`**: `class` is a reserved word with no
construct behind it, because the class-ness folded into the type system (§11).

Complete as a set of *words*, but not uniform in *when* each word opens a block.
The six procedure keywords — `SUB`, `FUNCTION`, `PROPERTY`, `OPERATOR`,
`CONSTRUCTOR`, `DESTRUCTOR` — open a block only in **implementation form**. In a
`TYPE`/`UNION` body the same six words are member *declarations*, and they take
**no closer**: the body ends at the next member or at `END TYPE`.

```freebasic
type t
  declare constructor()
  declare property p as integer
  x as integer
end type
```

That compiles `(fbc)`, and the same body with a closer after either declaration
does not: `declare constructor()` / `end constructor` is `error 19: Expected
'END TYPE' or 'END UNION', found 'constructor' in 'end constructor'`, and
`declare sub go()` / `end sub` fails the same way `(fbc)`. That is not a special
rule about `END` — it is the body-grammar rule below (a record body ends at the
first statement it cannot accept), so `end constructor` is read as the *end of
the record body*, not as a member closer. A member procedure spelled with its
body (`sub go()` / … / `end sub` inside the record) is a third shape again: fbc
answers `error 17: found 'go'`, and this parser accepts it deliberately
(§12.15).

So the same word opens a block, declares a member, or is refused depending on
the body it sits in, which is the one case a table keyed on the opener word
alone cannot answer.

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

A member procedure declaration is the one member that is **not** a block: it
carries a signature and no body, so there is no `END SUB` to write and none is
accepted — see the six-keyword note at the top of this section. It pairs with a
module-level implementation (see "Inheritance (`Extends`) and
member-procedure implementation" below).

`Dim` is optional because "variables are created in UDTs much the same way
variables are created normally, except that the Dim keyword is optional"
`(wiki)`. Both spellings declare a field fbc resolves through `p.member`
`(fbc)`.

The type half of a field declaration is a **chain**, not one word: a type
followed by any number of `Ptr`/`Pointer` modifiers, with `Const` allowed
inside the chain (fbc reads it as a modifier that must be followed by one) —
`As Integer Ptr the_data`, `As Integer Ptr Ptr m`, `As Integer Const Ptr c`,
`As Const Integer c`, `As Udt Ptr u`, `Dim x As Integer Ptr`, and the
type-first list `Dim As Integer Ptr a, b` (one type, several names) all
compile `(fbc)`. The field name is what *follows* the chain, so a `Ptr` in
that position is never the name; with nothing behind it the chain is
incomplete and fbc anchors `error 14: Expected identifier` on the dangling
word (`As Integer Const` alone is `error 273`, wanting its pointer).

`Type` itself has three non-body spellings, all probed `(fbc 1.10.2)` — none
of them opens a body, and only `Type <name>` (± `Extends`) ever does:

| where | spelling | meaning |
|---|---|---|
| module/`Extern` | `Type As <type> <name>` | alias whose name comes **after** the type — raylib's binding style, `Type As rAudioBuffer rAudioBuffer_`; the name is the last identifier at paren depth 0 past the `As` |
| module/`Extern` | `Type As <type>` (no name) | `error 14: Expected identifier` — declares nothing, opens nothing |
| record body | `Type As <type>` | a **field named `type`** (`Type As ulong` in a plain record compiles, and it is a conditional field name like any other — `error 238` once the body is armed); nothing may follow the type, `Type As Integer x` is `error 3` |
| record body | `Type <name> As <type>` | an alias *inside* the record: **not a member** (`t.f1` is `error 18: Element not defined`), but visible to the type's own methods (`Dim q As f1` inside one compiles, at module scope it does not), and it **arms** `error 238` the way a nested record does |

Reading the `As` as a name instead opens a record body no `end type` belongs
to, and every statement below it then parses as a member list — that single
misread is what produced ~96 phantom diagnostics in a real binding header
(`drd/temp/inc/raylib.bi`), most of them anchored at EOF.

Legal `ENUM` body members: `name`, `name = expr`, and comma-separated **lists**
of them — `a, b, c = 5, d` on one line compiles, and a comma at the line end
continues the list on the next line (probed; the shipped raylib `rlgl.bi`
writes its attribute enums that way, which is why a trailing comma must not
close the body). The array-shaped form is not one of them: `a(1) = 1` is
`error 3: Expected End-of-Line`, so an enumerator cannot carry a subscript.

The same member may be *declared* in two different enums — anonymous or
not — and beside a module-level `Dim`/`Const` as well: every such combination
compiles `(fbc)`. The refusal comes at the **use**: naming unqualified a
member that two enums declare (anonymous vs anonymous, named vs named alike)
is `error 255: Ambiguous symbol access, explicit scope resolution required
for <enum>.X, <enum>.X`. A module `Dim` next to an anonymous enum's member
is not ambiguous at all — the `Dim` wins and prints its own value.

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
| TYPE/UNION | `dim p As Point` in `type point` | `error 88: Recursive TYPE or UNION not allowed` |
| ENUM | `dim c As Integer` | `error 3: Expected End-of-Line` + `error 74: Expected 'END ENUM'` |
| ENUM | `sub foo()` | `error 3: Expected End-of-Line, found 'sub'` + `error 74: Expected 'END ENUM', found 'sub' in 'end sub'` |

A field may be **named after a reserved word** (`Next As Node Ptr` — the canonical
linked list — and `End As Integer` both compile `(fbc)`), which is why a record
body must not read the leading word as a closer. Only when the type *also* holds
member functions does fbc object: `error 238: Fields cannot be named as keywords
in TYPE's that contain member functions or in CLASS'es` `(fbc)`.

#### Which reserved words may name a member (probed, all 365)

Every word in the server's reserved-word catalog was compiled against fbc
1.10.2, one minimal program per body kind per word, by
`tools/probe_member_names.sh` — the tables in `src/language.cpp` are that
script's output, and the static asserts beside them keep them sorted, disjoint,
and inside the catalog. The template is one field per line after a required
`first_field` (fbc rejects an empty UDT with `error 256`, which would make every
word look rejected for the wrong reason), and the answers are three, not two:

| question | answer | fbc |
|---|---|---|
| never a `TYPE`/`UNION` field name | **16** — `and andalso const delete eqv imp mod new not or orelse pointer ptr shl shr xor` | `error 14: Expected identifier` (`const` is `error 273: Expected 'PTR' or 'POINTER'`, read as a type modifier) |
| legal as a field name in a *plain* record, refused once the body also holds a member procedure, a `Static` field, a `Const`, or a nested type/enum | **119** — the rest of the catalog, minus those 16 | `error 238` (a plain `Dim` field and an access section do **not** arm it) |
| never a legal `ENUM` member name | **135** — exactly those 16 + those 119 | `error 3: Expected End-of-Line` |

So the enum question needs no table of its own:
`enum-illegal(135) == never-field(16) + conditional(119)`, which leaves **230**
of 365 legal as an enum member name — the intrinsic and I/O statement words
(`print`, `stop`, `data`, `input`, `line`, `put`, `get`, …) among them. No
spelling rescues any of the 16: a type suffix (`and$`, `and%`) and `ALL CAPS`
fail identically.

`rem` belongs to the second row, and not because it is special: fbc reads a
line-leading `rem` as a **comment**, so no member named `rem` is ever created and
an enum holding nothing else is `error 256`. A probe whose body carried a second
member could not tell that apart from a legal name and reported `rem` as the one
word accepted as an enum member yet refused under `error 238` — the enum *count*
is what caught it. `rem_` is a real member either way, and `Rem` mid-line
(`as integer rem`) is a field name fbc accepts, because the comment rule only
applies at the start of a line.

`Field = n` (field alignment) and `Extends t` are **opener-line** modifiers, not
body members: `type t / Field = 4 / x As Single / End Type` is `error 17:
Syntax error, found '=' in 'field = 4'`, while `type t Field = 4` and
`type b Extends a` both compile `(fbc)`.

**A record must declare at least one data field** `(fbc)`. `Type`/`Union`/`Enum`
with no field is `error 256: An ENUM, TYPE or UNION cannot be empty`, and
**none** of a nested record/enum, a `Declare`d member procedure, an access
section, a `Const`, or a `Static` field counts toward it — only a plain data
field does. So a record whose body is *only* nested types is rejected, while
the same nesting plus one field compiles. **Not implemented** (§12, item 17).

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
§12.15 records what this parser does with that statement.

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
today, and for the two exceptions it makes (a member procedure spelled with its
body, and a keyword field name in a type holding member functions).

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

### Inheritance (`Extends`) and member-procedure implementation

Documentation: https://www.freebasic.net/wiki/KeyPgExtends

`Extends` on the opener line is FreeBASIC's **only** inheritance form, and that
is worth stating because the obvious alternatives are not FreeBASIC at all
`(fbc 1.10.2, all probed)`:

| construct | fbc |
|---|---|
| `type b Extends a` | compiles |
| `union u Extends a` | compiles |
| `type b : a` | `error 17: Syntax error in 'type b : a'` — and the same under `-lang qb` |
| `interface i … End Interface` | `error 42: Variable not declared, interface` — **there is no `interface` keyword in the language** |

So a type has **at most one base**, and there are no interfaces to model: no
multiple inheritance to reconcile, and nothing that is not a single parent chain
per type. Chains can be arbitrarily deep (`type c Extends b` where `b Extends
a`), and an inherited field and an inherited member call both resolve through
any number of levels `(fbc)`. `Extends object` is the same edge — `Object` is a
keyword and this is how a UDT gets a VMT. **There are no forward base
references**: the base must already be declared, exactly like a field's declared
type (see the record bodies section above), so a base that fails to resolve is
a defect rather than a form to answer for.

A **member procedure is declared inside the type and defined at module level,
qualified by the type name**. A definition inside the type body is a hard
error — `type t / n As Integer / Sub go() / End Sub / End Type` is `error 17:
Syntax error, found 'go' in 'sub go()'` plus `error 33: Illegal 'END'` `(fbc)`
— so `Type.name` at module level is not a stylistic choice but the only
spelling. It may sit in the includer `.bas` or in a sibling header of the same
`#include` closure, both probed `(fbc)`:

```fb
type t
  n as integer
  declare sub go()
end type
sub t.go()     ' module level, qualified — in the includer .bas or a sibling
end sub        ' header of the same #include closure
```

A member procedure declared `Static` may additionally be a **module
constructor** — the `Constructor` keyword goes on the module-level definition,
not on the `Declare` line, and a non-`Static` member sub may not (§9 Module
constructors and destructors).

A derived type may **not** re-implement an inherited member: `type d Extends t`
followed by `sub d.go()` is `error 158: Declaration outside the original
namespace or class in 'sub d.go()'` `(fbc)`. The declared→implemented edge is
therefore a **function, never a fan-out** — at most one implementation answers
for one declaration, which is a far smaller thing to answer than an override
graph. UDTs additionally cannot have member operators: `operator + (o as t) as
t` inside a `TYPE` body is `error 17: Syntax error, found '+'` `(fbc)`.

Constructor and destructor are the one place the qualifier is **not** a dot:
`declare constructor()` in the type pairs with `constructor t()` at module
level, and both spellings compile `(fbc)`. See §12.16 for why the parser does
not act on that.

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

Each **branch** inside a control block is its own scope, not merely the block
(probed `(fbc 1.10.2)`):

- `Dim p` in an `If` `Then` branch and again in its `Else` (or `ElseIf`)
  compiles — twice in the *same* branch is still `error 4: Duplicated
  definition`.
- No branch sees a sibling's name across the split: `Print p` in `Else` when
  only `Then` declared `p` is `error 42: Variable not declared, p`.
- The same holds per `Case` of a `Select Case`: each `Case` re-declares the
  name freely, and a `Case`-local `Dim` is gone after `End Select`.
- A one-line `If ... Then Dim p` scopes too: `p` is `error 42` after it.

The implementation models this as a `Scope` child per branch under the block's
own scope (siblings, so the parent walk from one branch never reaches
another).

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
  (`-m name` overrides — the name **without** `.bas`; see below). `.bi` files
  are headers pulled in **textually** via `#include`.
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

### Module-level executable code

Module level is not a neutral namespace: it holds statements, and **which `main`
they belong to decides what runs and when** `(fbc, probed)`.

- **The main module's module level *is* an implicit `main`.** Its statements run
  top-down in source order (`ord1.bas` prints `A`, calls, prints `B`), and `End`
  halts the program (`order.bas` prints `start` and nothing after `End`). The
  same module level also declares the program's globals — module-level
  `Dim`/`Const` — so it is a function body *and* a declaration space at once.
  A single-file program with any number of headers is exactly this shape.
- **No forward declaration inside the module.** A module-level call to a
  `Sub`/`Function` defined *later in the same file* is
  `error 42: Variable not declared, Later` (`fwd.bas`, `fwd2.bas`) — the
  implicit main sees only what is declared above the call, which is §8's
  declaration-order rule, not a module-model rule. `Declare` is the fix, and it
  is the only fix across files too: `m1h.bas` `#include`s a header carrying
  `Declare Function Helper() As Integer` and links the definition in a second
  file (`helper.bi` + `m2.bas`, prints `main got 42`).
- **Locals are not inherited in either direction.** Module-level names live in
  the implicit main, so a procedure it calls sees only `Shared`/`Common Shared`
  module names (§8) — never the main module's plain module-level `Dim`.
- **Every *other* module's module-level code runs too — before `main`.** fbc
  accepts it silently (nothing under `-w all`) and emits it as a load-time
  constructor, so a second file's top-level statements print *ahead of* the main
  module's first line: `fbc ord1.bas ord2.bas` gives `X: second module
  top-level`, then `A: main module, before call`. Swap the command line and
  `ord2` becomes main — so its statements now belong to `main` and run last
  (`ord2.bas ord1.bas` gives `A`, `Y`, `B`, then `X`). One rule, both orderings:
  **the main module's module level is `main`; every other module's is a
  constructor that precedes it.**
- **A library has no `main`, so the same code is a constructor there too.** A
  `-lib` module with module-level statements compiles clean — `rc=0`, no
  diagnostic under `-w all` (`lib.bas`) — `nm` on the archive shows them as
  `fb_ctor__lib`, and an executable linking that object runs them before its own
  `main` (`uselib.bas` + `liblib.a` prints the library's line first). A shared
  library behaves identically: `-dylib` builds it — the flag is **`-dylib`**,
  and the earlier `-dynlib` probe here was a misspelling, not a missing feature
  — the module-level statements survive, and a linking executable prints the
  library's line first again (`lmod.bas` + `app.bas`: `[lib] module-level ran`,
  then `[app] main module-level`). The emitted symbols say why: a `-dylib`
  object exports **no `main`** — the first input module's level becomes
  `__fb_DllMain_ctor` and the later ones `fb_ctor__<name>`, all local
  constructors — so **a library has no process entry point**. Its module-level
  code is entirely load-time initialization, and fbc's `-m` default (the first
  input `.bas`) still chooses which module becomes the library's own init. The
  Shared Libraries page calls that module's code "a main code"; it is a load
  hook, not a process `main`.
- **The default main module is the first `.bas` on the command line.** fbc's own
  `--help`: `-m <name> Specify main module (default if not -c: first input
  .bas)`. So which module is `main` is a property of the *link command line*,
  not of any file — invisible in the sources, and invisible to an LSP. The
  swap-the-command-line probe above is this rule seen from the other side.
- **`-m` takes the module name without `.bas`.** `-m ord1` makes `ord1` the main
  module even though it is listed second on the command line;
  `-m ord1.bas` compiles every file but then fails to link —
  `undefined reference to 'main'`.

### Module constructors and destructors

Documentation: https://www.freebasic.net/wiki/KeyPgModuleConstructor
Documentation: https://www.freebasic.net/wiki/KeyPgModuleDestructor

The **explicit** form of "run before `main`". It is a procedure marked at its
`Sub` definition, outside any type:

```
[Public|Private] Sub name() Constructor [priority]   ' body
End Sub
```

- **A constructor runs before its own module's module-level code**, and all
  constructors run before the main module's `main` — whichever module each is in
  (`ctor.bas`: `Constructor1() called`, `Constructor2() called`,
  `module-level code`; `kmain.bas ka.bas kb.bas` puts the main module's own
  constructor ahead of everything).
- **A constructor is a procedure body, so §8 applies to it.** A plain
  module-level `Dim` is *invisible* inside one — `error 42: Variable not
  declared, g` on a write (`kplain.bas`), and the same on a read (`ktot.bas`) —
  while `Dim Shared` and `Const` are visible (`ktot2.bas` prints
  `ctor: shared_total=42 K=7`). Initializing a module global from a constructor
  therefore requires `Shared`.
- **Order across constructors is unspecified, and it is not even stable across
  link orders.** Three modules, one constructor and one body line each, built
  two ways (`ka/kb/kmain`):

  - main module `kmain`: `main ctor`, `A ctor`, `A body`, `B ctor`,
    `B body`, `main body`
  - main module `kb`: `B ctor`, `A ctor`, `A body`, `main ctor`, `main body`,
    `B body`

  So a non-main module's module-level body can run *between* other modules'
  constructors, and the main module's constructor can run *after* a non-main
  module's body. Within one module this fbc emits constructors in definition
  order and destructors in reverse (`ctor.bas`) — the wiki's own example shows
  the reverse constructor order, and states both are permitted
  `(wiki)`. Nothing may depend on either.
- **`priority` is the only ordering control**: an integer **101–65535**, where
  101 is highest and the value means nothing except relative to other
  constructors that also carry one. Probed: 101 → first, 65535 → last, and an
  unprioritized constructor ran after both (`prio.bas`). Outside the range →
  `error 189: Invalid priority attribute` for `100` and for `65536`
  (`priolo.bas`, `priohi.bas`).
- **The parameter list must be empty** — `Sub Bad(x As Integer) Constructor` is
  `error 1: Argument count mismatch, before 'Constructor'` (`ctorparam.bas`).
  Consequently at most one constructor may exist in a set of overloads, since
  every overload of a 0-arg `Sub` would collide.
- **`Constructor`/`Destructor` are forbidden on a declaration line** —
  `Declare Sub Init() Constructor` is `error 3: Expected End-of-Line` in both
  the plain (`ctordecl.bas`) and UDT-member (`udtctor.bas`) spellings. Mark the
  **definition**.
- **A `Static` member procedure of a UDT can be one**: `Declare Static Sub
  Init()` in the type, `Sub Widget.Init() Constructor` at the definition
  (`udtctor3.bas` compiles and prints `widget static init` before
  `module-level code`). A non-`Static` member sub is `error 17: Syntax error`
  (`udtctor2.bas`).
- **Name clashes are a link error across modules**: two `Public` constructors
  named `Init` in different files give `multiple definition of 'INIT'` — the
  emitted symbol is the uppercased name. `Private` constructors still run
  (`privctor.bas` runs both).
- **Destructors** (`Sub name() Destructor`) mirror this at exit, in no
  guaranteed order either (`ctor.bas`: `Destructor2()` then `Destructor1()`,
  after the module-level code and before the process ends).
- Static globals whose initial value is computable at compile time are
  initialized before *any* code runs, so they — unlike a plain module `Dim` —
  are reliably readable from a constructor `(wiki)`. The wiki's standing advice
  follows from the ordering table: prefer one constructor that explicitly calls
  the other modules' init procedures over relying on constructor order.

### Exported symbols (`Export`)

Documentation: https://www.freebasic.net/wiki/KeyPgExport
Documentation: https://www.freebasic.net/wiki/ProPgSharedLibraries

`Export` marks a procedure for a **shared library's export table**, so another
program can bind it — statically (`#inclib` + `Declare`) or at runtime
(`DyLibSymbol`). It is not a visibility rule like `Public`; it is a request that
survives only as far as the compiler and linker allow.

- **It is a definition-line specifier, parsed with the constructor keywords.**
  `Sub name() Export`, forbidden on a `Declare` — `Declare Sub f() Export` is
  `error 3: Expected End-of-Line`, the same code `Constructor` gives on a
  declaration line (`decl_exp.bas`), and the same parser loop:
  `parser-proc.bas` reads `Export`, then `select case( tk ) case
  FB_TK_CONSTRUCTOR`. It also *implies* `Public` — the parser sets
  `FB_SYMBATTRIB_EXPORT or FB_SYMBATTRIB_PUBLIC` in one statement. On a plain
  exe it compiles and runs unchanged (`exeexp.bas`).
- **Its effect is target- and link-dependent** `(src)`. The export table is
  emitted only when the `-export` command-line option is given **and** the
  target carries `FB_TARGETOPT_EXPORT` (`emit_x86.bas`). In `fb.bas`'s
  `targetinfo`, **win32 and cygwin** have that flag and **linux does not**;
  where it is emitted the emitter writes a ` -export:<name>` directive into a
  COFF directive section, i.e. a linker directive. On linux, `-export` instead
  appends `--export-dynamic` to the link line (`fbc.bas`) — and a `-dylib`
  build gets that whether or not `-export` was passed, because an ELF shared
  object's symbols are already in its dynamic table.
- **So on Linux, `Export` looks like a no-op, and a Linux-only probe cannot see
  the difference.** A `-dylib` module whose sub is plain `Public` with no
  `Export` still shows `T <NAME>` under `nm -D` (`lmod.bas`). That is the
  platform's default, not `Export` working, and it must not be carried to
  Windows, where the COFF directive is what puts the name in the export table.
- **The catalog's `CANNOTEXPORT` warning never fires in 1.10.2.** "Cannot export
  symbol without -export option" sits at warning level 2 in `error.bas`, but its
  only call site is commented out (`parser-proc.bas`). `Export` without
  `-export` compiles clean under `-w all`, for an exe and a `-dylib` alike
  (`exp.bas`).
- **An executable can export too** — `Export` plus `-export` at link time, for
  symbols another shared library needs when it loads. `-export` has no extra
  effect with `-dylib`/`-dll` `(wiki)`.
- Dialect and platform notes `(wiki)`: not available in `-lang qb` except under
  the alias `__Export`, and no effect on DOS DXEs, where every `Public`
  procedure is exported anyway.

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
  The parser records the active dialect word in `ParseResult.lang`. `#lang` is
  **module level only** — inside a procedure it is `error 61: Illegal inside
  functions, found 'qb' in '#lang "qb"'` `(fbc, probed)`, not a dialect message.
- Dialect differences that matter to the lexer/parser:
  - identifiers with periods (qb/fblite; §1), suffix types (§2),
    default-typed `Dim q` (§2), `LongInt` not accepted in `qb`
    (probe: `error 14: Expected identifier`), QB 64-K `String` limits, legacy
    control-flow syntax — all gated to non-`fb` dialects.
  - Type access sections (`Public:`/`Private:`/`Protected:`) are "available
    only in the `-lang fb` dialect" `(wiki, §7 Access sections)`.
  - Deftype directives (`DEFINT`/`DEFLNG`/`DEFSNG`/`DEFSTR`/`DEFBYTE`/…)
    set the implicit default type per first letter; suffix throws override
    `DEFxxx` `(wiki)`.
  - **Implicit variable declaration does not exist in `fb`** `(fbc, probed)`:
    a bare undeclared name is `error 42: Variable not declared,
    undeclaredThing` (`implicit.bas`), and there is no keyword that switches
    implicitness on — `Option Explicit` is *itself* rejected in this dialect,
    `error 146: Only valid in -lang deprecated or fblite or qb, found 'Option'`
    (`optex.bas`). Implicitness is the *other* dialects' behavior
    `(wiki, ProPgImplicitdeclarations)`, and note what `Option Explicit` does
    there: it turns implicitness **off**, so `-lang qb` + `Option Explicit`
    gives error 42 exactly as `fb` does with no directive at all.

### How dialect gating actually works (fbc source, all of this probed)

Dialect behaviour is **not** scattered `if lang = qb` tests. It is one 23-bit
feature mask per dialect: `enum FB_LANG_OPT` (`fb.bi:329-356`) plus the
`langTb()` table (`fb.bas:31-105`), tested through the one-line macro
`#define fbLangOptIsSet( op ) ((env.lang.opt and (op)) <> 0)` (`fb.bi:622`).
Because the test is a bitmask, the whole dialect story is one table:

| bits set in | bits (and what they allow) |
|---|---|
| all four | `QUIRKFUNC` |
| `fb` only | `OPEROVL`, `CLASS`, `AUTOVAR`, `SINGERRLINE` |
| `fb`, `deprecated` | `SCOPE` |
| `fb`, `deprecated`, `fblite` | `MT`, `NAMESPC`, `EXTERN`, `FUNCOVL`, `INITIALIZER` |
| all but `fb` | `CALL`, `LET`, `PERIODS`, `NUMLABEL`, `IMPLICIT`, `DEFTYPE`, `SUFFIX`, `METACMD`, `OPTION`, `ONERROR` |
| all but `fb` and `deprecated` | `GOSUB` |
| **no dialect** | `ALWAYSOVL` — read at `symb-proc.bas:909,932`, set by none |
| set by all four, read by none | `QUIRKFUNC` — dead the other way |

Three **layers** produce three different diagnostics, which is why
"dialect-gate the check" is not a sufficient instruction:

1. **bit + keyword present** → `errReportNotAllowed` (`error.bas:810`) reports
   `error 146` (`ONLYVALIDINLANG`, the default argument) or a named
   `*ONLYVALIDINLANG` message. There are **32** such call sites, all in the
   parser phase and all lexically determined — none needs type resolution, so a
   dialect check is a mask test and not a semantic pass. Distribution of the
   32: **27** take the default and report 146, 3 report
   `147 DEFTYPEONLYVALIDINLANG` (`parser-decl-proc-params.bas:416`,
   `parser-decl-var.bas:1339`, `parser-proc.bas:1499`), 1 reports
   `150 AUTOVARONLYVALIDINLANG` (`parser-decl-var.bas:2161`), and 1 is the
   uncalled `hErrSuffix()` (§13).
2. **keyword absent in that dialect** → a **syntax error**, not 146. The
   keyword table `kwdTb` (`symb-keyword.bas`, 247 rows) carries per-word flags,
   applied in `symbKeywordInit` (`symb-keyword.bas:285-321`):
   `KWD_OPTION_NO_QB` (**90** rows) renames the plain spelling to `__name` in
   `qb`, so it becomes an ordinary identifier there — `scope = 1` compiles clean
   under `-lang qb`, and gives `error 10: Expected '='` while
   `error 146: Only valid in -lang fb or deprecated, found 'scope'` under
   `-lang fblite` `(both probed)`. So in `qb` the 90 `NO_QB` words (`Scope`,
   `Namespace`, `Extern`, `Overload`, `Class`, `Union`, `Constructor`,
   `Destructor`, `Property`, `Operator`, `Private`, `Public`, `Var`, `New`,
   `Delete`, `LongInt`, `Shl`, `Shr`, …) are identifiers: `Dim true As
   Integer` is `error 4: Duplicated definition, true` in `fb` and compiles
   clean in `qb` `(probed)`. Three more names are renamed by hand rather than
   through `kwdTb`, for the same effect: `True`/`False` are literals spelled
   `__true`/`__false` in `qb` (`symb-keyword.bas:381,393`), and `cva_list` is a
   typedef aliased `__cva_list` (`:454`) — so unlike the other two it is not
   reserved, and `Dim cva_list As Integer` compiles in both dialects.
   `KWD_OPTION_STRSUFFIX` (**12**: `STRING`, `STR`, `MKD`, `MKS`, `MKI`, `MKL`,
   `MID`, `RTRIM`, `LTRIM`, `LCASE`, `UCASE`, `CHR`) — the `$`-suffixed spelling
   exists only in `qb`. `KWD_OPTION_QB_ONLY` (**1** row: the `SCREENQB` quirk
   keyword; `SCREEN` has a *second*, `NO_QB` row, so it is reserved only in
   `qb`). Everything else — `Gosub`, `Return`, `Let`, `Option`, `LPrint`,
   `DefInt`, `On`, `Error`, `Seek`, `Resume`, `Data`, `Restore`, `Width`,
   `Palette`, `Window` — is a plain keyword in **all four** dialects, so its
   *statement* is gated by a bit and gives 146, not a syntax error.
3. **silent semantic difference** → **nothing at all**, which is the layer a
   diagnostic cannot show. About 42 direct dialect comparisons (25 `fbLangIsSet`
   + 17 inline `env.clopt.lang <>`), dominated by QB-vs-the-rest: the QB type
   remap (`fb.bas:414-448`, `integerkeyworddtype` Integer vs **Short**,
   `int16literaldtype` UInteger, `floatliteraldtype` **Single** — note this is
   `lang <> qb`, so `deprecated` and `fblite` keep fb's typing), and **builtin
   availability**: the `rtl*.bas` intrinsic tables carry per-entry option flags
   consumed at `rtl.bas:120-142`. Four of them gate a builtin on the dialect —
   `NOQB` (135 entries; in `qb` the name is re-prefixed `__`, `rtl.bas:240-245`,
   so the builtin silently vanishes), `QBONLY` (11), `FBONLY` (4), `NOFB` (3),
   the last three being direct `env.clopt.lang = FB_LANG_…` tests. `MT` (13) is
   the fifth dialect gate and the only one routed through a feature bit
   (`fbLangOptIsSet( FB_LANG_OPT_MT )`, `rtl.bas:122`). `STRSUFFIX` (30) is
   **not** a dialect gate despite the name: it sets `FB_SYMBATTRIB_SUFFIXED`
   (`rtl.bas:232-234`), i.e. the builtin has a `$`-suffixed spelling, so §2's
   `$`-rule reaches the *library* too. `OVER` (135), `ERROR` (13), `X86ONLY`,
   `32BIT`/`64BIT`, `NOGCC`, `ASSERTONLY` and `CANBECLONED` are non-dialect.
   So roughly 135 of the rtlib's entries simply do not exist in a `qb` buffer,
   with no diagnostic until the compile fails.

`SINGERRLINE` is a *delivery policy* wearing a dialect bit: it is fb-only, and
it decides whether a diagnostic quotes the offending line (`error.bas:565-568`).

### `DEFTYPE` and `INITIALIZER` are disjoint — so no dialect allows `Dim a = 5`

`DEFTYPE` allows a declaration with no `As` clause; `INITIALIZER` allows
`Dim … = expr`. `fb` has only the second, `qb` only the first, and
`deprecated`/`fblite` have both — so the *modern* spelling is the only one `fb`
accepts, and the *implicit-type* spelling is the only one `qb` accepts. Probed
in all four dialects:

| source | `fb` | `deprecated` | `fblite` | `qb` |
|---|---|---|---|---|
| `Dim a As Integer` | ok | ok | ok | ok |
| `Dim a` (no `As`) | **147** | ok | ok | ok |
| `Dim a(10)` (no `As`) | **147** | ok | ok | ok |
| `Sub s(x)` (untyped param) | **147** | ok | ok | ok |
| `Dim a As Integer = 5` | ok | ok | ok | **146** |
| `Dim a = 5` (no `As`) | **147** | ok | ok | **146** |
| `var v = 1` | ok | **150** | **150** | **10** |

`(fbc, probed)`. Reading the numbers: 147 = `DEFTYPE` off (fb only), 146 here =
`INITIALIZER` off (qb only), 150 = `AUTOVAR` off (`var`, which is also a
`NO_QB` keyword, so in `qb` it is not even a keyword and the failure is layer 2,
`error 10`). Two details worth not getting wrong: **`Dim a = 5` is illegal in
every dialect** — `fb` rejects the untyped name, `qb` rejects the initializer —
so "`Dim x = 5` needs `Dim x As Integer = 5` in modern FB" is not a style
preference; and the token `146` quotes is the `=`, not the declaration
(`error 146: Only valid in -lang fb or deprecated or fblite, found '='`).

### `class` is a reserved word with no construct behind it

`class` **is** in the keyword catalog (`symb-keyword.bas:152`, and in
`language.cpp`'s `kReserved` / `kConditionalFieldNames`), but there is **no
`class … end class`** in FreeBASIC — the class-ness folded into the type system
and is spelled `type … end type` plus member procedures and `Virtual` (§7;
`Virtual` needs the type to `Extends Object`, else `error 221: Method declared
VIRTUAL, but UDT does not extend OBJECT` `(probed)`). Probed in all four
dialects: `class c / n as integer / end class` gives `error 3: Expected
End-of-Line, found 'class'` (`error 10` in `qb`) and then cascades. The bit is
`FB_LANG_OPT_CLASS`, and it gates **member procedures inside a Type**, not a
type declaration — `parser-decl-struct.bas:62` (a `Declare` member) and
`parser-proc.bas:1857,1868,1886` (`Constructor`, `Destructor`, `Property`).
`CLASS` is set for `fb` only, so those three give `error 146: Only valid in
-lang fb, found 'declare'` in all three legacy dialects `(probed)`. Two
consequences: never treat `class` as a type-declaration opener when extending
§7, and treat any future `class` keyword as a *breaking* change to this section.

The class-ness survives only in **prose**, which is the strongest available
evidence that this is settled rather than merely unfinished: **12 of the 328
catalog messages** say "CLASS" while there is no `class` construct —
`error 238: Fields cannot be named as keywords in TYPE's that contain member
functions or in CLASS'es`, `error 158: Declaration outside the original
namespace or class`, `error 160: Expected class or UDT identifier`,
`error 168: Parent is not a class or UDT`, `error 183: TYPE or CLASS has no
default constructor`, `error 265: Symbol not a CLASS, ENUM, TYPE or UNION type`,
`error 270: COMMON variables cannot be object instances of CLASS/TYPE's with
cons/destructors`, `error 293: Not extending a TYPE/UNION`,
`error 294: Illegal outside a CLASS, TYPE or UNION method`,
`error 295: CLASS, TYPE or UNION not derived`, `error 296: CLASS, TYPE or UNION
has no constructor` (one of the 18 dead numbers, §13), and `error 299: Expected
a CLASS, TYPE or UNION symbol type`. Every one means "`Type`", and a quoting
tool must reproduce the text verbatim rather than normalize it — so a message
that says `CLASS'es` is not a hint that a `CLASS` block exists. Where §7 already
says "a Type or Class" it is quoting this tradition, not asserting a second
construct.

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
15. **A record body's closer is placed from a member-grammar check; one
    statement fbc refuses is still read as a member.** §7 (Record and enum
    bodies) records that a record body is a member list, not a statement list,
    and that fbc ends the body at the first statement it cannot accept — naming
    that statement in `error 19` / `error 74`. The parser asks
    `acceptsBodyMember` (`language.cpp`) and closes the body at the first
    statement the record/enum grammar refuses, then re-parses that statement in
    the enclosing scope, so the rest of the file is no longer captured as fields
    and the `unterminated-block` diagnostic carries the offset its quick fix
    inserts at. A closer that does not match is the same evidence from the other
    side, and is treated the same way. A **by-value self-reference** (`Dim p As
    Point` inside `type point`) is such a statement — `error 88`, and it can never
    be a field — with two forms exempt, because they have no per-instance storage
    of their own and fbc accepts both: `ptr` (the documented workaround, also the
    reason `Next As Node Ptr` is a field rather than a boundary, §7) and `static`.
    An array dimension does not exempt it (`Dim p As Point(10)` is still `error
    88`), and neither does a type suffix, which `fb` ignores (warning 44). One
    statement fbc refuses is still accepted, on purpose:
    - A member procedure spelled **with its body** inside the record body
      (`type t` / `sub go()` / … / `end sub` / `end type`), where fbc wants
      `Declare Sub go()` plus a module-level definition and answers `error 17:
      found 'go'`. Hover, call hierarchy and code lens resolve inside such a
      member, so a boundary there would close a body the author plainly means and
      re-attribute that member's own members to the record. The cost: a record
      left unclosed before one is still reported at end-of-buffer, as is a keyword
      field name in a type that *does* hold member functions (`error 238`, which
      fbc only raises there).
16. **A constructor/destructor's type owner is unmodelled** (§7, Inheritance).
    The member-procedure implementation edge is recorded for the three forms
    that carry an unambiguous `Type.name` qualifier — `sub t.go()`,
    `function t.val()`, `property t.p` — and the parser keys those roots by the
    *member*, so `sub t.go()` no longer claims the type's key. A
    constructor/destructor is deliberately **not** re-keyed: its implementation
    is spelled `constructor t()` with no dot, and `constructor name()` at module
    level is also how a *module* constructor is declared (fbc accepts both
    `(fbc)`), so the two are indistinguishable in the parser without resolving
    the name against a type. A wrong guess would rewrite a real module-level
    declaration's key, which is worse than having no edge, so `constructor t()`
    stays a root named `t` — meaning it still collides with `type t` in the
    same way `sub t.go()` used to. Cost: no go-to-implementation for a UDT
    constructor or destructor, and a phantom `documentSymbol`/code-lens entry
    for that one form.
17. **`error 256` is not raised; `error 238` is raised at close, not at the
    field.** Two fbc checks on a record body are both probed in §7 ("Which
    reserved words may name a member"):
    - `error 238` — a keyword field name in a body that also holds a member
      procedure, a `Static` field, a `Const`, or a nested type. Now raised:
      the body is armed when a trigger is read (any `Declare`, a body-formed
      member procedure, `Static`, `Const`, or a nested record/enum) and the
      119 conditional words
      (`kConditionalFieldNames`) are refused when the body closes, with the
      member dropped as fbc drops it. Two details diverge: fbc anchors its
      report on `end type` and always says "member functions"; this server
      anchors on the field word, where the rename goes, and keeps fbc's
      wording so the two stay searchable together. The 16 words fbc refuses
      outright remain diagnosed at capture.
    - `error 256` — a `Type`/`Union`/`Enum` body with no plain data field. The
      body-level count that would raise it is not computed. This one has a
      visible consequence for `rem`: a line-leading `rem` is a comment, so
      `enum e / rem / end enum` declares no member here and draws no diagnostic
      where fbc answers `error 256`.
18. **Only the first name of a multi-name enum-member line registers.** §7
    records that `a, b, c = 5, d` and a trailing-comma continuation are legal
    enum member lists, and the parser now accepts them (no boundary, no false
    unterminated block), but the symbol capture behind it keeps one name per
    line: `b`, `c`, `d` above are missing from completion/hover while `a`
    resolves. A miss, not a false report — no user-visible diagnostic hangs
    on it — tracked because a test pins the accepting half.
19. **An ambiguous enum-member use resolves silently to the first candidate.**
    §7 records that fbc accepts every cross-*declaration* combination of enum
    members — two enums, anonymous or not, sharing a member; one beside a
    module `Dim` — and refuses the *use* instead: `error 255: Ambiguous
    symbol access` when two enums declare the name being used unqualified.
    This implementation's resolution is first-match, so it picks one enum's
    member and carries on (hover/goto land somewhere real rather than
    nowhere). The `Dim`-shadows-anonymous-member case matches fbc: the `Dim`
    wins here too. Raising error 255 needs a diagnostic computed at resolve
    time against two live candidates — parse-time diagnostics cannot see a
    use site at all — so it is unimplemented rather than wrong, and a test
    pins the pick.

## 13. Where the diagnostics come from (compiler source)

Every `error N` / `warning N` in this file is a real fbc message, and fbc's
**entire catalog is data in its own source** — no install, no probe, no guess
needed to check one `(src)`.

Source: the compiler is FreeBASIC itself. `src/compiler/` (173 files),
`src/rtlib/` and `src/gfxlib2/` are C. A local copy of the compiler is located 
at `~/Projects/freebasic/compiler`, do not modify repo and only look at a checkout
of a tagged release matching the installed version.

- **Repository**: `https://github.com/freebasic/fbc`. Read at tag **`1.10.2`** =
  commit `8e1023e3`, the release matching the system `fbc` this file is probed
  against. There is a later `1.10.3`; it is *not* what the
  local compiler is, and the 25/25 message-code match below is the evidence that
  `1.10.2` is the right pin. The tag's commit is dated 2023-12-28 while the
  installed binary reports a 2026-09-27 build, so an `fbc` built from this tag
  may differ in ways the catalog does not show.
- **The catalog**: `src/compiler/error.bas`, as two arrays —
  `errorMsgs` (328 entries) and `warningMsgs` (49 entries).
- **Numbering = position.** Both are 1-based `dim shared` arrays, so the *n*th
  entry is message `n`. Verified 25/25 against codes printed by the local
  compiler, including every one quoted in this file.
- **Warning tuples are `(level, text)`**, where `level` is compared against the
  `-w` threshold — *not* the message number. Levels: 4 at 0, 29 at 1, 12 at 2,
  4 at 3. The test is `errReportWarnEx`:
  `if( warningMsgs(msgnum).level < env.clopt.warninglevel ) then exit sub`,
  against `FB_WARNINGMSGS_DEFAULT_LEVEL = 1` (`error.bi:393-395`). A warning is
  shown **iff `level >= warninglevel`**, so the level is a floor and a *higher*
  `-w N` shows *fewer* warnings. The four `level 0` warnings —
  `CONSTQUALIFIERDISCARDED`, `RETURNTYPEMISMATCH`, `CALLINGCONVMISMATCH`,
  `ARGCNTMISMATCH` — are therefore **off by default** and need `-w all`; the 45
  at level ≥ 1 need no opt-in. `-w all` = 0 (everything), `-w none` = 4 (nothing
  exists at 4), `-w N` = level ≥ N. `-w constness` and `-w funcptr` set a
  pedantic flag *and* drop the level to 0, so they reveal the level-0 group;
  `-w param`, `-w escape`, `-w next`, `-w signedness`, `-w suffix`, `-w error`
  and `-w pedantic` leave the level alone. Probed with fbc's own
  `tests/warnings/ptr-callconv.bas`: with no `-w` it prints
  `warning 4(2): Suspicious pointer assignment` only; `-w all` adds
  `warning 42(0): Calling convention mismatch in function pointer`. Note the
  printed shape `warning <number>(<level>)` — the parenthesised number is the
  level, not a second message number.
- **Some catalog texts are prefixes.** `errReportNotAllowed` appends the dialect
  list at report time, so the stored strings are `"Only valid in -lang"` for
  `error 146` and `"Default types or suffixes are only valid in -lang"` for
  `error 147` — the `deprecated or fblite or qb` tail is not in the array.
  Quoting these needs the reporter, not just the catalog.
- **Reporters** — eight, all in `error.bas`: `errReport`, `errReportEx`,
  `errReportWarn`, `errReportWarnEx`, `errReportNotAllowed`, `errReportParam`,
  `errReportParamWarn`, `errReportUndef`. That list is **not** enough to locate a
  call site, because only one shape puts a number next to the reporter: seven
  indirect shapes exist, and in each the constant is *not on the reporting line*.
  A declared **default argument** (`errReportNotAllowed`'s `errnum` defaults to
  `FB_ERRMSG_ONLYVALIDINLANG`, so every one-argument call still reports
  `error 146`); a **pass-through wrapper** sub (31 of them — `hParamError` alone
  reaches 11 messages); a `byref` **out-param assigned by the callee**
  (`symbCalcProcMatch` sets 5 `OVERRIDE*` errors); a **`#macro` body**
  (`hExitError` is report + `hSkipStmt( )` + `return`); a local **chosen by a
  `select case`** over the statement token and reported once at the end
  (`cCompStmtGetTOS`); the sub's **return value** (`astNewCONV` ends
  `return FB_ERRMSG_CASTDERIVEDPTRFROMINCOMPATIBLE`); and a message carried as a
  **parameter's default** (`byval msgnum as FB_ERRMSG = FB_ERRMSG_ILLEGALPARAMSPECAT`).
- **18 error entries are dead** — the *only* occurrence of each name anywhere in
  the 1.10.2 tree (`src/compiler`, `tests/`, `inc/`, `src/rtlib`) is its own
  catalog line, so these are **reserved numbers fbc cannot print**:
  `2 EXPECTEDEOF`, `38 INNERPROCNOTALLOWED`, `39 EXPECTEDENDSUBORFUNCT`,
  `43 VARIABLEREQUIRED`, `46 PROCNOTDECLARED`, `56 ARRAYALREADYDIMENSIONED`,
  `57 ILLEGALRESUMEERROR`, `70 FORWARDREFNOTALLOWED`, `80 MACROTEXTTOOLONG`,
  `93 MISSINGCMDOPTION`, `97 CANTPASSUDTRESULTBYREF`, `102 CANTINITDYNAMICFIELDS`,
  `103 BRANCHTOBLOCKWITHLOCALVARS`, `138 PARAMORRESULTMUSTBEANUDT`,
  `139 SAMEPARAMETERTYPES`, `262 INVALIDINITIALIZER`, `296 CLASSWITHOUTCTOR`,
  `321 INCOMPATIBLEREFINIT`. Every one of the 49 warnings is referenced. Two
  entries a naive reference audit also flags are in fact **live**:
  `133 TOOMANYERRORS` is raised by `errReportEx` itself (`error.bas:626`), and
  `146 ONLYVALIDINLANG` is never passed explicitly — it *is* the default
  argument, so the 27 one-argument `errReportNotAllowed` calls report it (the
  other 5 call sites pass a specific `*ONLYVALIDINLANG` message). A bare
  substring search is worse than useless: `EXPECTEDEOF` matches the unrelated
  `ERROR_SXS_XML_E_UNEXPECTEDEOF` in `inc/win/`, and a search for
  `ONLYVALIDINLANG` finds five *different* `*ONLYVALIDINLANG` constants.
- **Two further entries are *referenced but unreachable*** — a second, distinct
  kind of dead number, and the one a reference audit is blind to by
  construction. Both are `*ONLYVALIDINLANG` messages, both are named at a call
  site, and neither can ever be printed:
  - `148 SUFFIXONLYVALIDINLANG` ("Suffixes are only valid in -lang") — its only
    site is the macro `#define hErrSuffix()` at `lex.bas:2567`, which is
    **defined and never called**. All three `LEXCHECK_POST_*` paths in
    `lexCheckToken` use `hWarnSuffix()` (→ warning 44) plus `hDropSuffix()`
    instead (`lex.bas:2586-2619`). Probed in fb mode: `foo%` gives
    `warning 44(1): Suffix ignored in 'foo%'` and nothing else.
  - `149 IMPLICITVARSONLYVALIDINLANG` ("Implicit variables are only valid in
    -lang") — its guard reads `fbLangOptIsSet( FB_LANG_OPT_IMPLICIT = FALSE )`
    (`parser-expr-atom.bas:435`). The `= FALSE` is **inside the macro
    argument**, and the macro is just
    `#define fbLangOptIsSet( op ) ((env.lang.opt and (op)) <> 0)` (`fb.bi:622`),
    so what gets compiled is `((env.lang.opt and FALSE) <> 0)` — permanently
    false. It is unreachable twice over: `IMPLICIT` is off only in `fb`, and
    `env.opt.explicit = (env.clopt.lang = FB_LANG_FB)` (`fb.bas:411`), so in
    that one dialect `error 42` fires first. Probed: an undeclared name gives
    `error 42` only.

  ∴ the audit needs **three** tests, not two — referenced, called, and guarded
  by a condition that can be true — and only the third caught these. "Is the
  name referenced?" is not the reachability test; neither is a count of
  references.
- **`error 14` has two constant names.** `error.bi:19-20` declares
  `FB_ERRMSG_EXPECTEDVAR` and then
  `FB_ERRMSG_EXPECTEDIDENTIFIER = FB_ERRMSG_EXPECTEDVAR` — the only alias in the
  catalog. It consumes no number and both names print "Expected identifier", so
  key a table on the number and never on the constant.
- Call-site frequency is a usable proxy for "what real code trips on":
  `17 SYNTAXERROR` 106, `24 INVALIDDATATYPES` 86, `14 EXPECTEDIDENTIFIER` 61,
  `9 EXPECTEDEXPRESSION` 59, `4 DUPDEFINITION` 45, `20 TYPEMISMATCH` 43,
  `7 EXPECTEDRPRNT` 42. Note that 4 and 20 are *semantic* — roughly a quarter
  of the catalog's call sites are name and type checks, not syntax.
- `tests/` is 2493 `.bas` files, and **`tests/warnings/` is one minimal
  reproducer per warning** (72 files) — empirical trigger conditions rather than
  a prose description. `tests/quirk/` (52 files) is deliberate oddities.

Two traps when reading `error.bas` by hand, both silent rather than loud: the
arrays are **brace**-delimited inside the parens-looking array bound
(`warningMsgs( 1 to N-1 )` closes a naive paren scan on its first `)`), and
each name is space-padded before its closing quote, so a pattern without
tolerating that whitespace matches **zero** entries and still reports success.
