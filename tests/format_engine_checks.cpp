/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// format_engine_checks: the formatter engine. LSP-agnostic: formatDocument
// over literal inputs with explicit FormatConfig values, asserting full
// expected outputs — indentation from the block stack, spacing rules, case
// transforms against the language catalogs, and the verbatim-preservation
// invariants (comments, strings, directives, macro and asm bodies, format-off
// regions, continuation lines, elided argument slots, number literals). Every
// test pins bytes, and the idempotence test asserts format(format(x)) ==
// format(x).

#include <cstdio>
#include <string>

#include "format/config.h"
#include "format/engine.h"
#include "format/spelling.h"

using namespace fbformat;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    if (!((a) == (b))) {                                                       \
      std::printf(                                                             \
          "FAIL %s:%d: %s != %s\n  actual:   [%s]\n  expected: [%s]\n",        \
          __FILE__, __LINE__, #a, #b, (a).c_str(), (b).c_str());               \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

// Two-space space indentation for readable expectations; case transforms
// stay Leave unless a test asks for them.
static FormatConfig spaceConfig() {
  FormatConfig c;
  c.useTab = UseTab::Never;
  c.indentWidth = 2;
  c.tabWidth = 2;
  return c;
}

static std::string fmt(std::string const &src, FormatConfig const &c) {
  return formatDocument(src, "test.bas", c);
}

// Indentation: the block stack from fblang's opener/closer tables — sub,
// if/else/end if, for/next — with Else and End If dedented to their opener.
static void TestIndentBlocks() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "x=1\n"
                         "if a then\n"
                         "y=2\n"
                         "else\n"
                         "y=3\n"
                         "end if\n"
                         "for i=1 to 2\n"
                         "z=i\n"
                         "next\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  x = 1\n"
                           "  if a then\n"
                           "    y = 2\n"
                           "  else\n"
                           "    y = 3\n"
                           "  end if\n"
                           "  for i = 1 to 2\n"
                           "    z = i\n"
                           "  next\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// `If cond Then stmt` is one line and opens nothing; only a Then that ends
// its line pushes the block.
static void TestOneLineIfDoesNotPush() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "if a then b\n"
                         "x=1\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  if a then b\n"
                           "  x = 1\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// Select: Case labels sit at the Select's own depth, bodies one deeper. The
// merged EndIf spelling closes an If exactly like End If.
static void TestSelectCaseAndEndIf() {
  FormatConfig c = spaceConfig();
  std::string const in = "select case x\n"
                         "case 1\n"
                         "a=1\n"
                         "case is > 5\n"
                         "b=2\n"
                         "end select\n"
                         "if a then\n"
                         "b=1\n"
                         "endif\n";
  std::string const want = "select case x\n"
                           "case 1\n"
                           "  a = 1\n"
                           "case is > 5\n"
                           "  b = 2\n"
                           "end select\n"
                           "if a then\n"
                           "  b = 1\n"
                           "endif\n";
  CHECK_EQ(fmt(in, c), want);
}

// A UDT field named after a closer word (`Next As Node Ptr`, FreeBASIC.md
// §7) is not a closer: it keeps the body indent and pops nothing.
static void TestFieldNamedCloser() {
  FormatConfig c = spaceConfig();
  std::string const in = "type node\n"
                         "next as node ptr\n"
                         "end type\n";
  std::string const want = "type node\n"
                           "  next as node ptr\n"
                           "end type\n";
  CHECK_EQ(fmt(in, c), want);
}

// Unbalanced code must not crash or drift below column zero.
static void TestMalformedBlocksAreTolerated() {
  FormatConfig c = spaceConfig();
  std::string const in = "end if\n"
                         "next\n"
                         "x=1\n";
  std::string const want = "end if\n"
                           "next\n"
                           "x = 1\n";
  CHECK_EQ(fmt(in, c), want);
}

// `Extern name As type` declares a variable shared across modules and opens
// nothing (FreeBASIC.md §7); only `Extern "<cc>"` opens the block that
// `End Extern` closes. The declaration form used to push an Extern for any
// statement starting with the word, so every line after it indented one
// level, the real block's body sat two deep, and no later line dedented.
static void TestExternDeclarationDoesNotOpenBlock() {
  FormatConfig c = spaceConfig();
  std::string const in = "extern sharedCount as integer\n"
                         "\n"
                         "dim y as integer\n"
                         "y = 1\n"
                         "\n"
                         "extern \"C\"\n"
                         "declare function foo() as integer\n"
                         "end extern\n"
                         "\n"
                         "dim z as integer\n"
                         "z = 2\n";
  std::string const want = "extern sharedCount as integer\n"
                           "\n"
                           "dim y as integer\n"
                           "y = 1\n"
                           "\n"
                           "extern \"C\"\n"
                           "  declare function foo() as integer\n"
                           "end extern\n"
                           "\n"
                           "dim z as integer\n"
                           "z = 2\n";
  std::string const once = fmt(in, c);
  CHECK_EQ(once, want);
  CHECK_EQ(fmt(once, c), once);
}

// The block form alone: `Extern "C"` still opens, its body indents one
// level, `End Extern` dedents, and the line after is back at the base.
static void TestExternBlockFormStillIndents() {
  FormatConfig c = spaceConfig();
  std::string const in = "extern \"C\"\n"
                         "declare sub init()\n"
                         "end extern\n"
                         "x = 1\n";
  std::string const want = "extern \"C\"\n"
                           "  declare sub init()\n"
                           "end extern\n"
                           "x = 1\n";
  CHECK_EQ(fmt(in, c), want);
}

// The declaration form as one statement of a colon-compound line: the
// walker applies each statement in order, and neither the rest of the line
// nor the next line may inherit an Extern from it.
static void TestCompoundExternDeclarationLineDoesNotLeak() {
  FormatConfig c = spaceConfig();
  std::string const in = "extern sharedCount as integer : y = 1\n"
                         "z = 2\n";
  std::string const want = "extern sharedCount as integer: y = 1\n"
                           "z = 2\n";
  CHECK_EQ(fmt(in, c), want);
}

// A bare `Extern` with nothing after it is fbc's error 14, never a block:
// the string-literal test covers it, so the next line keeps its indent.
static void TestBareExternIsNotABlock() {
  FormatConfig c = spaceConfig();
  std::string const in = "extern\n"
                         "x = 1\n";
  std::string const want = "extern\n"
                           "x = 1\n";
  CHECK_EQ(fmt(in, c), want);
}

// The core spacing rules: assignment and binary operators spaced, commas
// after-only, Print semicolons tight, the operand of a sign attached, word
// operators spaced, and the combined assign (`and=`) kept one token wide.
static void TestSpacingBasics() {
  FormatConfig c = spaceConfig();
  std::string const in = "x=1+2\n"
                         "print \"a\";\"b\"\n"
                         "f(a,b)\n"
                         "y = -1\n"
                         "z = a - b\n"
                         "w = a + -b\n"
                         "v = not q\n"
                         "u = t and= 5\n";
  std::string const want = "x = 1 + 2\n"
                           "print \"a\";\"b\"\n"
                           "f(a, b)\n"
                           "y = -1\n"
                           "z = a - b\n"
                           "w = a + -b\n"
                           "v = not q\n"
                           "u = t and= 5\n";
  CHECK_EQ(fmt(in, c), want);
}

// A statement keyword that introduces a value or value list makes a directly
// following -/+ a sign, because the keyword is not a value, so the sign can
// never be binary there. `data` is the list shape and must keep the sign
// attached in every slot — first item and after a comma, however the source
// spaced it; `sleep` and `randomize` take one value whose documented default
// is negative (`Sleep -1`, `Randomize -1.0`). The formatted list is a fixed
// point. Statement indentation after a label is a separate open issue, so
// this pins the value lists at the top level only.
static void TestSignedLiteralAfterValueKeyword() {
  FormatConfig c = spaceConfig();
  std::string const in = "ship_data:\n"
                         "\n"
                         "data 3\n"
                         "data 0,-13\n"
                         "data -5, 13\n"
                         "data -5,13\n"
                         "data - 5, 13\n"
                         "data 0, 5\n"
                         "sleep -1\n"
                         "randomize -1\n"
                         "print -5\n";
  std::string const want = "ship_data:\n"
                           "\n"
                           "data 3\n"
                           "data 0, -13\n"
                           "data -5, 13\n"
                           "data -5, 13\n"
                           "data -5, 13\n"
                           "data 0, 5\n"
                           "sleep -1\n"
                           "randomize -1\n"
                           "print -5\n";
  std::string const once = fmt(in, c);
  CHECK_EQ(once, want);
  CHECK_EQ(fmt(once, c), once);
}

// Binary minus between two operands keeps the configured operator spacing
// whatever the author's gaps were, and the unary-context words above do not
// leak into it: only a sign directly after the word is a sign.
static void TestBinaryMinusUnchanged() {
  FormatConfig c = spaceConfig();
  CHECK_EQ(fmt("a-5\n", c), std::string("a - 5\n"));
  CHECK_EQ(fmt("a -5\n", c), std::string("a - 5\n"));
  CHECK_EQ(fmt("a- 5\n", c), std::string("a - 5\n"));
  CHECK_EQ(fmt("print a-5\n", c), std::string("print a - 5\n"));
  CHECK_EQ(fmt("x = 10- z\n", c), std::string("x = 10 - z\n"));
}

// The fbc-style paren overrides: inner spaces in conditionals and parameter
// lists, a space after the control keyword, tight call parens. Both
// polarities of the control-statement space flag are pinned, because the
// context mapping is what could silently regress.
static void TestParenContexts() {
  FormatConfig c = spaceConfig();
  c.spaceBeforeParensAfterControlStatements = true;
  c.spacesInParensInConditionalStatements = true;
  c.spacesInParensInParameterLists = true;
  std::string const in = "if( a ) then\n"
                         "sub f( x as integer )\n"
                         "g(x)\n"
                         "end sub\n"
                         "end if\n";
  std::string const want = "if ( a ) then\n"
                           "  sub f( x as integer )\n"
                           "    g(x)\n"
                           "  end sub\n"
                           "end if\n";
  CHECK_EQ(fmt(in, c), want);

  c.spaceBeforeParensAfterControlStatements = false;
  CHECK_EQ(fmt("if( a ) then\n", c), std::string("if( a ) then\n"));
}

// Canonical case is the documented spelling — ElseIf, End If, LBound — not
// bare TitleCase (which would produce Elseif). ElseIf and End If sit at their
// opener's depth, the same alignment TestIndentBlocks pins for Else.
static void TestCanonicalKeywordCase() {
  FormatConfig c = spaceConfig();
  c.keywordCase = CaseStyle::Canonical;
  c.intrinsicCase = CaseStyle::Canonical;
  std::string const in = "if a then\n"
                         "elseif b then\n"
                         "end if\n"
                         "x = lbound(y)\n";
  std::string const want = "If a Then\n"
                           "ElseIf b Then\n"
                           "End If\n"
                           "x = LBound(y)\n";
  CHECK_EQ(fmt(in, c), want);
}

static void TestCaseStyles() {
  // Case transforms apply to catalog words only — an identifier keeps the
  // bytes it was written with in every style (renaming is a different
  // feature's job).
  FormatConfig c = spaceConfig();
  c.keywordCase = CaseStyle::Title;
  CHECK_EQ(fmt("elseif a then\n", c), std::string("Elseif a Then\n"));
  c.keywordCase = CaseStyle::lower;
  CHECK_EQ(fmt("ELSEIF A THEN\n", c), std::string("elseif A then\n"));
  c.keywordCase = CaseStyle::UPPER;
  CHECK_EQ(fmt("if a then\n", c), std::string("IF a THEN\n"));
  c.keywordCase = CaseStyle::Leave;
  CHECK_EQ(fmt("If a Then\n", c), std::string("If a Then\n"));
}

// Precedence by specificity: a builtin type wins over the intrinsic catalog
// (bare `string` is the type; `Left$` the intrinsic), and boolean literals
// have their own switch.
static void TestTypeIntrinsicBooleanPrecedence() {
  FormatConfig c = spaceConfig();
  c.typeNameCase = CaseStyle::Title;
  c.intrinsicCase = CaseStyle::lower;
  c.keywordCase = CaseStyle::lower;
  std::string const in = "dim s as string\n"
                         "print left$(\"a\",1)\n";
  std::string const want = "dim s as String\n"
                           "print left$(\"a\", 1)\n";
  CHECK_EQ(fmt(in, c), want);

  FormatConfig b = spaceConfig();
  b.booleanLiteralCase = CaseStyle::UPPER;
  CHECK_EQ(fmt("x = true and false\n", b), std::string("x = TRUE and FALSE\n"));
}

// Preservation invariants: strings, comments, and doc blocks keep their
// bytes; only placement changes.
static void TestCommentsAndStringsVerbatim() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "s = \"a \"\"quote\"\" here\"\n"
                         "'dont touch me\n"
                         "/'multi\n"
                         "   line'/\n"
                         "x=1  'note   here\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  s = \"a \"\"quote\"\" here\"\n"
                           "  'dont touch me\n"
                           "  /'multi\n"
                           "   line'/\n"
                           "  x = 1 'note   here\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// Directive lines sit at column 0 with DirectiveCase::Leave, and a
// directive ending in `_` puts its macro body through raw.
static void TestDirectivesAndMacroBodies() {
  FormatConfig c = spaceConfig();
  std::string const in = "#define FOO 1\n"
                         "#DEFINE BAR(x) _\n"
                         "  ((x)+1)\n"
                         "dim y as integer\n";
  CHECK_EQ(fmt(in, c), in);

  c.directiveCase = CaseStyle::Title;
  std::string const want = "#Define FOO 1\n"
                           "#Define BAR(x) _\n"
                           "  ((x)+1)\n"
                           "dim y as integer\n";
  CHECK_EQ(fmt(in, c), want);
}

// A `'$...` metacommand comment is a comment and stays verbatim.
static void TestMetacommandCommentVerbatim() {
  FormatConfig c = spaceConfig();
  c.metacommandCase = CaseStyle::UPPER;
  CHECK_EQ(fmt("sub f()\n'$dynamic\nx=1\nend sub\n", c),
           std::string("sub f()\n  '$dynamic\n  x = 1\nend sub\n"));
}

// ContinuationStyle::Leave: a line continuing a `_`-terminated logical line
// is copied byte for byte, author alignment intact.
static void TestContinuationLinesVerbatim() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f(x as integer,_\n"
                         "      y as integer)\n"
                         "x = y\n"
                         "end sub\n";
  std::string const want = "sub f(x as integer, _\n"
                           "      y as integer)\n"
                           "  x = y\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// Blank runs collapse to MaxEmptyLinesToKeep; trailing whitespace goes; the
// final newline is normalized to one.
static void TestBlankLinesAndDocumentTail() {
  FormatConfig c = spaceConfig();
  c.maxEmptyLinesToKeep = 1;
  CHECK_EQ(fmt("a=1\n\n\n\n\nb=2\n", c), std::string("a = 1\n\nb = 2\n"));
  CHECK_EQ(fmt("a=1   \n\n", c), std::string("a = 1\n"));
  CHECK_EQ(fmt("a=1", c), std::string("a = 1\n"));
}

// Number literals are bytes, never respelled — and the float/hex forms are
// the regression for the tokenizer's zero-length-token loop (1.5, .25, 1.,
// &H1F each lex as one token and must round-trip).
static void TestNumbersVerbatim() {
  FormatConfig c = spaceConfig();
  CHECK_EQ(fmt("x = &H1F + 1.5 + .25 + 2e3 + 1.\n", c),
           std::string("x = &H1F + 1.5 + .25 + 2e3 + 1.\n"));
}

// Goto labels per IndentGotoLabels: None puts them at column zero.
static void TestGotoLabels() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "goto skip\n"
                         "skip:\n"
                         "x=1\n"
                         "skip2: y=2\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  goto skip\n"
                           "skip:\n"
                           "  x = 1\n"
                           "skip2: y = 2\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// Graphics: coordinate pairs keep the author's sign tightness, and elided
// argument slots (`,, , ,`) keep the author's spacing — they are structure.
static void TestGraphicsCoordinatePairsAndSlots() {
  FormatConfig c = spaceConfig();
  CHECK_EQ(fmt("line -(10,10)-(20,20),,b\n", c),
           std::string("line -(10, 10)-(20, 20),, b\n"));
  CHECK_EQ(fmt("circle (160,100),50,2,, , ,1.5\n", c),
           std::string("circle(160, 100), 50, 2,, , , 1.5\n"));
}

// A trailing comment rides at SpacesBeforeTrailingComments.
static void TestTrailingCommentGap() {
  FormatConfig c = spaceConfig();
  CHECK_EQ(fmt("x=1  'note\n", c), std::string("x = 1 'note\n"));
  c.spacesBeforeTrailingComments = 2;
  CHECK_EQ(fmt("x=1  'note\n", c), std::string("x = 1  'note\n"));
}

// DeriveLF keeps a CRLF file CRLF and an LF file LF.
static void TestLineEndingDerivation() {
  FormatConfig c = spaceConfig();
  CHECK_EQ(fmt("sub f()\r\nx=1\r\nend sub\r\n", c),
           std::string("sub f()\r\n  x = 1\r\nend sub\r\n"));
  CHECK_EQ(fmt("x=1\n", c), std::string("x = 1\n"));
}

// Format-off/on pragmas: the opening line, every line between, and the
// closing line are copied from the original bytes (indent and trailing
// whitespace included), while code outside the region still formats. A
// second `off` inside the region is state, not depth, and is ignored.
static void TestFormatOffRegionPreserved() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "  ' fbformat: off\n"
                         "   x=1   \n"
                         "  ' fbformat: off\n"
                         "     junk   \n"
                         "  ' fbformat: on\n"
                         "y=2\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  ' fbformat: off\n"
                           "   x=1   \n"
                           "  ' fbformat: off\n"
                           "     junk   \n"
                           "  ' fbformat: on\n"
                           "  y = 2\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);

  // An empty pragma must never match: `find("")` would otherwise turn every
  // comment line into a format-off switch.
  FormatConfig emptyOff = spaceConfig();
  emptyOff.formatOffPragma.clear();
  CHECK_EQ(fmt("sub f()\n' note\nx=1\nend sub\n", emptyOff),
           std::string("sub f()\n  ' note\n  x = 1\nend sub\n"));
}

// An unclosed off region runs to end of file; the document tail still gets
// its one final newline, but no raw line is rewritten.
static void TestFormatOffToEof() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "  x=1\n"
                         "  ' fbformat: off\n"
                         "   y=2   \n"
                         "     z=3\n";
  std::string const want = "sub f()\n"
                           "  x = 1\n"
                           "  ' fbformat: off\n"
                           "   y=2   \n"
                           "     z=3\n";
  CHECK_EQ(fmt(in, c), want);
}

// A `#macro` body is opaque until `#endmacro`: raw bytes, and no block-stack
// effect (a body `loop` must not pop the enclosing `Do`, a body `if` must not
// push an `If`). The delimiters themselves are directive lines, so
// DirectiveCase still applies to them.
static void TestMacroBodyVerbatim() {
  FormatConfig c = spaceConfig();
  c.directiveCase = CaseStyle::Title;
  std::string const in = "do\n"
                         "#macro m(x)\n"
                         "    loop\n"
                         "    if x then\n"
                         "#endmacro\n"
                         "x=1\n"
                         "loop\n";
  std::string const want = "do\n"
                           "#Macro m(x)\n"
                           "    loop\n"
                           "    if x then\n"
                           "#Endmacro\n"
                           "  x = 1\n"
                           "loop\n";
  CHECK_EQ(fmt(in, c), want);
}

// An `Asm ... End Asm` body is raw and does not touch the stack; the opener
// and closer are ordinary code lines, so they take the block indent.
static void TestAsmBodyVerbatim() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "asm\n"
                         "    mov eax, 0\n"
                         "  x=1   \n"
                         "end asm\n"
                         "x=2\n"
                         "end sub\n";
  std::string const want = "sub f()\n"
                           "  asm\n"
                           "    mov eax, 0\n"
                           "  x=1   \n"
                           "  end asm\n"
                           "  x = 2\n"
                           "end sub\n";
  CHECK_EQ(fmt(in, c), want);
}

// Bare `End` terminates the program; it is not `End <block>` and must not
// dedent or pop the enclosing Scope. `End As Integer` remains a UDT field.
static void TestBareEndIsAStatement() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "end\n"
                         "end sub\n"
                         "type t\n"
                         "end as integer\n"
                         "end type\n";
  std::string const want = "sub f()\n"
                           "  end\n"
                           "end sub\n"
                           "type t\n"
                           "  end as integer\n"
                           "end type\n";
  CHECK_EQ(fmt(in, c), want);
  CHECK_EQ(fmt("end\n", c), std::string("end\n"));
}

static void TestDisableFormat() {
  FormatConfig c = spaceConfig();
  c.disableFormat = true;
  std::string const in = "sub f()\r\n   x=1  \n";
  CHECK_EQ(fmt(in, c), in);
}

// The canonical table's contract: exceptions where they exist, "" (meaning
// TitleCase) where they don't, and the keep-lowercase entries protected.
static void TestSpellingTable() {
  CHECK_EQ(std::string(canonicalSpelling("elseif")), std::string("ElseIf"));
  CHECK_EQ(std::string(canonicalSpelling("lbound")), std::string("LBound"));
  CHECK_EQ(std::string(canonicalSpelling("endif")), std::string("EndIf"));
  CHECK_EQ(std::string(canonicalSpelling("ulongint")), std::string("ULongInt"));
  CHECK_EQ(std::string(canonicalSpelling("cdecl")), std::string("cdecl"));
  CHECK_EQ(std::string(canonicalSpelling("va_first")), std::string("va_first"));
  CHECK(canonicalSpelling("goto").empty());
  CHECK(canonicalSpelling("notarealword").empty());
}

// A kitchen-sink document formatted twice is the first output: every rule
// must be a fixed point of its own emission.
static void TestIdempotence() {
  FormatConfig c = spaceConfig();
  c.keywordCase = CaseStyle::Canonical;
  std::string const in = "#define M(x) _\n"
                         "  ((x))\n"
                         "#macro SAY(w)\n"
                         "    print w   \n"
                         "#endmacro\n"
                         "' fbformat: off\n"
                         "   raw=1   \n"
                         "' fbformat: on\n"
                         "sub f( byval x as integer )\n"
                         "dim y as string\n"
                         "asm\n"
                         "    mov eax, 0\n"
                         "end asm\n"
                         "if x>0 then\n"
                         "y = str$(x) + \"!\"\n"
                         "select case x\n"
                         "case is > 5\n"
                         "print \"big\";'note\n"
                         "case else\n"
                         "x = x + -1\n"
                         "end select\n"
                         "else\n"
                         "goto done\n"
                         "endif\n"
                         "done:\n"
                         "end\n"
                         "end sub\n";
  std::string const once = fmt(in, c);
  CHECK_EQ(fmt(once, c), once);
}

// --- range formatting (§9.6) ----------------------------------------------

static RangeResult rangeOf(std::string const &src, FormatConfig const &c,
                           size_t start, size_t end) {
  return formatRange(src, "test.bas", c, start, end);
}

static std::string applyRange(std::string const &src, RangeResult const &r) {
  if (!r.changed) {
    return src;
  }
  return src.substr(0, r.replacementStart) + r.text +
         src.substr(r.replacementEnd);
}

// A range over one line in the middle of nested blocks: the edit is that
// line alone, at the depth the prefix leaves. The surrounding lines are
// already formatted, so applying the edit reproduces the full format.
static void TestRangeMiddleOfBlock() {
  FormatConfig c = spaceConfig();
  std::string const in =
      "sub f()\n  x = 1\n  if a then\ny=2\n  end if\nend sub\n";
  size_t const start = in.find("y=2");
  RangeResult const r = rangeOf(in, c, start, start + 3);
  CHECK(r.changed);
  CHECK(r.replacementStart == start);
  CHECK(r.replacementEnd == in.find("\n  end if") + 1);
  CHECK_EQ(r.text, std::string("    y = 2\n"));
  CHECK_EQ(applyRange(in, r), fmt(in, c));
}

// The dedent of a closer comes from the dry-run stack, not the raw line:
// selecting only `end if` still lands it at the If body's parent depth.
static void TestRangeIndentContextFromPrefix() {
  FormatConfig c = spaceConfig();
  std::string const in =
      "sub f()\n  x = 1\n  if a then\n    y = 2\nend if\nend sub\n";
  size_t const start = in.find("end if");
  RangeResult const r = rangeOf(in, c, start, start + 6);
  CHECK(r.changed);
  CHECK_EQ(r.text, std::string("  end if\n"));
  CHECK_EQ(applyRange(in, r), fmt(in, c));
}

// A range that touches the inside of a block comment expands to the whole
// lines the comment token spans.
static void TestRangeBlockCommentBoundary() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n/' one\ntwo '/\n  x = 1\nend sub\n";
  size_t const start = in.find("two");
  RangeResult const r = rangeOf(in, c, start, start);
  CHECK(r.changed);
  CHECK(r.replacementStart == in.find("/'"));
  CHECK_EQ(applyRange(in, r), fmt(in, c));
}

// Selecting the tail of a `_` chain pulls the window back to the chain
// head, so the head formats and the continuation lines stay verbatim.
static void TestRangeContinuationHead() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\nx = 1 + _\n2 + _\n3\nend sub\n";
  size_t const start = in.find("\n3\n") + 1;
  RangeResult const r = rangeOf(in, c, start, start + 1);
  CHECK(r.changed);
  CHECK(r.replacementStart == in.find("x = 1"));
  CHECK_EQ(applyRange(in, r), fmt(in, c));
}

// A range over blank lines only has nothing to format.
static void TestRangeBlankOnlyIsNoOp() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n\n\nx=1\nend sub\n";
  size_t const start = in.find("x=1") - 2;
  RangeResult const r = rangeOf(in, c, start, start + 2);
  CHECK(!r.changed);
}

// A range over a line that already reads as formatted changes nothing.
static void TestRangeAlreadyFormattedIsNoOp() {
  FormatConfig c = spaceConfig();
  std::string const in =
      fmt("sub f()\nx=1\nif a then\ny=2\nend if\nend sub\n", c);
  size_t const start = in.find("y = 2");
  RangeResult const r = rangeOf(in, c, start, start + 5);
  CHECK(!r.changed);
}

// A mid-document range does not normalize the end of the file: the missing
// final newline stays missing and the unformatted closer stays untouched.
static void TestRangeMidDocumentKeepsEofAlone() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\nx=1\nend sub";
  size_t const start = in.find("x=1");
  RangeResult const r = rangeOf(in, c, start, start + 3);
  CHECK(r.changed);
  CHECK(r.replacementEnd == in.find("end sub"));
  CHECK_EQ(applyRange(in, r), std::string("sub f()\n  x = 1\nend sub"));
}

// A block that opens and closes on one colon-separated line nets to zero
// on the block stack: the `Next` after the colon closes the `For` the line
// opened, so the enclosing `End Sub` still pops and the statement after
// the sub sits at depth 0. Reading only the first statement leaked the
// `For`, and every line after the sub stayed indented.
static void TestCompoundForLineDoesNotLeak() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "dim as integer used(0 to 3)\n"
                         "for i as integer = 0 to 3 : used(i) = 0 : next\n"
                         "end sub\n"
                         "dim as integer sentinel\n";
  std::string const want = "sub f()\n"
                           "  dim as integer used(0 to 3)\n"
                           "  for i as integer = 0 to 3: used(i) = 0: next\n"
                           "end sub\n"
                           "dim as integer sentinel\n";
  CHECK_EQ(fmt(in, c), want);
}

// The drd engine.bas failure in miniature: the leaked `For` also blocked
// the enclosing blocks' closers, because a closer pops only the matching
// kind. With the compound line applied in full, `Next` closes the outer
// `For`, `End With` and `End Sub` pop, and the trailing statement is at
// depth 0 instead of depth 3.
static void TestCompoundForInsideWithAndFor() {
  FormatConfig c = spaceConfig();
  std::string const in = "sub f()\n"
                         "with m\n"
                         "for s as integer = 0 to 2\n"
                         "for i as integer = 0 to 1 : used(i) = 0 : next\n"
                         "next\n"
                         "end with\n"
                         "end sub\n"
                         "dim as integer sentinel\n";
  std::string const want = "sub f()\n"
                           "  with m\n"
                           "    for s as integer = 0 to 2\n"
                           "      for i as integer = 0 to 1: used(i) = 0: "
                           "next\n"
                           "    next\n"
                           "  end with\n"
                           "end sub\n"
                           "dim as integer sentinel\n";
  CHECK_EQ(fmt(in, c), want);
}

// The same one-line compound shape for the other bare closers: `While`
// closed by `Wend` and `Do` closed by `Loop` on the same line.
static void TestCompoundWhileAndDoLines() {
  FormatConfig c = spaceConfig();
  std::string const in = "function f() as integer\n"
                         "dim as integer n = 0\n"
                         "while n < 3 : n = n + 1 : wend\n"
                         "do : n = n - 1 : loop while n > 0\n"
                         "return n\n"
                         "end function\n"
                         "dim as integer sentinel\n";
  std::string const want = "function f() as integer\n"
                           "  dim as integer n = 0\n"
                           "  while n < 3: n = n + 1: wend\n"
                           "  do: n = n - 1: loop while n > 0\n"
                           "  return n\n"
                           "end function\n"
                           "dim as integer sentinel\n";
  CHECK_EQ(fmt(in, c), want);
}

int main() {
  TestIndentBlocks();
  TestOneLineIfDoesNotPush();
  TestSelectCaseAndEndIf();
  TestFieldNamedCloser();
  TestMalformedBlocksAreTolerated();
  TestExternDeclarationDoesNotOpenBlock();
  TestExternBlockFormStillIndents();
  TestCompoundExternDeclarationLineDoesNotLeak();
  TestBareExternIsNotABlock();
  TestSpacingBasics();
  TestSignedLiteralAfterValueKeyword();
  TestBinaryMinusUnchanged();
  TestParenContexts();
  TestCanonicalKeywordCase();
  TestCaseStyles();
  TestTypeIntrinsicBooleanPrecedence();
  TestCommentsAndStringsVerbatim();
  TestDirectivesAndMacroBodies();
  TestMetacommandCommentVerbatim();
  TestContinuationLinesVerbatim();
  TestBlankLinesAndDocumentTail();
  TestNumbersVerbatim();
  TestGotoLabels();
  TestGraphicsCoordinatePairsAndSlots();
  TestTrailingCommentGap();
  TestLineEndingDerivation();
  TestDisableFormat();
  TestFormatOffRegionPreserved();
  TestFormatOffToEof();
  TestMacroBodyVerbatim();
  TestAsmBodyVerbatim();
  TestBareEndIsAStatement();
  TestSpellingTable();
  TestIdempotence();
  TestRangeMiddleOfBlock();
  TestRangeIndentContextFromPrefix();
  TestRangeBlockCommentBoundary();
  TestRangeContinuationHead();
  TestRangeBlankOnlyIsNoOp();
  TestRangeAlreadyFormattedIsNoOp();
  TestRangeMidDocumentKeepsEofAlone();
  TestCompoundForLineDoesNotLeak();
  TestCompoundForInsideWithAndFor();
  TestCompoundWhileAndDoLines();

  if (failures != 0) {
    std::printf("format_engine_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("format_engine_checks: all passed\n");
  return 0;
}
