/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fblang {

enum class TokenKind {
  Identifier,   // may carry a type-suffix char: foo$, i%
  Keyword,      // reserved word; text holds the raw slice, compare lowercased
  Number,       // 1, 1.5, 1e-5, &HFF, suffix allowed
  String,       // "..." with "" escapes; terminated=false if clipped by EOL
  Comment,      // '... to end of line
  DocComment,   // ''... or ///... to end of line
  Preprocessor, // #-directive line, text spans the whole line
  Meta,         // $-meta line ($DYNAMIC ...), text spans the whole line
  Symbol,  // punctuation / operators, including combined assigns (=, +=, ->,
           // ...)
  Newline, // end of a logical line (continuation lines are merged)
  Eof
};

struct Token {
  TokenKind kind = TokenKind::Eof;
  uint32_t beg = 0;           // byte offset of the first character
  uint32_t end = 0;           // byte offset one past the last character
  bool terminated = true;     // strings: false when clipped by end-of-line
  const char *data = nullptr; // == source.data() + beg

  std::string_view text() const {
    return {data, static_cast<size_t>(end - beg)};
  }
};

// Tokenizer over FreeBASIC source. Works on byte offsets; lines are merged
// across trailing-underscore continuations so Newline marks true logical EOLs.
// Result depends only on the source buffer; the source must outlive tokens.
class Lexer {
public:
  explicit Lexer(std::string_view source);

  Token next();
  Token peek(size_t ahead = 0);

  std::string_view source() const { return src_; }

private:
  Token lexNext();
  void skipHorizontalWs();
  bool consumeNewline(); // consumes one \n / \r\n / \r, true if present
  Token lexIdentifier();
  Token lexNumber();
  Token lexString();
  Token lexComment();
  Token lexSymbol();
  bool atLineStart() const;
  char peekChar(size_t ahead = 0) const;
  char advance();

  std::string_view src_;
  const char *p_ = nullptr;
  const char *end_ = nullptr;
  std::vector<Token> lookahead_;
};

} // namespace fblang
