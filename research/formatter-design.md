# Formatter design research

The original option-catalog research the formatter was designed from, written
before M17 and kept out of tree until the 2026-11-02 plan pass moved it here.
It is the provenance for the config schema (`src/format/config.h` parses
nearly all of it) and for the reserved-options tail M26 carries in
[`PLAN.md`](../PLAN.md). Two of its points were settled or left outstanding
in ways the text below does not reflect, and both are recorded in the plan
rather than edited here: the built-in default shipped as the `FBdocs` preset,
not the factory `Leave` values recommended below, a deliberate choice (it is
the style readers of the FreeBASIC documentation already know), and the
`ext` preset planned below, measured from the Extended Library project,
remains outstanding under M26.

# FreeBASIC formatter options

Research notes for M17 (`textDocument/formatting` and friends). This file is a
design reference, not code. It lists the options a FreeBASIC formatter should
expose, what each controls, what clang-format calls the same idea, and whether
the clang-format shape survives contact with FreeBASIC.

Everything here is grounded in the language facts in `FreeBASIC.md`, the
FreeBASIC distribution's own sources under `~/Projects/freebasic/compiler`, and
probes run against fbc 1.10.2. Option names in `code font` are proposals, not a
committed schema.

## How to read a row

Each catalog table has the same columns:

- Option: the proposed key.
- Values: the accepted settings. `Leave` always means "do not touch".
- Default: the library default, chosen to keep a first format run boring.
- clang-format: the analogue, or `new` when clang-format has none.
- Notes: FreeBASIC-specific behavior that changes the option's meaning.

Defaults favor `Leave` wherever changing a value rewrites a lot of bytes on a
first run. The `FBdocs` and `fbc` presets switch to the documentation's and the
compiler's house styles; see [Presets](#presets).

## The constraints the language puts on a formatter

Five facts decide what is safe to configure. Get these wrong and the formatter
stops being a formatter.

1. **There is no implicit line continuation.** A break inside `( ... )` without
   a trailing `_` is a parse error. Probed:

   ```freebasic
   dim a as integer = (1 +
    2)          ' error 9: Expected expression
   ```

   So wrapping a long line means inserting `_`, which is a token the formatter
   owns, not whitespace.

2. **A `:` on a single-line `If` line is part of the branch, not a statement
   separator you can split.** Probed: `if 0 then print "a" : print "b"` prints
   nothing, so both prints are inside the guarded branch. Splitting it into two
   lines makes the second print unconditional. The same holds after `Else`.
   `:` splitting is safe for `For`/`Next`, `With`, `Case` labels, and ordinary
   statements, which are all separate statements.

3. **`#macro` and `#define` bodies are opaque.** A `#macro` body is never parsed
   and may hold anything. The formatter must emit those lines byte for byte.

4. **Some case and spelling choices are load-bearing.** `ElseIf` cannot be
   rewritten as `Else If`: probed, a multi-line `else` followed by `if` is
   `error 32`. `Rem` at the start of a line is a comment, `Rem` mid-line is a
   statement. String sigils (`!` and `$`) select different literal semantics.
   In `qb`/`fblite`/`deprecated`, an identifier's type suffix (`foo$`) is part of
   its identity.

5. **Metacommands live in comments.** `'$LANG: "qb"` is a comment the preprocessor
   reads. Moving it, re-casing the `$` word, or merging it into another comment
   line changes compilation, and `''` is the marker that says "this comment
   holds no metacommand".

One prerequisite sits ahead of all of this: the lexer does not currently lex
`/' ... '/` block comments (`FreeBASIC.md` §12 item 3). A formatter needs a
lossless token stream first, because a file that uses a block comment cannot be
round-tripped today.

## What transfers from clang-format

clang-format is a good guide because its option set is the result of the same
problem, and because its file layout (one flat namespace, a `BasedOnStyle`
preset, per-language sections, walking up the tree to find the config, and
`clang-format off` pragmas) is a shape a FreeBASIC tool can copy almost
unchanged.

Three differences matter:

- **clang-format never changes keyword or identifier case.** C++ keywords have
  one spelling. FreeBASIC keywords are case-insensitive, so casing is a real
  style choice, and the whole `KeywordCase` family is new work. It is also the
  option the user's example asks about first.
- **clang-format's block model is braces.** Everything about `BreakBeforeBraces`,
  `BraceWrapping`, `InsertBraces`, and `RemoveBracesLLVM` drops. FreeBASIC blocks
  are keyword pairs, and a whole block can sit on one physical line because `:`
  joins its parts. Probed one-liners: `If x Then y`, `For ... : Next`,
  `While ... : Wend`, `Do ... : Loop`, `Select Case ... : End Select`,
  `With ... : End With`, and even `Namespace ... : End Namespace`. So the brace
  options become two per-block-family gates, one for `If` and one for the rest.
  Both are one-way: the formatter may expand a one-liner into a block, never
  merge a block back onto one line. The `If` case shows why the directions are
  not symmetric: a one-line `If` cannot take `ElseIf` (`error 3`), and a block
  `End If` cannot follow a body that starts on the `Then` line (`error 110`).
- **Live operator/symbol spellings replace C's qualifiers.** `Ptr` versus
  `Pointer`, `Const` before or after the type in a chain, `Dim x As T` versus
  `Dim As T x`, and `?` versus `Print` are all real, legal, and interchangeable.
  That is a new option family with no clang-format counterpart.

## Catalog

### A. File, dialect, and line endings

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `BasedOnStyle` | preset name | `None` | `BasedOnStyle` | See [Presets](#presets). `None` leaves the factory values, which are all `Leave`. |
| `Standard` | `auto`, `fb`, `fblite`, `qb`, `deprecated` | `auto` | `Standard` | `auto` reads `#LANG` and `$LANG`. Gates the suffix and `Option` rules. |
| `ColumnLimit` | integer, `0` = off | `0` | `ColumnLimit` | Default off because every break needs a `_`. The `FBdocs` and `fbc` presets set 80. |
| `LineEnding` | `LF`, `CRLF`, `DeriveLF`, `DeriveCRLF`, `Leave` | `DeriveLF` | `LineEnding` | Matters on Windows, where a disk copy is CRLF under an LF `didOpen`. |
| `InsertNewlineAtEOF` | bool | `true` | `InsertNewlineAtEOF` | FreeBASIC sources usually end with a newline. |
| `TrimTrailingWhitespace` | bool | `true` | client option | Also supplied by the client. See [precedence](#where-the-config-lives). |
| `TrimFinalNewlines` | bool | `true` | client option | Same. |
| `DisableFormat` | bool | `false` | `DisableFormat` | Whole-file opt-out. |

### B. Indentation and tabs

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `IndentWidth` | integer | `4` | `IndentWidth` | The distribution uses one tab per level. |
| `TabWidth` | integer | `4` | `TabWidth` | |
| `UseTab` | `Never`, `ForIndentation`, `ForContinuationAndIndentation`, `Always` | `ForIndentation` | `UseTab` | Compiler sources indent with tabs and align continuations with tabs too, so `ForContinuationAndIndentation` matches them exactly. |
| `ContinuationIndentWidth` | integer | `4` | `ContinuationIndentWidth` | Indent for a wrapped line when `AlignEscapedNewlines` is off. |
| `IndentCaseLabels` | bool | `false` | `IndentCaseLabels` | Shipped examples put `Case` and `End Select` at one level, the bodies one deeper. |
| `NamespaceIndentation` | `None`, `All`, `Inner` | `All` | `NamespaceIndentation` | `namespace.bas` indents the body. |
| `IndentAccessModifierSections` | bool | `false` | `IndentAccessModifiers` | `Public:` and `Private:` inside a `Type`. |
| `IndentPPDirectives` | `None`, `BeforeHash`, `AfterHash` | `None` | `IndentPPDirectives` | Compiler sources keep `#` in column 0 even inside a procedure. |
| `IndentGotoLabels` | `None`, `OuterIndent`, `InnerIndent` | `None` | `IndentGotoLabels` | |
| `IndentExternBlock` | `None`, `Indent`, `AfterExternBlock` | `Indent` | `IndentExternBlock` | Body of `Extern ... End Extern` and `Extern "C" ... End Extern`. |
| `IndentWrappedFunctionNames` | bool | `false` | `IndentWrappedFunctionNames` | A wrapped `Sub` head. |
| `BracedInitializerIndentWidth` | integer | `4` | `BracedInitializerIndentWidth` | `{ ... }` initializers, which FreeBASIC does have. |

### C. Blank lines and block separation

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `MaxEmptyLinesToKeep` | integer | `2` | `MaxEmptyLinesToKeep` | |
| `KeepEmptyLines` | `AtEndOfFile`, `AtStartOfBlock`, `AtStartOfFile` | all `true` | `KeepEmptyLines` | |
| `SeparateDefinitionBlocks` | `Leave`, `Always`, `Never` | `Leave` | `SeparateDefinitionBlocks` | Applies around `Sub`, `Function`, `Property`, `Type`, `Enum`, `Namespace`. |
| `EmptyLineBeforeAccessModifier` | `Never`, `Always`, `LogicalBlock` | `Leave` | `EmptyLineBeforeAccessModifier` | |
| `EmptyLineAfterAccessModifier` | `Never`, `Always`, `LogicalBlock` | `Leave` | `EmptyLineAfterAccessModifier` | |
| `WrapNamespaceBodyWithEmptyLines` | `Leave`, `Never`, `Always` | `Leave` | `WrapNamespaceBodyWithEmptyLines` | |

### D. Keyword, type, and builtin name case

This family is the reason a FreeBASIC formatter exists at all. It has no
clang-format counterpart because C++ keywords are case-sensitive.

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `KeywordCase` | `Leave`, `lower`, `UPPER`, `Title`, `Canonical` | `Leave` | new | Reserved words. `Title` gives `Dim`, `Byval`; `Canonical` gives the catalog's own spelling, which matters for compounds: `ElseIf`, `AndAlso`, `LongInt`, `UInteger`, `ULongInt`, `ZString`. |
| `TypeNameCase` | same | `Leave` | new | Builtin type names: `Integer`, `String`, `Single`, `ZString`. Split from `KeywordCase` because a codebase may want `As Integer` with `dim` lower. |
| `IntrinsicCase` | same | `Leave` | new | Built-in functions and statements: `Print`, `Cls`, `Left$`. Overlaps `KeywordCase` for words that are both, so the more specific key wins. |
| `BooleanLiteralCase` | `Leave`, `lower`, `UPPER`, `Title` | `Leave` | new | `True` and `False`. |
| `IntrinsicDollarSuffix` | `Leave`, `Drop`, `Add` | `Leave` | new | In `fb`, `Left` and `Left$` are one symbol (warning 44), so `Drop` is safe there and only there. In `qb`/`fblite` the suffix selects a distinct builtin, so the formatter must ignore `Drop`. |
| `IdentifierCase` | `Leave` | `Leave` | new | There is deliberately no other value. See [invariants](#do-not-make-these-configurable). |

A `Canonical` value only works against the catalog in `src/language.cpp`, so it
is also a maintenance question: the spelling table is data the grammar generator
already reads.

### E. Literal spelling

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `NumericLiteralCase.Prefix` | `Leave`, `lower`, `UPPER` | `Leave` | `NumericLiteralCase.Prefix` | `&H`, `&B`, `&O`. Probed case-insensitive, so normalizing is safe. |
| `NumericLiteralCase.HexDigit` | `Leave`, `lower`, `UPPER` | `Leave` | `NumericLiteralCase.HexDigit` | `&Hff` versus `&HFF`. |
| `NumericLiteralCase.Suffix` | `Leave`, `lower`, `UPPER` | `Leave` | `NumericLiteralCase.Suffix` | `UL`, `ULL`, `L`, `U`, `%`, `&`. Mixed forms are legal. |
| `NumericLiteralCase.ExponentLetter` | `Leave`, `lower`, `UPPER` | `Leave` | `NumericLiteralCase.ExponentLetter` | `E` and `D` versus `e` and `d`. |
| `FloatSuffixStyle` | `Leave`, `Sigil`, `Letter` | `Leave` | new | `1.5!` = `1.5F`, `1.5#` = `1.5D`. FreeBASIC-only; both are legal. |
| `IntegerLiteralSeparator` | not available | n/a | `IntegerLiteralSeparator` | FreeBASIC has no separator syntax. Do not invent one. |
| `InsertLeadingZero` | bool | `Leave` | new | `.5` to `0.5`. Probed: both compile. |
| `StringSigilStyle` | `Preserve` | `Preserve` | new | Never convert `"..."`, `!"..."`, `$"..."`. See invariants. |

### F. Punctuation and operator spacing

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `SpaceBeforeAssignmentOperators` | bool | `true` | `SpaceBeforeAssignmentOperators` | Covers `=`, `+=`, `-=`, `*=`, `/=`, `\=`, `and=`, `or=`, and the rest of the combined assigns. |
| `SpaceAroundBinaryOperators` | bool | `true` | new | `a+b` versus `a + b`. clang-format always spaces binary operators and has no key; FB code varies, so expose it. |
| `SpaceAfterUnaryOperators` | bool | `false` | `SpaceAfterLogicalNot`, `SpaceAfterCStyleCast` | `-x`, `*p`, `@num`, `~x`. `Not x` always keeps its space because `Not` is a word. |
| `SpaceAroundMemberAccess` | `Leave`, `None`, `Both`, `After` | `None` | new | `.` and `->`: `a.b` versus `a . b`, `p->n` versus `p -> n`. |
| `SpaceBeforeColon` | bool | `false` | `BitFieldColonSpacing`, `SpaceBeforeCaseColon` | Labels (`done:`), access sections (`Private:`), `Case 1:`. |
| `SpaceAfterStatementSeparator` | bool | `true` | new | Around a mid-line `:` when the formatter does not split it. |
| `SpaceAfterPrintChannelHash` | bool | `true` | new | `Print #1, x` versus `Print#1, x`. FreeBASIC-only. |
| `SpacesBeforeTrailingComments` | integer | `1` | `SpacesBeforeTrailingComments` | |

### G. Parentheses, commas, and argument lists

The user's example (`sub_name(byval param1 as long, ...)`) lands here. FB code in
the wild uses at least three paren styles, so these are independent toggles.

FreeBASIC also lets a statement-position subroutine call drop the parentheses
entirely: `our_sub 1, 2, "a string"` is equivalent to
`our_sub(1, 2, "a string")`, both probed. The paren-free form is not free
everywhere, and the exceptions are what the call options have to encode:

- A `Function` used for its value needs its parentheses. `Add3 1, 2` is
  `error 1: Argument count mismatch`; only a statement-position `Sub` call may
  drop them.
- A niladic `Function` may be bare: `Print f` and `Print f()` are both `7`.
  That makes "add parentheses to every call" a semantic no-op but a large diff.
- A parenthesized first argument followed by more arguments groups as one
  argument: `two (1) + (2)` calls `two(3)`, not `two(1) + (2)`. So removing or
  adding the outer parens of a statement call can regroup its arguments.
- Graphics statements that take a coordinate pair cannot be parenthesized at
  all. `Line((0,0)-(10,10), 15)` is `error 7: Expected ')'`. See the graphics
  subsection below.

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `SpaceBeforeParens` | `Never`, `ControlStatements`, `FunctionDeclaration`, `FunctionDefinition`, `FunctionCall`, `Always` | `Never` | `SpaceBeforeParens` | `If (x)` and `sub f (x)` are valid; style varies. |
| `SpaceBeforeParensOptions.AfterControlStatements` | bool | `false` | same | |
| `SpaceBeforeParensOptions.AfterFunctionDeclarationName` | bool | `false` | same | `Declare Sub f( ... )` versus `Declare Sub f ( ... )`. |
| `SpaceBeforeParensOptions.AfterFunctionDefinitionName` | bool | `false` | same | `Sub f()`. |
| `SpaceBeforeParensOptions.AfterFunctionCallName` | bool | `false` | same | `f(x)` versus `f (x)`. |
| `SpacesInParens.InConditionalStatements` | bool | `false` | same | `if( x )`. |
| `SpacesInParens.InEmptyParentheses` | bool | `false` | same | `sub f( )` versus `sub f()`. The distribution examples use `( )`. |
| `SpacesInParens.InParameterLists` | bool | `false` | `SpacesInParens.Other` | `sub f( a as integer )`. Split from calls; clang-format folds both into `Other`. |
| `SpacesInParens.InCallParens` | bool | `false` | `SpacesInParens.Other` | `f( x )`. |
| `SpacesInParens.InSubscripts` | bool | `false` | `SpacesInParens.Other` | `a( 0 )`. |
| `SpacesInParens.InCasts` | bool | `false` | `SpacesInParens.InCStyleCasts` | `Cast( Integer, x )`, `CPtr`. |
| `SpaceAfterComma` | bool | `true` | always | |
| `SpaceBeforeComma` | bool | `false` | new | `f( a , b )`. Present because the request named it. |
| `SpacesInBracedInitializers` | bool | `true` | `SpacesInContainerLiterals` | `{ 1, 2, 3 }` versus `{1,2,3}`. Probed: both compile. |
| `SpaceBeforeBracedInitializer` | bool | `false` | `SpaceBeforeCpp11BracedList` | `= {` versus `={`. |
| `RemoveRedundantParens` | `Leave`, `MultipleParentheses` | `Leave` | `RemoveParentheses` | Risky around `ByVal` overloads and string concat. Default off. |
| `CallParentheses` | `Leave`, `Add`, `Remove` | `Leave` | new | Statement-position `Sub` calls only. `Add` wraps `our_sub 1, 2` into `our_sub(1, 2)`; `Remove` is the reverse. Never touches a `Function` call in an expression, a niladic call, or a name in `CallParenthesesExclude`. |
| `CallParenthesesExclude` | list of statement names | the coordinate-pair graphics statements | new | Names `Add` must skip, because their argument grammar has no parenthesized form. Default: `Circle`, `Draw`, `Get`, `Line`, `Paint`, `Preset`, `PReset`, `Pset`, `Put`, `View`, `Window`. |
| `NiladicCallParentheses` | `Leave`, `Add`, `Remove` | `Leave` | new | `Print f` versus `Print f()`. Only meaningful for a known niladic procedure. |

#### Graphics statements and coordinate-pair arguments

Graphics statements are ordinary statement-position calls, so the paren-free rule
above applies to them, and they add a second layer of argument shapes that the
formatter has to parse rather than assume. These come from the wiki pages for
`Line`, `Circle`, `Paint`, `Pset`, `Preset`, `Get`, `Put`, `Draw`, `View`, and
`Window`, all cross-checked against fbc 1.10.2.

The shared shape is a leading optional target buffer, then a coordinate pair,
then optional values. `Circle (100,100), 50, 15, , , , F` and
`Line img, (0,0)-(10,10), 15, BF` both compile.

What makes them different from a normal call:

- A coordinate pair `(x, y)` is one argument written as a parenthesized pair.
  It is not a call and not a subscript. Its parens are never added or removed.
- The `-` between two pairs in `Line`, `Get`, and `Put` is a separator, not
  subtraction. Spaces around it are legal (`Line (0, 0) - (10, 10), 15`
  compiles) but the docs write it tight, and it must never be broken across
  lines as a binary operator.
- `STEP` is a keyword attached to a pair: `Step (1,1)` and `Step(1,1)` both
  compile.
- `Line -(x, y)` draws from the current point, so `-` may lead with no left
  operand. A formatter must not add one.
- Mode words sit in comma slots and are not expressions: `B`/`BF` (`Line`),
  `F` (`Circle`), and the `Put` method words `PSet`, `PReset`, `XOR`, `OR`,
  `AND`, `ALPHA`, `CUSTOM`, `TRANS`. They are case-insensitive (`line ..., b`
  compiles). The ones that are not reserved words (`B`, `BF`, `F`) need
  `GraphicsModeWordCase`; the rest already move with `KeywordCase`.
- Optional arguments are skipped with empty slots, as in
  `Circle (100,100), 50, , , , 1.5`. The `,,` runs carry position and are never
  collapsed.
- `Draw` takes a string of drawing commands (`Draw "BM100,100"`), and `Paint`'s
  fill can be a pattern string. Those strings are never touched.
- `Get` and `Put` are overloaded with a file form, `Get #1, , x` and
  `Put #1, , x`. The graphics rules must not apply when a `#` channel appears.

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `CoordPairSeparatorSpacing` | `Leave`, `None`, `Around` | `Leave` | new | Spaces around the `-` in `(x1,y1)-(x2,y2)`. `None` matches the docs, `Around` is legal but unusual. |
| `StepPrefixSpacing` | `Leave`, `Attached`, `Spaced` | `Leave` | new | `Step(x,y)` versus `Step (x,y)`. |
| `SpaceBetweenEmptyArguments` | bool | `false` | new | `f(a,,b)` versus `f(a, , b)`. |
| `GraphicsModeWordCase` | `Leave`, `lower`, `UPPER` | `Leave` | new | `B`, `BF`, `F`, and the `Put` method words. A reserved word also obeys `KeywordCase`; the more specific key wins. |

The rest are invariants, not options, and are restated in "Do not make these
configurable": the pair separator is never a break point, empty slots stay put,
and a `Draw` command string or `Paint` pattern string is never edited.

### H. Declarations and procedure heads

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `CanonicalizeSpecifierOrder` | `Leave`, `Canonical` | `Leave` | `QualifierAlignment` | Legal order varies: `Public`/`Private`/`Static`, `Declare`, `Sub`/`Function`, then after the name `Cdecl`/`Stdcall`/`Pascal`, `Alias`, `Overload`, `Constructor [priority]`, `Destructor`, `Export`, `As Type`. |
| `PointerSpellingStyle` | `Leave`, `Ptr`, `Pointer` | `Leave` | new | `As Integer Ptr` versus `As Integer Pointer`. Both legal. `Ptr(T)` is not legal, so this only renames the keyword. |
| `ConstQualifierPosition` | `Leave`, `Left`, `Right` | `Leave` | `QualifierAlignment` | `As Const Integer Ptr` versus `As Integer Const Ptr`. Both legal, both probed. |
| `DimTypePosition` | `Leave`, `NameFirst`, `TypeFirst` | `Leave` | new | `Dim x As Integer` versus `Dim As Integer x`. Both legal for `Dim`/`Static`. A parameter is name-first only: `Sub f(ByVal As Integer x)` is `error 59`, so this option never touches parameter lists. |
| `TypeFieldDim` | `Leave`, `RequireDim`, `OmitDim` | `Leave` | new | `Dim x As Integer` versus `x As Integer` inside a `Type`. `Dim` is optional in a record body. |
| `NextVariableStyle` | `Leave`, `Always`, `Omit` | `Leave` | new | `Next i` and bare `Next` both compile. |
| `CallKeywordStyle` | `Leave`, `Remove` | `Leave` | new | `Call foo()` versus `foo()`. `Call` is `error 146` in `fb`, so `Remove` only ever fixes a legacy-dialect buffer and `Require` does not exist. |
| `LetKeywordStyle` | `Leave`, `Remove` | `Leave` | new | `Let x = 1` versus `x = 1`. `Let` is `error 146` in `fb`, same as `Call`. |
| `BreakFunctionDeclarationParameters` | `Before`, `After`, `Leave` | `Leave` | same | |
| `BreakFunctionDefinitionParameters` | `Before`, `After`, `Leave` | `Leave` | same | |
| `AllowAllParametersOfDeclarationOnNextLine` | bool | `true` | same | |
| `AllowAllArgumentsOnNextLine` | bool | `true` | same | |
| `BinPackParameters` | bool | `true` | `BinPackParameters` | |
| `PackParameters` | `OnePerLine`, `Never`, `Always` | `OnePerLine` | `PackParameters` | |
| `BinPackArguments` | bool | `true` | `BinPackArguments` | |

### I. Records, enums, and access sections

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `AccessSectionSpacing` | `Leave`, `ColonOnly`, `ColonAndBlank` | `Leave` | part of brace options | `Public:` with or without a following blank line. |
| `FieldAlignmentSpacing` | `Leave`, `Tight`, `Aligned` | `Leave` | new | `Field = 4` on the opener line. |
| `EnumTrailingComma` | `Leave` | `Leave` | `EnumTrailingComma` | A trailing comma continues the member list on the next line, so adding or removing one changes the parse. Leave only. |

### J. Statements and the `:` separator

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `StatementSeparatorStyle` | `Leave`, `SplitSafe`, `SplitAll`, `Join` | `Leave` | new | `SplitSafe` splits every top-level `:` except the ones inside a one-line `If` or a one-line block. `SplitAll` splits those too, which needs the matching `AllowShort...OnASingleLine: Never`. `Join` merges adjacent ordinary statements and is rarely what anyone wants; it never creates a one-line block. |
| `AllowShortIfStatementsOnASingleLine` | `Leave`, `Never` | `Leave` | `AllowShortIfStatementsOnASingleLine` | The one-line `If`. `Never` expands every one-line `If` into a block; `Leave` keeps it as written. One-way: no value merges a block `If` onto one line. Splitting a `:` that sits inside a one-line `If` needs `Never`, because that `:` belongs to the branch, not to the statement separator. |
| `AllowShortBlocksOnASingleLine` | `Leave`, `Never` | `Leave` | `AllowShortBlocksOnASingleLine` | The same gate for the other one-line blocks, whose parts `:` joins: `For`/`Next`, `While`/`Wend`, `Do`/`Loop`, `Select Case`/`End Select`, `With`/`End With`, and the declaration blocks `Type`/`End Type`, `Enum`/`End Enum`, `Namespace`/`End Namespace`. `Never` expands each into a block; `Leave` keeps it. One-way, no join value. |
| `IfThenSpacing` | `OneSpace` | `OneSpace` | new | `Then` needs at least one space. Not worth a value besides the default. |
| `RemCommentStyle` | `Leave`, `ConvertToQuote` | `Leave` | new | `Rem ...` to `' ...`. Safe only at the start of a line, where `Rem` is a comment. |
| `PrintShortcutStyle` | `Leave`, `Expand` | `Leave` | new | `?` to `Print`. Probed: 47 uses in the shipped examples, so leaving it is the polite default. |
| `ElseIfStyle` | `Leave` | `Leave` | new | `ElseIf` to `Else If` is not a style change, it is a different construct. Leave only. |

### K. Line breaking and `_` continuations

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `ContinuationStyle` | `Leave`, `WrapOnly`, `WrapAndJoin` | `Leave` | new | `WrapOnly` re-wraps lines over the limit but never re-joins an author's breaks. `WrapAndJoin` re-joins a `_` line that would fit. |
| `BreakBinaryOperators` | `None`, `NonAssignment`, `All` | `None` | `BreakBeforeBinaryOperators` | `None` leaves the operator at the end of the first line (`x or _`), which is what the compiler sources do. `All` puts it at the start of the continuation. |
| `AlignEscapedNewlines` | `Left`, `Right`, `DontAlign` | `Right` | `AlignEscapedNewlines` | Aligns the `_` column. |
| `AlignmentAfterOpenBracket` | `Align`, `DontAlign`, `AlwaysBreak`, `BlockIndent` | `Align` | `AlignAfterOpenBracket` | Where wrapped arguments line up. |
| `BreakStringLiterals` | bool | `false` | `BreakStringLiterals` | Splitting a string means `"..." & _` and a changed literal. Default off. |
| `PenaltyExcessCharacter` | integer | preset | `PenaltyExcessCharacter` | The cost model that decides where a wrap goes. A small FB set is enough. |
| `PenaltyBreakBeforeFirstCallParameter` | integer | preset | same | |

### L. Alignment

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `AlignConsecutiveAssignments` | bool, plus `AcrossEmptyLines`, `AcrossComments`, `PadOperators` | `false` | same | Aligns runs of `=` and the combined assigns. |
| `AlignConsecutiveDeclarations` | bool, plus the sub-keys | `false` | same | Aligns `Dim` names and `As` types. FB declarations are common enough to want this. |
| `AlignConsecutiveMacros` | bool, plus the sub-keys | `false` | `AlignConsecutiveMacros` | Aligns `#define` values. |
| `AlignTrailingComments` | `Always`, `Never`, `Leave`, plus `OverEmptyLines` | `Leave` | `AlignTrailingComments` | |
| `AlignOperands` | `DontAlign`, `Align`, `AlignAfterOperator` | `DontAlign` | `AlignOperands` | |
| `AlignConsecutiveCaseStatements` | bool, plus the sub-keys | `false` | `AlignConsecutiveShortCaseStatements` | |
| `AlignArrayOfStructures` | `None`, `Left`, `Right` | `None` | `AlignArrayOfStructures` | `{ ... }` initializers. |

### M. Comments and doc comments

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `ReflowComments` | `Never`, `NonDoc`, `All` | `Never` | `ReflowComments` | Default off. A reflow can merge a line that carried a `$` metacommand, or join two `''` doc lines with different meanings. |
| `SpacesInLineCommentPrefix.Minimum` | integer | `1` | same | `'foo` to `' foo`. |
| `SpacesInLineCommentPrefix.Maximum` | integer | `-1` | same | |
| `SpacesBeforeTrailingComments` | integer | `1` | same | |
| `CommentPragmas` | regex | `^[[:space:]]*\$` | `CommentPragmas` | Lines matching it are never reflowed. The default protects metacommands. |
| `DocCommentPrefix` | `Leave`, `DoubleQuote`, `Slash` | `Leave` | new | `''` versus `///`. Hover text depends on these, so changing them is a project decision, not a formatting whim. |

### N. Preprocessor and metacommands

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `DirectiveCase` | `Leave`, `lower`, `UPPER` | `Leave` | new | `#include`, `#if`, `#define`. |
| `MetacommandCase` | `Leave`, `lower`, `UPPER` | `Leave` | new | `$lang`, `$dynamic`, `$include`. |
| `MacroBodyFormatting` | `Preserve` | `Preserve` | `WhitespaceSensitiveMacros` | `#macro` and `#define` bodies are emitted verbatim. Leave only. |
| `SortIncludes` | bool | `false` | `SortIncludes` | Default off. `#include` is textual and order-sensitive: reordering can break scope and once-guards. |
| `IncludeBlocks` | `Preserve`, `Merge`, `Regroup` | `Preserve` | `IncludeBlocks` | Only meaningful if sorting is on. |
| `IncludeCategories` | list | empty | `IncludeCategories` | Group headers if sorting is ever enabled. |
| `AlignConsecutiveMacros` | bool | `false` | same | See alignment. |
| `DirectiveContinuationAlign` | `Leave`, `ContinuationIndent`, `AlignToValue` | `Leave` | part of `AlignEscapedNewlines` | A multi-line `#define` or `#macro` head. |

### O. Format-off control

| Option | Values | Default | clang-format | Notes |
|---|---|---|---|---|
| `FormatOffPragma` | string | `fbformat: off` | `clang-format off` (fixed) | Written as a comment: `' fbformat: off`. |
| `FormatOnPragma` | string | `fbformat: on` | `clang-format on` (fixed) | |
| `OneLineFormatOffRegex` | regex | empty | `OneLineFormatOffRegex` | Lines to leave untouched. |
| `DisableFormat` | bool | `false` | `DisableFormat` | Whole file. |

Format-off regions are the escape hatch for the shapes the formatter cannot
prove safe: hand-aligned tables, `#macro` bodies the user splits by hand, and
preprocessor-generated blocks.

## Presets

A preset is a named bundle of the keys above, the way `BasedOnStyle` works in
clang-format. Each ships as a `<name>.toml` file holding only `[format]` keys,
found through the `[format-style]` search, so a project can shadow a shipped
preset with its own file of the same name.

The built-in default, used when no `BasedOnStyle` is named, is the factory
values: every case option `Leave`, `ColumnLimit 0`, `ContinuationStyle Leave`,
`StatementSeparatorStyle Leave`, and both `AllowShort...OnASingleLine` gates
`Leave`. A first run fixes indentation, spacing, and line endings and changes no
token spelling. It is not a shipped preset, because every named preset below
re-cases keywords on a first run.

`FBdocs`. The style the wiki's code examples use. The wiki renders the 1189
files under `compiler/examples/manual`, so that tree is the corpus, 40080 lines:
`KeywordCase Title`, `TypeNameCase Title`, `IntrinsicCase Title` (`Dim`, `As`,
`Integer`, `Print`, `If`, `Then`, `Allocate`, `SizeOf`), and `Dim As T x`
type-first, 1376 against 921 name-first. Indentation is always a tab (10906
tab-led lines against 0 space-led), calls lean tight (`f(x)` at 6606 against 1602
spaced), and 96.5% of lines are already at or under 80 columns.

`fbc`. The compiler's own style. Measured from the 132603 lines under
`compiler/src`:

- `KeywordCase lower`, `TypeNameCase lower`, `IntrinsicCase lower`.
- `UseTab ForContinuationAndIndentation`, `IndentWidth 4`.
- `SpaceBeforeParens Never` and all `After*` keys false, but
  `SpacesInParens.InConditionalStatements true`,
  `SpacesInParens.InEmptyParentheses true`,
  `SpacesInParens.InParameterLists true`, `SpacesInParens.InCallParens false`.
  The compiler sources write `if( x ) then` with inner spaces, `f(x)` with
  none, and `sub f( x as integer )` with inner spaces.
- `ColumnLimit 80`. 97.8% of compiler-source lines are already at or under 80.
- `IndentCaseLabels false`, `NamespaceIndentation All`.
- `LineEnding LF`, `InsertNewlineAtEOF true`.
- `StatementSeparatorStyle Leave`.

`FBdocs` and `fbc` differ in exactly one dimension: keyword case, `Title`
against `lower`. Both indent with tabs, both put the type first in a `Dim` (more
decisively at 90/10 in `compiler/src` than at 60/40 in `examples/manual`), and
both keep calls tight, so the two measured corpora agree on everything else. A
third preset, `ext`, is planned from a couple of larger hand-written projects,
to see whether real code settles somewhere the documented and compiler corpora
do not.

Both shipped presets re-case keywords on a first run, which is why neither is
the default.

## Do not make these configurable

Each of these can be written two ways by a human and only one way by a correct
formatter. Expose them and the formatter will eventually corrupt code.

- String literal sigils. `"..."`, `!"..."`, and `$"..."` are three different
  literals, and `""` inside them means different things. Never rewrite one to
  another.
- `Print` separators. `,` and `;` produce different output. They are not
  whitespace.
- `ElseIf`. Writing `Else If` after a multi-line `Else` is `error 32`.
- `#macro` and `#define` bodies. Not parsed, not touched.
- Metacommand lines. `$` inside a comment is preprocessor input. Never reflow,
  merge, or move it. `''` is the marker that a comment holds none.
- Identifier type suffixes under `qb`, `fblite`, and `deprecated`. `foo$` and
  `foo` are different symbols there. In `fb` they alias, so only `fb` may drop
  the suffix.
- Enum member trailing commas. A trailing comma continues the list.
- `_` continuation markers. Adding or removing one changes whether two lines are
  one statement.
- `#include` order, by default. Inclusion is textual and order-sensitive.
- Doc-comment markers and the hover text built from them.
- The `:` that separates a label from its statement target.
- The `?` and `Rem` shortcuts, unless the user asks. Both are real syntax.
- The `-` between two coordinate pairs (`Line (0,0)-(10,10)`). It is a
  separator, not subtraction, and never a line-break point.
- Empty argument slots in a graphics statement. The `,,` runs carry argument
  position, so `Circle (100,100), 50, , , , 1.5` cannot be compacted.
- The `Draw` command string and a `Paint` pattern string. They hold a second
  language of their own.

## Where the config lives

Two files share one schema. Both hold the same `[format]` options table and the
same `[format-style]` table, and the schema is defined once so a single loader
serves both.

1. `freebasicd.toml`, the project file. This is the first home, because the
   server already discovers roots by that file, already parses it with
   tomlplusplus (`src/settings.{h,cpp}`), already re-reads it on
   `workspace/didChangeConfiguration`, and already snapshots `Settings` per
   index. No new discovery path, no new parser.
2. `.fbformat`, the standalone formatter's own TOML file, discovered by walking
   up the tree like `.clang-format`. The separate executable is the eventual
   shape, not the first one.

When both files are present, `.fbformat` wins. The rule is about intent: a user
who writes a `.fbformat` beside a tree is formatting that tree directly, so the
file they wrote for the tool is the one that should be authoritative.

The `[format]` header is kept even though the option table is the only thing the
formatter strictly needs, because the file also holds a sibling section:

```toml
[format-style]
style = "fbc"      # preset name, with no .toml extension

[format]
KeywordCase = "lower"
# ...
```

`[format-style].style` names a preset file `<name>.toml` that itself holds only
`[format]` keys. The name is resolved by search, highest priority first: local,
then user, then global. [Preset search paths](#preset-search-paths) gives the
folders for each platform. The first `<name>.toml` found wins, which is what
lets a project-level preset shadow a user or global one of the same name.

`.editorconfig` is rejected. It has no vocabulary for `KeywordCase` or
`FloatSuffixStyle`, and half the options here are language-specific.

### Precedence

**One file, then its preset, then the built-in default.** A config file, when
present, is the whole rule set. A key it omits takes the value from the preset
it names, or the built-in default when it names none. It never falls through to
the other config file: a `.fbformat` that sets `KeywordCase` and nothing else
leaves every other key at the preset's value, not at `freebasicd.toml`'s.

Highest to lowest:

1. The `[format]` keys of `.fbformat`, if that file exists.
2. Otherwise the `[format]` keys of `freebasicd.toml`. These two are alternatives,
   not layers: only the file that exists is read, whichever it is.
3. The `[format]` keys of the preset named by that file's `[format-style].style`.
4. The built-in default for a key the file and its preset both leave unset.

Presence decides, not content: an existing `.fbformat` with no `[format]` table
still suppresses `freebasicd.toml`'s.

The client's LSP `FormattingOptions` (`tabSize`, `insertSpaces`,
`trimTrailingWhitespace`, `insertFinalNewline`, `trimFinalNewlines`) are a
separate, lower layer that applies only when no config file is present at all. A
configured project is not silently re-tabbed by an editor whose defaults are
spaces. Mapping the client keys is mechanical: `insertSpaces false` means
`UseTab ForIndentation`, `tabSize` feeds `TabWidth`, and `insertSpaces true`
means `UseTab Never`.

The standalone executable has no client, so its order is `.fbformat`, then
`freebasicd.toml`, then the named preset, then the built-in default.

### Preset search paths

A `[format-style].style` name resolves to `<name>.toml`, searched in three
places, highest priority first. The first match wins, so a project preset
shadows a user or global preset of the same name.

| Level | Linux | macOS | Windows |
|---|---|---|---|
| local | `<config-file-dir>/.styles/` | same | same |
| user | `~/.config/fbformat/styles/` | `~/.config/fbformat/styles/` | `%USERPROFILE%\fbformat\styles\` |
| global | `${prefix}/share/fbformat/styles/` (`/usr/share` by default) | `${prefix}/share/fbformat/styles/` (`/usr/local` or Homebrew) | `styles\` beside the installed binaries |

Notes:

- "local" is relative to the config file that was found, so a project preset
  travels with `freebasicd.toml` or `.fbformat`. It is hidden (`.styles/`) to
  keep a project root uncluttered; the user and global folders stay visible
  under their own `fbformat/` directory, where nothing shares the namespace.
- The user folder sits under `~/.config` on Linux and macOS, and under `$HOME`
  on Windows, so all three platforms share one shape.
- The global folder follows the install prefix on Unix, `/usr` for a system
  package and the Homebrew prefix on macOS. On Windows there is no prefix, so it
  sits next to the binaries.
- macOS keeps the Linux layout. A native layout, if it is ever wanted, would
  move only the user folder, into `~/Library/Application Support`.

## Open questions for the design pass

- Does the formatter target the whole document first, or `rangeFormatting` and
  `onTypeFormatting` too? A range format that only reformats lines touched is a
  different engine from a full reprint.
- What are the `onTypeFormatting` triggers? Candidates: `:` (reindent the new
  statement, but never split a one-line `If`), `_` (indent the continuation),
  `)` (fix a parameter list), and newline.
- May a preset file name another preset to inherit from, so presets chain? If
  so, the loader needs cycle detection, the way `BasedOnStyle` does.
- With a config file present, do the client's `FormattingOptions` ever apply, or
  only when the project has no file at all? The current cut says the latter.
- Does `StatementSeparatorStyle SplitSafe` align with what users expect, or
  should `Leave` be permanent for `:` and the option dropped?
- Should `SplitAll` imply expanding the enclosing one-line block, or stay a
  separate opt-in above the `AllowShort...OnASingleLine: Never` gates? The gates
  are the only place that decides whether a block may sit on one line.
- How does the formatter stay idempotent once `_` wrapping is on? Wrapping feeds
  the next wrap's decisions, and the `Penalty*` weights decide whether a second
  run moves anything.
- Should the formatter reuse the parser, or a lexer-only pass with a declaration
  scanner? `SplitSafe` needs to know which `:` sits inside a one-line `If`, which
  is a block-structure question the lexer alone cannot answer.
- Should `FBdocs` be the default instead of the factory `Leave` values, given the
  wiki style is what a reader will have seen? The conservative answer is no: it
  re-cases a file written in `lower` or `UPPER`, the same objection that keeps
  `fbc` a named preset.

## Appendix: clang-format options by disposition

Grouped by what FreeBASIC should do with each clang-format key.

### Copy, the idea is identical

`BasedOnStyle`, `ColumnLimit`, `CommentPragmas`, `ContinuationIndentWidth`,
`DisableFormat`, `IndentWidth`, `TabWidth`, `UseTab`, `LineEnding`,
`InsertNewlineAtEOF`, `MaxEmptyLinesToKeep`, `KeepEmptyLines`,
`SeparateDefinitionBlocks`, `WrapNamespaceBodyWithEmptyLines`,
`NamespaceIndentation`, `IndentCaseLabels`, `IndentPPDirectives`,
`IndentGotoLabels`, `IndentWrappedFunctionNames`, `IndentExternBlock`,
`EmptyLineBeforeAccessModifier`, `EmptyLineAfterAccessModifier`,
`AlignConsecutiveAssignments`, `AlignConsecutiveDeclarations`,
`AlignConsecutiveMacros`, `AlignTrailingComments`, `AlignOperands`,
`AlignEscapedNewlines`, `AlignAfterOpenBracket`, `AlignArrayOfStructures`,
`SpacesBeforeTrailingComments`, `SpacesInLineCommentPrefix`,
`SpaceBeforeAssignmentOperators`, `SpaceBeforeParens`, `SpacesInParens`,
`SpaceAfterCStyleCast`, `SpaceAfterLogicalNot`, `BinPackArguments`,
`BinPackParameters`, `PackArguments`, `PackParameters`,
`AllowAllArgumentsOnNextLine`,
`AllowAllParametersOfDeclarationOnNextLine`,
`BreakFunctionDeclarationParameters`, `BreakFunctionDefinitionParameters`,
`AllowShortIfStatementsOnASingleLine`, `AllowShortBlocksOnASingleLine`,
`BreakStringLiterals`, `NumericLiteralCase`, `IntegerLiteralSeparator`,
`RemoveParentheses`, `ReflowComments`, `SortIncludes`, `IncludeBlocks`,
`IncludeCategories`, `OneLineFormatOffRegex`, `PenaltyExcessCharacter`,
`PenaltyBreakBeforeFirstCallParameter`, `Standard`.

`IntegerLiteralSeparator` copies the key but has nothing to do: FreeBASIC has no
separator syntax. `SortIncludes` copies the key and defaults to off.

### Rename to the FreeBASIC word, same behavior

- `QualifierAlignment` becomes two keys: `ConstQualifierPosition` and
  `CanonicalizeSpecifierOrder`.
- `PointerAlignment` and `SpaceAroundPointerQualifiers` fold into
  `PointerSpellingStyle` and `SpaceAroundUnaryOperators`. FreeBASIC's `Ptr` is a
  postfix word, not a `*` the formatter aligns.
- `SpacesInContainerLiterals` becomes `SpacesInBracedInitializers`.
- `SpaceBeforeCpp11BracedList` becomes `SpaceBeforeBracedInitializer`.
- `EnumTrailingComma` keeps the name and loses every value but `Leave`.
- `AlignConsecutiveShortCaseStatements` becomes
  `AlignConsecutiveCaseStatements`.
- `WhitespaceSensitiveMacros` becomes `MacroBodyFormatting`, which has one value.

### Adapt, keep the name and change the meaning

- `SpaceBeforeParensOptions` splits `AfterFunctionCallName` from the declaration
  and definition names. FreeBASIC needs all three because `Declare Sub f (`,
  `Sub f (`, and `f (` are three independent choices.
- `SpacesInParensOptions` must split `Other` into `InParameterLists`,
  `InCallParens`, `InSubscripts`, and `InCasts`. The distribution writes spaced
  parameter lists and tight calls in the same file.
- `AllowShortIfStatementsOnASingleLine` and `AllowShortBlocksOnASingleLine` drop
  the C values that merge a block onto one line and keep only `Leave` and
  `Never`, because the two directions are not symmetric in FreeBASIC. `Never`
  expands a one-liner into a block; nothing turns a block into one line.
- `StatementSeparatorStyle` is new, and together with those two keys it encodes
  the `:` rules from [the constraints](#the-constraints-the-language-puts-on-a-formatter).
- `BreakBeforeBinaryOperators` becomes `BreakBinaryOperators`, and `None` means
  the operator trails the first line, which is the compiler-source convention.

### Drop, no FreeBASIC construct behind it

Braces and blocks: `BraceWrapping`, `BreakBeforeBraces`, `InsertBraces`,
`RemoveBracesLLVM`, `RemoveSemicolon`, `IndentExportBlock`,
`AllowShortCaseLabelsOnASingleLine`, `AllowShortEnumsOnASingleLine`,
`AllowShortFunctionsOnASingleLine`, `AllowShortLambdasOnASingleLine`,
`AllowShortNamespacesOnASingleLine`, `AllowShortRecordOnASingleLine`,
`AllowShortCaseExpressionOnASingleLine`, `LambdaBodyIndentation`.
`AllowShortLoopsOnASingleLine` becomes `AllowShortBlocksOnASingleLine`, since a
FreeBASIC loop body closes with `Next`/`Wend`/`Loop`, not a brace.

C++ type and template machinery: `BreakTemplateDeclarations`,
`BreakBeforeTemplateCloser`, `IndentRequiresClause`, `RequiresClausePosition`,
`RequiresExpressionIndentation`, `AllowShortCompoundRequirementOnASingleLine`,
`BreakBeforeConceptDeclarations`, `SpaceAfterTemplateKeyword`,
`SpacesInAngles`, `BreakInheritanceList`, `SpaceBeforeInheritanceColon`,
`SpaceBeforeEnumUnderlyingTypeColon`, `BreakConstructorInitializers`,
`SpaceBeforeCtorInitializerColon`, `ConstructorInitializerIndentWidth`,
`SpaceBeforeRangeBasedForLoopColon`, `BreakBeforeInlineASMColon`,
`AllowBreakBeforeNoexceptSpecifier`, `AttributeMacros`,
`StatementAttributeLikeMacros`, `BreakAfterAttributes`,
`BreakAfterJavaFieldAnnotations`, `BreakBeforeReturnType`, `BreakAfterReturnType`,
`AlwaysBreakAfterDefinitionReturnType`, `PenaltyReturnTypeOnItsOwnLine`,
`PenaltyBreakTemplateDeclaration`, `PenaltyBreakScopeResolution`,
`PenaltyBreakAssignment`, `PenaltyBreakBeforeMemberAccess`,
`PenaltyBreakOpenParenthesis`, `PenaltyBreakFirstLessLess`, `PenaltyBreakString`,
`PenaltyIndentedWhitespace`, `PenaltyBreakComment`.

`PenaltyBreakScopeResolution` is worth calling out: FreeBASIC uses `.` for member
access, not `::`, so there is no scope operator to break on.

Arrays and brackets: `SpaceBeforeSquareBrackets`, `SpacesInSquareBrackets`,
`BreakArrays`, `BreakAfterOpenBracketBracedList`. FreeBASIC subscripts use `(`.

Other languages: `ObjC*`, `Java*`, `JavaScriptQuotes`, `JavaScriptWrapImports`,
`TableGen*`, `BitFieldColonSpacing`, `SortJavaStaticImport`,
`SortUsingDeclarations`, `FixNamespaceComments`, `CompactNamespaces`
(FreeBASIC has `namespace C.D.E`, which the formatter should leave, not
compact), `ShortNamespaceLines`, `MainIncludeChar`, `IncludeIsMainRegex`,
`IncludeIsMainSourceRegex`, `MacroBlockBegin`, `MacroBlockEnd`, `IfMacros`,
`ForEachMacros`, `ExperimentalAutoDetectBinPacking`, `DerivePointerAlignment`,
`ReferenceAlignment`, `SpacesInAngles`, `SpaceBeforeJsonColon`,
`BreakAdjacentStringLiterals`, `AlwaysBreakBeforeMultilineStrings`,
`KeepFormFeed`, `BinaryMinDigitsInsert`, `SkipMacroDefinitionBody` (subsumed by
`MacroBodyFormatting`).

## A worked example

The request's example line, under the default and the two presets:

```freebasic
declare sub sub_name(byval param1 as long, byval param2 as single)
```

Under the built-in default, everything at `Leave`, nothing changes except
spacing the formatter can prove safe. Under `FBdocs`:

```freebasic
Declare Sub sub_name(ByVal param1 As Long, ByVal param2 As Single)
```

That is `KeywordCase Title`, `TypeNameCase Title`, and `IntrinsicCase Title`.
Under `fbc`:

```freebasic
declare sub sub_name( byval param1 as long, byval param2 as single )
```

That is `KeywordCase lower` (already lower), `SpacesInParens.InParameterLists
true`, `SpaceBeforeParensOptions.AfterFunctionDeclarationName false`, and
`ColumnLimit 80` (the line fits). The space before `(`, the spaces inside it,
and each keyword's case are separate keys, and the two presets differ on exactly
one axis: keyword case.
