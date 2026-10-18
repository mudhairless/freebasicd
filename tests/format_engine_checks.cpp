/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// format_engine_checks: the M17 formatter engine. LSP-agnostic: formatDocument
// over literal inputs with explicit FormatConfig values, asserting full
// expected outputs — indentation from the block stack, spacing rules, case
// transforms against the language catalogs, and the verbatim-preservation
// invariants (comments, strings, directives, macro bodies, continuation
// lines, elided argument slots, number literals). Every test pins bytes, and
// the idempotence test asserts format(format(x)) == format(x).

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
                         "sub f( byval x as integer )\n"
                         "dim y as string\n"
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
                         "end sub\n";
  std::string const once = fmt(in, c);
  CHECK_EQ(fmt(once, c), once);
}

int main() {
  TestIndentBlocks();
  TestOneLineIfDoesNotPush();
  TestSelectCaseAndEndIf();
  TestFieldNamedCloser();
  TestMalformedBlocksAreTolerated();
  TestSpacingBasics();
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
  TestSpellingTable();
  TestIdempotence();

  if (failures != 0) {
    std::printf("format_engine_checks: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("format_engine_checks: all passed\n");
  return 0;
}
