/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "selection.h"

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "lexer.h"
#include "resolve.h"
#include "symbols.h"

namespace fblang {
namespace {

// The `:` that separates two statements on one logical line. It reaches the
// token stream as a one-byte Symbol token (lexer.cpp lexSymbol's default arm),
// and a colon inside a string literal is part of that String token, so it can
// never be mistaken for a separator here.
bool isSeparator(Token const &t) {
  return t.kind == TokenKind::Symbol && t.end == t.beg + 1 && t.data[0] == ':';
}

// True when everything in `src[beg, end)` is whitespace — the test for "this
// level adds nothing the user can see".
bool isBlank(std::string_view src, std::size_t beg, std::size_t end) {
  for (std::size_t i = beg; i < end && i < src.size(); ++i) {
    if (src[i] != ' ' && src[i] != '\t' && src[i] != '\r' && src[i] != '\n') {
      return false;
    }
  }
  return true;
}

// One chain level, kept only when it strictly contains the level before it and
// adds some text to it. The protocol requires `parent.range` to contain
// `this.range`, and an editor that expands through a level which repeats,
// shrinks, or only adds whitespace looks broken — so such a level is dropped,
// not repaired. The two tests reduce to one, because a range that contains
// another can only add text at its front and at its back; a duplicate level
// adds nothing at either and so falls out of the same check.
void pushLevel(std::vector<SourceRange> &chain, SourceRange r,
               std::string_view src) {
  if (r.end <= r.beg) {
    return;
  }
  if (!chain.empty()) {
    SourceRange const &last = chain.back();
    if (r.beg > last.beg || r.end < last.end) {
      return; // not a superset of the level below it
    }
    if (isBlank(src, r.beg, last.beg) && isBlank(src, last.end, r.end)) {
      return; // same text, or more of it with nothing but whitespace
    }
  }
  chain.push_back(r);
}

} // namespace

std::vector<SourceRange> selectionChain(AnalyzedDoc const &doc,
                                        std::string_view content,
                                        std::uint32_t off) {
  std::vector<SourceRange> chain;
  if (content.empty() || off > content.size()) {
    return chain;
  }
  std::vector<Token> const &toks = doc.tokens;

  // The logical line holding `off` is the run of tokens between the enclosing
  // Newline tokens, and the lexer has already merged `_` continuations — so one
  // run is one statement however it is broken across lines. A blank line is a
  // run of no tokens, which is why the bounds are indices and not offsets.
  // A Newline token that *ends* at `off` is the one before the cursor (the
  // cursor sits at the end of that line), and one that *begins* at `off` is the
  // one after it (the cursor sits at the start of the next line).
  std::size_t runFirst = 0;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].beg > off) {
      break; // ordered by offset: nothing later can end at or before the cursor
    }
    if (toks[i].kind == TokenKind::Newline && toks[i].end <= off) {
      runFirst = i + 1;
    }
  }
  std::size_t runLast = runFirst;
  while (runLast < toks.size() && toks[runLast].kind != TokenKind::Newline &&
         toks[runLast].kind != TokenKind::Eof) {
    ++runLast;
  }

  // Innermost level: the token the offset sits in, or — an offset just past a
  // token, which is where a cursor is while that token is being typed — the
  // one ending exactly there. Whitespace between tokens has no token level and
  // starts the chain at the statement. A `:` is never a level: it is a
  // separator, and as the innermost level it would *not* be contained by the
  // statement segment around it, so it would suppress that level instead.
  bool tokenLevel = false;
  for (std::size_t i = runFirst; i < runLast; ++i) {
    if (isSeparator(toks[i])) {
      continue;
    }
    if (toks[i].beg <= off && off < toks[i].end) {
      pushLevel(chain, {toks[i].beg, toks[i].end}, content);
      tokenLevel = true;
      break;
    }
  }
  if (!tokenLevel && runFirst < runLast) {
    for (std::size_t i = runFirst; i < runLast; ++i) {
      if (isSeparator(toks[i])) {
        continue;
      }
      if (toks[i].end == off) {
        pushLevel(chain, {toks[i].beg, toks[i].end}, content);
        break;
      }
    }
  }

  // Statement level: the `:`-separated segment of the run the offset is on,
  // decided by offset so a cursor in the whitespace between two segments lands
  // in the one it follows. A trailing `:` with nothing after it leaves the
  // segment empty and pushLevel drops the level.
  if (runFirst < runLast) {
    std::size_t segFirst = runFirst;
    std::size_t segLast = runLast;
    for (std::size_t i = runFirst; i < runLast; ++i) {
      if (isSeparator(toks[i]) && toks[i].end <= off) {
        segFirst = i + 1;
      }
    }
    for (std::size_t i = runFirst; i < runLast; ++i) {
      if (isSeparator(toks[i]) && toks[i].beg >= off) {
        segLast = i;
        break;
      }
    }
    if (segFirst < segLast) {
      pushLevel(chain, {toks[segFirst].beg, toks[segLast - 1].end}, content);
    }
  }

  // Block levels: every block of `parse.blockRanges` that encloses the level
  // below it, innermost first. blockRanges is a flat list with no nesting info,
  // but the ranges nest by construction, so ordering by size and letting
  // pushLevel drop what does not strictly contain the previous level yields the
  // chain. A block the parser never closed (its closer has not been typed yet)
  // has no range at all and is simply absent.
  //
  // With no level yet — a blank line, or whitespace between statements — the
  // cursor's own position is the seed, so the blocks around it still apply. It
  // is zero-width, so it is never pushed as a level of its own.
  SourceRange const seed = chain.empty() ? SourceRange{off, off} : chain.back();
  std::vector<SourceRange> enclosing;
  for (SourceRange const &br : doc.parse.blockRanges) {
    if (br.beg <= seed.beg && seed.end <= br.end) {
      enclosing.push_back(br);
    }
  }
  std::sort(enclosing.begin(), enclosing.end(),
            [](SourceRange a, SourceRange b) {
              return (a.end - a.beg) < (b.end - b.beg);
            });
  for (SourceRange const &br : enclosing) {
    pushLevel(chain, br, content);
  }

  // Outermost level: the whole document. The file is a module, and a
  // module-level statement with no enclosing block still has somewhere to grow.
  pushLevel(chain, {0, static_cast<std::uint32_t>(content.size())}, content);
  return chain;
}

} // namespace fblang
