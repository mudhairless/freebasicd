/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "inlay_hints.h"

#include "language.h"
#include "lexer.h"
#include "symbols.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fblang {

namespace {

// Suffix char -> inferred type name. `%` is INTEGER (not SHORT) and `$` is
// STRING; see FreeBASIC.md §2.
bool suffixTypeName(char suffix, std::string *out) {
  switch (suffix) {
  case '$':
    *out = "String";
    return true;
  case '%':
    *out = "Integer";
    return true;
  case '&':
    *out = "Long";
    return true;
  case '!':
    *out = "Single";
    return true;
  case '#':
    *out = "Double";
    return true;
  default:
    return false;
  }
}

// One-past-the-last-character position of the physical line containing `off`
// (i.e. the offset of the line's newline, or the buffer end).
std::size_t endOfLine(std::string_view content, std::size_t off) {
  std::size_t const nl = content.find('\n', off);
  std::size_t end = (nl == std::string_view::npos) ? content.size() : nl;
  if (end > 0 && content[end - 1] == '\r') {
    --end;
  }
  return end;
}

// Zero-based physical line index of `off`.
std::size_t lineOf(std::string_view content, std::size_t off) {
  std::size_t lines = 0;
  std::size_t const limit = off < content.size() ? off : content.size();
  for (std::size_t i = 0; i < limit; ++i) {
    if (content[i] == '\n') {
      ++lines;
    }
  }
  return lines;
}

Token const *tokenAt(std::vector<Token> const &tokens, std::uint32_t beg) {
  for (Token const &t : tokens) {
    if (t.beg == beg) {
      return &t;
    }
  }
  return nullptr;
}

bool isDimLikeKeyword(std::string_view lower) {
  return lower == "dim" || lower == "redim" || lower == "var" ||
         lower == "common" || lower == "static";
}

} // namespace

std::vector<InlayHintItem> inlayHints(AnalyzedDoc const &doc,
                                      std::string_view content) {
  std::vector<InlayHintItem> out;

  // Expected closers: one per multi-line block, anchored at the end of the
  // opener's line so the hint never collides with the opener's own text. A
  // single-line construct (one-line IF ... THEN) gets none, mirroring folding.
  for (SourceRange const &br : doc.parse.blockRanges) {
    if (lineOf(content, br.beg) == lineOf(content, br.end)) {
      continue;
    }
    Token const *const opener = tokenAt(doc.tokens, br.beg);
    if (opener == nullptr) {
      continue;
    }
    // The same lookup the M12 `unterminated-block` quick fix uses, so a hint
    // and a fix can never name the closer differently.
    std::string label = expectedCloserAt(doc.tokens, br.beg);
    if (label.empty()) {
      continue;
    }
    out.push_back({static_cast<std::uint32_t>(endOfLine(
                       content, static_cast<std::size_t>(opener->beg))),
                   std::move(label)});
  }

  // Inferred types for suffix-typed declarations without an AS clause. Strictly
  // local: a dim-like keyword, then an identifier carrying a suffix, then a
  // token that is not `as`. Cosmetic in `fb` mode (fbc ignores suffixes).
  for (std::size_t i = 0; i + 1 < doc.tokens.size(); ++i) {
    Token const &kw = doc.tokens[i];
    if (kw.kind != TokenKind::Keyword ||
        !isDimLikeKeyword(toLowerChars(kw.text()))) {
      continue;
    }
    Token const &name = doc.tokens[i + 1];
    if (name.kind != TokenKind::Identifier || name.text().empty()) {
      continue;
    }
    std::string typeName;
    if (!suffixTypeName(name.text().back(), &typeName)) {
      continue;
    }
    if (i + 2 < doc.tokens.size() &&
        doc.tokens[i + 2].kind == TokenKind::Keyword &&
        toLowerChars(doc.tokens[i + 2].text()) == "as") {
      continue;
    }
    out.push_back({name.end, "As " + typeName});
  }

  return out;
}

} // namespace fblang
