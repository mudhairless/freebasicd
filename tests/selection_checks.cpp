/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Expand-selection checks for the M13 chain module.
//
// Byte-offset, LSP-agnostic: exercises the token / statement / block / file
// levels, the `:` statement separator, `_` continuations (which the lexer has
// already merged), comments, a blank line, an unterminated block, and the one
// invariant the protocol rests on — every level strictly contains the level
// below it, at every offset of a non-trivial document.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "resolve.h"
#include "selection.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

// The chain as text, one "<beg>-<end>" per level, for readable expectations.
std::string levels(std::vector<SourceRange> const &chain) {
  std::string out;
  for (SourceRange const &r : chain) {
    if (!out.empty()) {
      out += " ";
    }
    out += std::to_string(r.beg) + "-" + std::to_string(r.end);
  }
  return out;
}

// An exact chain, asserted as one string. A mismatch prints what the chain
// really is, so a failing expectation is readable without a debugger: the
// numbers are byte offsets and the string says which levels they are.
void expectChain(char const *what, std::vector<SourceRange> const &chain,
                 char const *expected) {
  std::string const got = levels(chain);
  if (got != expected) {
    std::printf("FAIL %s: chain is \"%s\", expected \"%s\"\n", what,
                got.c_str(), expected);
    ++failures;
  }
}

std::uint32_t at(std::string const &src, std::string_view needle,
                 std::size_t from = 0) {
  std::size_t const hit = src.find(needle, from);
  CHECK(hit != std::string::npos);
  return static_cast<std::uint32_t>(hit);
}

// The invariant, checked over every offset of `src`: non-empty, strictly
// growing, each level containing the one below, and the outermost reaching the
// end of the file's text. A chain that breaks it makes an editor's
// expand-selection shrink, repeat, or stop short, so it is the one property
// worth asserting exhaustively rather than per example.
void checkWellFormed(std::string const &src) {
  AnalyzedDoc const doc = analyze(src);
  for (std::uint32_t off = 0; off <= src.size(); ++off) {
    std::vector<SourceRange> const chain = selectionChain(doc, src, off);
    if (src.empty()) {
      CHECK(chain.empty());
      continue;
    }
    if (chain.empty()) {
      std::printf("FAIL empty chain at offset %u of %zu bytes\n", off,
                  src.size());
      ++failures;
      continue;
    }
    // The outermost level has to cover the file's text; it need not be the
    // whole buffer, since a level that adds nothing but whitespace is dropped
    // as unselectable.
    SourceRange const &outermost = chain.back();
    bool covered = true;
    for (std::uint32_t i = outermost.end; i < src.size(); ++i) {
      if (src[i] != ' ' && src[i] != '\t' && src[i] != '\r' && src[i] != '\n') {
        covered = false;
        break;
      }
    }
    if (!covered) {
      std::printf("FAIL offset %u: outermost level %u-%u leaves text of a "
                  "%zu-byte file uncovered\n",
                  off, outermost.beg, outermost.end, src.size());
      ++failures;
    }
    for (std::size_t i = 0; i < chain.size(); ++i) {
      if (chain[i].end <= chain[i].beg) {
        std::printf("FAIL offset %u: level %zu is empty (%u-%u)\n", off, i,
                    chain[i].beg, chain[i].end);
        ++failures;
      }
      if (i == 0) {
        continue;
      }
      SourceRange const &inner = chain[i - 1];
      SourceRange const &outer = chain[i];
      if (outer.beg > inner.beg || outer.end < inner.end ||
          (outer.beg == inner.beg && outer.end == inner.end)) {
        std::printf("FAIL offset %u: level %zu (%u-%u) does not strictly "
                    "contain level %zu (%u-%u)\n",
                    off, i, outer.beg, outer.end, i - 1, inner.beg, inner.end);
        ++failures;
      }
    }
  }
}

} // namespace

int main() {
  // --- the chain under a nested block -----------------------------------
  //
  //   sub outer()          0
  //     dim total = 0      1
  //     if total > 0 then  2
  //       total = total + 1 3
  //     end if             4
  //   end sub              5
  std::string const src = "sub outer()\n"
                          "  dim total = 0\n"
                          "  if total > 0 then\n"
                          "    total = total + 1\n"
                          "  end if\n"
                          "end sub\n";
  AnalyzedDoc const doc = analyze(src);

  // On `total` inside the if-body: the token, its statement, the if block, the
  // sub block. The file is not a level of its own here — the sub's range stops
  // at the end of `end sub`, so the file would add one newline and nothing
  // else, which is a keystroke that selects the same text.
  std::uint32_t const onTotal = at(src, "total = total + 1");
  expectChain("token in a nested block", selectionChain(doc, src, onTotal),
              "52-57 52-69 30-78 0-86");

  // A cursor at the end of the statement, where it sits while the last
  // character is being typed, still selects the last token.
  std::uint32_t const afterTotal =
      onTotal +
      static_cast<std::uint32_t>(std::string("total = total + 1").size());
  expectChain("just past a token", selectionChain(doc, src, afterTotal),
              "68-69 52-69 30-78 0-86");

  // On the `if` keyword itself: the keyword token, then the whole if statement
  // (single-line `then` header and body are one block), then the sub.
  std::uint32_t const onIf = at(src, "if total");
  expectChain("block keyword", selectionChain(doc, src, onIf),
              "30-32 30-47 30-78 0-86");

  // In the indentation: no token is adjacent, so the chain starts at the
  // statement — which starts at its first token, not at the cursor.
  std::uint32_t const inIndent = at(src, "    total = total + 1") + 2;
  expectChain("indentation of a line", selectionChain(doc, src, inIndent),
              "52-69 30-78 0-86");

  // On a blank line: no token and no statement, so the chain is the file.
  std::string const withBlank = src + "\n";
  AnalyzedDoc const blankDoc = analyze(withBlank);
  expectChain("blank line",
              selectionChain(blankDoc, withBlank,
                             static_cast<std::uint32_t>(src.size())),
              "0-88");

  // --- `:` statement separators ------------------------------------------
  std::string const colonSrc = "sub s()\n"
                               "  a = 1 : b = 2\n"
                               "end sub\n";
  AnalyzedDoc const colonDoc = analyze(colonSrc);
  std::uint32_t const onB = at(colonSrc, "b = 2") + 1;
  expectChain("token after a `:`", selectionChain(colonDoc, colonSrc, onB),
              "18-19 18-23 0-31");
  // The cursor in the gap between the two segments belongs to the one it
  // follows, and the whole line is not a level of its own.
  std::uint32_t const inGap = at(colonSrc, ": b") + 1;
  expectChain("whitespace after a `:`",
              selectionChain(colonDoc, colonSrc, inGap), "18-23 0-31");
  // On the `:` itself the preceding segment is the one that ends there, and the
  // separator is never a level: as the innermost level it is not contained by
  // the statement segment around it, so it would suppress that level instead.
  std::uint32_t const onColon = at(colonSrc, ":");
  expectChain("on a `:`", selectionChain(colonDoc, colonSrc, onColon),
              "10-15 0-31");

  // A `:` inside a string literal is part of the string, never a separator.
  std::string const strSrc = "print \"a: b\"\n";
  AnalyzedDoc const strDoc = analyze(strSrc);
  std::uint32_t const inString = at(strSrc, "a: b") + 2;
  expectChain("`:` inside a string", selectionChain(strDoc, strSrc, inString),
              "6-12 0-12");

  // --- `_` continuations ---------------------------------------------------
  // The lexer merges them, so one continued statement is one level spanning
  // both lines rather than two.
  std::string const contSrc = "sub s()\n"
                              "  x = 1 + _\n"
                              "      2\n"
                              "end sub\n";
  AnalyzedDoc const contDoc = analyze(contSrc);
  std::uint32_t const onTwo = at(contSrc, "      2") + 6;
  expectChain("continued statement, second line",
              selectionChain(contDoc, contSrc, onTwo), "26-27 10-27 0-35");
  std::uint32_t const onOne = at(contSrc, "x = 1");
  expectChain("continued statement, first line",
              selectionChain(contDoc, contSrc, onOne), "10-11 10-27 0-35");

  // --- comments ------------------------------------------------------------
  std::string const commentSrc = "sub s()\n"
                                 "  a = 1 ' note\n"
                                 "end sub\n";
  AnalyzedDoc const commentDoc = analyze(commentSrc);
  std::uint32_t const onComment = at(commentSrc, "' note") + 2;
  expectChain("comment", selectionChain(commentDoc, commentSrc, onComment),
              "16-22 10-22 0-30");

  // --- an unterminated block ------------------------------------------------
  // The closer has not been typed yet, so the parser recorded no range for the
  // block: the level is absent rather than wrong, and the chain still grows —
  // here all the way to the file, whose level adds text the statement lacks.
  std::string const openSrc = "sub s()\n"
                              "  a = 1\n";
  AnalyzedDoc const openDoc = analyze(openSrc);
  std::uint32_t const onA = at(openSrc, "a = 1") + 1;
  expectChain("unterminated block", selectionChain(openDoc, openSrc, onA),
              "10-11 10-15 0-16");

  // --- module level, no enclosing block -------------------------------------
  std::string const modSrc = "dim sharedCounter = 0\n"
                             "dim localOnly = 1\n";
  AnalyzedDoc const modDoc = analyze(modSrc);
  std::uint32_t const onMod = at(modSrc, "localOnly") + 1;
  expectChain("module level", selectionChain(modDoc, modSrc, onMod),
              "26-35 22-39 0-40");

  // --- type bodies ----------------------------------------------------------
  std::string const typeSrc = "type point\n"
                              "  x as integer\n"
                              "end type\n";
  AnalyzedDoc const typeDoc = analyze(typeSrc);
  std::uint32_t const onX = at(typeSrc, "x as integer") + 1;
  expectChain("type body", selectionChain(typeDoc, typeSrc, onX),
              "13-14 13-25 0-34");

  // --- degenerate documents and offsets -------------------------------------
  AnalyzedDoc const emptyDoc = analyze("");
  CHECK(selectionChain(emptyDoc, "", 0).empty());
  AnalyzedDoc const oneDoc = analyze("print 1\n");
  expectChain("one-line document", selectionChain(oneDoc, "print 1\n", 0),
              "0-5 0-7");
  // Past the end of the buffer: no chain at all, rather than a fabricated one.
  CHECK(selectionChain(oneDoc, "print 1\n", 9).empty());

  // --- the invariant, exhaustively ------------------------------------------
  checkWellFormed(src);
  checkWellFormed(colonSrc);
  checkWellFormed(contSrc);
  checkWellFormed(commentSrc);
  checkWellFormed(typeSrc);
  checkWellFormed(openSrc);
  checkWellFormed("");
  std::string const kitchen = "#include \"lib.bi\"\n"
                              "#if defined(__FB_PLATFORM__)\n"
                              "type vec\n"
                              "  x as float\n"
                              "  declare sub init()\n"
                              "end type\n"
                              "sub main()\n"
                              "  dim v as vec\n"
                              "  v.init()\n"
                              "  for i = 0 to 9\n"
                              "    if i mod 2 = 0 then\n"
                              "      print i\n"
                              "    else\n"
                              "      print \"odd\" : i += 1\n"
                              "    end if\n"
                              "  next\n"
                              "  with v\n"
                              "    .x = 1.0\n"
                              "  end with\n"
                              "end sub\n"
                              "\n"
                              "' trailing comment\n";
  checkWellFormed(kitchen);

  if (failures == 0) {
    std::printf("selection_checks: all passed\n");
    return 0;
  }
  std::printf("selection_checks: %d failures\n", failures);
  return 1;
}
