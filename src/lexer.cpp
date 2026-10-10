/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "lexer.h"

#include "language.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Radix of FreeBASIC `&H`/`&O`/`&B` numeric-literal prefixes.
#define HEX_RADIX 16
#define OCT_RADIX 8
#define BIN_RADIX 2

namespace fblang {

namespace {

bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c) { return isIdentStart(c) || (c >= '0' && c <= '9'); }

bool isDigit(char c) { return c >= '0' && c <= '9'; }

bool isWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

// Does this physical line carry any code? A comment-only or whitespace-only
// line does not — a `#define x 5 _` followed by such a line cannot extend the
// directive (fbc joins the comment into the directive and the logical line
// ends there), so the join must stop at the underscore's own line. A string
// literal counts as code, and Basic strings cannot span lines, so the fresh
// per-line scan needs no escape state.
bool lineHasCode(std::string_view line) {
  for (std::size_t i = 0; i < line.size(); ++i) {
    char const c = line[i];
    if (c == '\'') {
      return false; // the rest of the line is a comment
    }
    if (c == '"') {
      return true; // a string literal is code
    }
    if (!isWhitespace(c)) {
      return true;
    }
  }
  return false;
}

bool isRadixPrefix(char c) {
  return c == 'h' || c == 'H' || c == 'o' || c == 'O' || c == 'b' || c == 'B';
}

} // namespace

Lexer::Lexer(std::string_view source) : src_(source) {
  p_ = src_.data();
  end_ = src_.data() + src_.size();
}

char Lexer::peekChar(size_t ahead) const {
  const char *q = p_ + ahead;
  return q < end_ ? *q : '\0';
}

char Lexer::advance() {
  char const c = *p_;
  if (p_ < end_) {
    ++p_;
  }
  return c;
}

bool Lexer::atLineStart() const {
  const char *q = p_;
  while (q > src_.data() && *(q - 1) != '\n' && *(q - 1) != '\r') {
    --q;
  }
  while (q < p_ && isWhitespace(*q)) {
    ++q;
  }
  return q == p_;
}

// Does the accumulated directive text `[beg, end)` end in a line-continuation
// `_`? fbc's rule (FreeBASIC.md §6 — "must not follow an identifier/word
// without a space"): a trailing `_` continues the line unless it is the tail
// of an identifier. The discriminator is the word's start: a letter or `_`
// begins an identifier and absorbs later digits/underscores (`foo2_`,
// `__FB_DEBUG__` are one word, no continuation); a digit begins a number
// literal, which a `_` never extends — `5_` stops the number and the `_` is a
// fresh token, so at end of line it continues (probed: `#define X 5_` joins
// the next line; `#define X ABC_` does not; `#define 5_10` errors "expected
// identifier, found '_10'"). Comment- and string-aware: a `'` runs to the end
// of its own physical line only (in a joined directive it must not hide the
// code that follows), and a `_` inside a string literal is data.
bool Lexer::directiveEndsWithContinuation(uint32_t beg, uint32_t end) const {
  uint32_t codeEnd = beg;
  bool inString = false;
  bool inWord = false;          // inside an identifier or numeric word
  bool wordIsNumber = false;    // that word began with a digit
  bool freshUnderscore = false; // last code char was a non-absorbed `_`
  for (uint32_t i = beg; i < end; ++i) {
    char const ch = src_[i];
    if (inString) {
      if (ch == '"') {
        if (i + 1 < end && src_[i + 1] == '"') {
          ++i; // `""` escape: stay in the string
        } else {
          inString = false;
          // The closing quote is the last code char of the literal.
          freshUnderscore = false;
          codeEnd = i + 1;
        }
      }
      continue;
    }
    if (ch == '"') {
      inString = true;
      inWord = false;
      wordIsNumber = false;
      // A string literal is code, and its closing quote (or a dangling open
      // one) can be the line's last code char.
      freshUnderscore = false;
      codeEnd = i + 1;
    } else if (ch == '\'') {
      // A comment runs to the end of its physical line only. In an
      // accumulated multi-line directive (`..._ ' note` + next line) it must
      // not hide the code that follows it, so skip past the line instead of
      // ending the scan. On the last line there is no newline and the scan
      // simply ends.
      while (i < end && src_[i] != '\n' && src_[i] != '\r') {
        ++i;
      }
      inWord = false;
      wordIsNumber = false;
    } else if (isIdentStart(ch) && ch != '_') {
      // A letter begins (or resumes) an identifier word.
      inWord = true;
      wordIsNumber = false;
      freshUnderscore = false;
      codeEnd = i + 1;
    } else if (ch == '_') {
      // Extends an identifier (`foo_`, `__FB_DEBUG__`); after a number or
      // anything non-word it begins a fresh token, and a fresh trailing `_`
      // is the continuation.
      freshUnderscore = !inWord || wordIsNumber;
      inWord = true;
      wordIsNumber = false;
      codeEnd = i + 1;
    } else if (isDigit(ch)) {
      if (!inWord) {
        inWord = true;
        wordIsNumber = true;
      }
      freshUnderscore = false;
      codeEnd = i + 1;
    } else if (ch == '\n' || ch == '\r' || isWhitespace(ch)) {
      // A line or space break ends the current word; it is not code, so the
      // continuation status of a preceding `_` survives.
      inWord = false;
      wordIsNumber = false;
    } else {
      // Any other symbol ends the word and is code.
      inWord = false;
      wordIsNumber = false;
      freshUnderscore = false;
      codeEnd = i + 1;
    }
  }
  return codeEnd > beg && src_[codeEnd - 1] == '_' && freshUnderscore;
}

void Lexer::skipHorizontalWs() {
  while (p_ < end_ && isWhitespace(*p_)) {
    ++p_;
  }
}

bool Lexer::consumeNewline() {
  if (p_ >= end_) {
    return false;
  }
  if (*p_ == '\r') {
    ++p_;
    if (p_ < end_ && *p_ == '\n') {
      ++p_;
    }
    return true;
  }
  if (*p_ == '\n') {
    ++p_;
    return true;
  }
  return false;
}

Token Lexer::next() {
  if (!lookahead_.empty()) {
    Token t = lookahead_.front();
    lookahead_.erase(lookahead_.begin());
    return t;
  }
  return lexNext();
}

Token Lexer::peek(size_t ahead) {
  while (lookahead_.size() <= ahead) {
    lookahead_.push_back(lexNext());
  }
  return lookahead_[ahead];
}

Token Lexer::lexNext() {
  for (;;) {
    skipHorizontalWs();
    if (p_ >= end_) {
      Token t;
      t.kind = TokenKind::Eof;
      t.beg = static_cast<uint32_t>(end_ - src_.data());
      t.end = t.beg;
      t.data = src_.data() + t.beg;
      return t;
    }
    if (consumeNewline()) {
      Token t;
      t.kind = TokenKind::Newline;
      // The newline is one or two bytes (\r, \n, \r\n); widen to cover it.
      t.beg = static_cast<uint32_t>(p_ - src_.data());
      while (t.beg > 0 &&
             (src_[t.beg - 1] == '\n' || src_[t.beg - 1] == '\r')) {
        --t.beg;
      }
      t.end = static_cast<uint32_t>(p_ - src_.data());
      t.data = src_.data() + t.beg;
      return t;
    }

    char const c = *p_;

    // Line-leading '#' is a preprocessor directive (swallows the line).
    if (c == '#' && atLineStart()) {
      uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
      uint32_t tokEnd = beg;
      for (;;) {
        while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
          ++p_;
        }
        tokEnd = static_cast<uint32_t>(p_ - src_.data());
        // A directive line whose code tail is a `_` continues onto the next
        // line: fbc reads the whole continuation as one directive, so a
        // multi-line `#define max(a,b) _` body arrives as one Preprocessor
        // token and the preprocessor sees one value. Comment- and
        // string-aware — `_ ' note` still continues, a `_` inside a string or
        // comment does not, and an identifier tail (`#define X abc_`) never
        // does — see directiveEndsWithContinuation.
        if (!directiveEndsWithContinuation(beg, tokEnd)) {
          break;
        }
        bool consumed = false;
        if (p_ < end_ && *p_ == '\r') {
          ++p_;
          consumed = true;
        }
        if (p_ < end_ && *p_ == '\n') {
          ++p_;
          consumed = true;
        }
        // A dangling `_` with no newline after it (the source ends right
        // there) has nothing to join with: stop instead of looping forever.
        if (!consumed) {
          break;
        }
        // A continuation joins the next physical line only if that line
        // carries code; a comment-only or blank line ends the directive, so
        // the token stops at the underscore's own line.
        uint32_t const nextBeg = static_cast<uint32_t>(p_ - src_.data());
        while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
          ++p_;
        }
        if (!lineHasCode(std::string_view(src_.data() + nextBeg,
                                          p_ - src_.data() - nextBeg))) {
          break;
        }
      }
      Token t;
      t.kind = TokenKind::Preprocessor;
      t.beg = beg;
      t.end = tokEnd;
      t.data = src_.data() + beg;
      return t;
    }

    // Line-leading REM is a comment (swallows the line). It must be the
    // first token and followed by whitespace, a quote, or end of line.
    //
    // "End of line" has to mean the newline too, not just the end of the
    // buffer: `isWhitespace` stops at the line ends (they are line ends, not
    // blanks), so `rem note` was a comment and a bare `rem` was not. fbc reads
    // both as comments, and the difference is load-bearing rather than
    // cosmetic — in a record or enum body a `Rem` line contributes no member,
    // so a bare `rem` came out as a keyword the body had to reject. The source
    // whose real complaint is that the enum is left empty
    // (`error 256`, FreeBASIC.md §7) was reported as a member-name error.
    char const afterRem = peekChar(3);
    if (atLineStart() && p_ + 3 <= end_ && (c == 'r' || c == 'R') &&
        (peekChar(1) == 'e' || peekChar(1) == 'E') &&
        (peekChar(2) == 'm' || peekChar(2) == 'M') &&
        (p_ + 3 == end_ || isWhitespace(afterRem) || afterRem == '\'' ||
         afterRem == '\n' || afterRem == '\r')) {
      uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
      while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
        ++p_;
      }
      Token t;
      t.kind = TokenKind::Comment;
      t.beg = beg;
      t.end = static_cast<uint32_t>(p_ - src_.data());
      t.data = src_.data() + beg;
      return t;
    }

    // Line-leading '$' is a legacy meta-command (swallows the line).
    if (c == '$' && atLineStart()) {
      uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
      while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
        ++p_;
      }
      Token t;
      t.kind = TokenKind::Meta;
      t.beg = beg;
      t.end = static_cast<uint32_t>(p_ - src_.data());
      t.data = src_.data() + beg;
      return t;
    }

    // Line-leading '///' is a doc comment (swallows the line).
    if (c == '/' && atLineStart() && peekChar(1) == '/' && peekChar(2) == '/') {
      uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
      while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
        ++p_;
      }
      Token t;
      t.kind = TokenKind::DocComment;
      t.beg = beg;
      t.end = static_cast<uint32_t>(p_ - src_.data());
      t.data = src_.data() + beg;
      return t;
    }

    // `/'` opens a multi-line block comment closed by `'/`. It nests, spans
    // newlines, and *contents* are inert: a `'` or `$` inside is not a comment
    // or a metacommand (FreeBASIC.md §1; fbc's own `error 132` is the
    // unterminated case). Handled before the single-quote branch, because that
    // one would read the whole first line as a line comment.
    if (c == '/' && peekChar(1) == '\'') {
      return lexBlockComment();
    }
    if (c == '\'') {
      return lexComment();
    }
    if (c == '"') {
      return lexString();
    }
    if (isIdentStart(c)) {
      return lexIdentifier();
    }
    if (isDigit(c) || (c == '&' && isRadixPrefix(peekChar(1))) ||
        (c == '.' && peekChar(1) >= '0' && peekChar(1) <= '9')) {
      return lexNumber();
    }
    return lexSymbol();
  }
}

Token Lexer::lexComment() {
  uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
  // Only a line-leading '' turns a quote-comment into a doc comment; mid-line
  // lone '' (e.g. after ':' on a statement line) stays an ordinary comment.
  bool const doc = p_ + 1 < end_ && *(p_ + 1) == '\'' && atLineStart();
  while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
    ++p_;
  }
  Token t;
  t.kind = doc ? TokenKind::DocComment : TokenKind::Comment;
  t.beg = beg;
  t.end = static_cast<uint32_t>(p_ - src_.data());
  t.data = src_.data() + beg;
  return t;
}

// `/` ... `'/`, with `/'`/`'/` nesting as fbc does: an inner `/'` defers the
// close, so `/' a /' b '/ c '/` is one comment. `terminated` is false when the
// buffer ended first — fbc's `error 132`; the parser does not map that number
// yet, so an unterminated comment is simply consumed to EOF here.
Token Lexer::lexBlockComment() {
  uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
  p_ += 2; // opening `/'`
  int depth = 1;
  while (p_ < end_ && depth > 0) {
    if (*p_ == '/' && p_ + 1 < end_ && *(p_ + 1) == '\'') {
      ++depth;
      p_ += 2;
    } else if (*p_ == '\'' && p_ + 1 < end_ && *(p_ + 1) == '/') {
      --depth;
      p_ += 2;
    } else {
      ++p_;
    }
  }
  Token t;
  t.kind = TokenKind::Comment;
  t.beg = beg;
  t.end = static_cast<uint32_t>(p_ - src_.data());
  t.terminated = depth == 0;
  t.data = src_.data() + beg;
  return t;
}

Token Lexer::lexString() {
  uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
  advance(); // opening quote
  bool terminated = false;
  while (p_ < end_ && *p_ != '\n' && *p_ != '\r') {
    if (*p_ == '"') {
      if (p_ + 1 < end_ && *(p_ + 1) == '"') {
        p_ += 2; // doubled quote inside a string
        continue;
      }
      ++p_;
      terminated = true;
      break;
    }
    ++p_;
  }
  Token t;
  t.kind = TokenKind::String;
  t.beg = beg;
  t.end = static_cast<uint32_t>(p_ - src_.data());
  t.terminated = terminated;
  t.data = src_.data() + beg;
  return t;
}

Token Lexer::lexIdentifier() {
  uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
  while (p_ < end_ && isIdentChar(*p_)) {
    ++p_;
  }
  const char *baseStart = src_.data() + beg;
  const char *baseEnd = p_;

  bool const isBareUnderscore = (baseEnd - baseStart == 1 && *baseStart == '_');

  // Trailing '_' as the last thing on a logical line is a line continuation:
  // drop it and the newline so callers see one continuous logical line.
  if (isBareUnderscore) {
    const char *q = p_;
    while (q < end_ && isWhitespace(*q)) {
      ++q;
    }
    // A trailing comment may sit between the continuation marker and the
    // newline: fbc reads `_ ' note` as a continuation and joins the next line
    // (FreeBASIC.md §1). Without this the `_` fell through as a Symbol and the
    // parser called valid code `bad-continuation`.
    if (q < end_ && *q == '\'') {
      while (q < end_ && *q != '\n' && *q != '\r') {
        ++q;
      }
    }
    if (q < end_ && (*q == '\n' || *q == '\r')) {
      // A continuation never joins a preprocessor directive line: the
      // preprocessor owns those before tokenizing, and inside a skipped `#if`
      // region a `_` is inert rather than a joiner (pp/if-skip-1.bas). Leaving
      // the newline in place lets the directive start its own line.
      const char *r = q;
      if (r < end_ && *r == '\r') {
        ++r;
      }
      if (r < end_ && *r == '\n') {
        ++r;
      }
      while (r < end_ && isWhitespace(*r)) {
        ++r;
      }
      if (!(r < end_ && *r == '#')) {
        p_ = q;
        consumeNewline();
        return lexNext();
      }
    }
    Token t;
    t.kind = TokenKind::Symbol;
    t.beg = beg;
    t.end = beg + 1;
    t.data = baseStart;
    return t;
  }

  std::string_view const base(baseStart,
                              static_cast<size_t>(baseEnd - baseStart));
  bool const isKeywordBase = isReservedWord(base);

  // A directly-attached suffix char belongs to the identifier (foo$ is one
  // symbol). A reserved word may also carry one — fbc warns "Suffix ignored"
  // and keeps the bare keyword, so `end%` is `end` — with one exception: `#`
  // is never a keyword suffix, because PRINT#1 is PRINT + "#1" (a file
  // channel), not PRINT suffixed.
  bool skippedKeywordSuffix = false;
  if (p_ < end_ && isSuffixChar(*p_) && !(isKeywordBase && *p_ == '#')) {
    if (isKeywordBase) {
      skippedKeywordSuffix = true;
    }
    ++p_;
  }

  uint32_t end = static_cast<uint32_t>(p_ - src_.data());
  // A keyword's suffix is skipped, not folded into the token: callers match
  // the bare word ("and" for AND%, "end" for END$). The combined-assignment
  // merge below is the only thing allowed to extend a keyword past its base.
  if (isKeywordBase) {
    end = static_cast<uint32_t>(baseEnd - src_.data());
  }
  // Combined assignment keywords lex as one token only when the '=' is
  // directly attached (AND= works; "and =" stays two tokens; and%= is the
  // suffix-skipped AND followed by '=', so it folds too). The token spans the
  // raw slice through the '=' — for `and%=` that is "and%=", because the
  // suffix char sits between the base and the '=' and no single slice is
  // "and="; isCombinedAssignKeyword strips the char before the '='.
  if (isKeywordBase && p_ < end_ && *p_ == '=') {
    if (base == "and" || base == "or" || base == "xor" || base == "eqv" ||
        base == "imp" || base == "mod" || base == "shl" || base == "shr") {
      ++p_;
      end = static_cast<uint32_t>(p_ - src_.data());
    }
  }

  Token t;
  t.kind = isKeywordBase ? TokenKind::Keyword : TokenKind::Identifier;
  t.beg = beg;
  t.end = end;
  t.suffixBeg = 0;
  if (skippedKeywordSuffix) {
    // `end%` has its skipped suffix at baseEnd; `and%=` folds the '=' into the
    // token but the suffix stayed outside it, also at baseEnd.
    t.suffixBeg = static_cast<uint32_t>(baseEnd - src_.data());
  }
  t.data = src_.data() + beg;
  return t;
}

Token Lexer::lexNumber() {
  uint32_t const beg = static_cast<uint32_t>(p_ - src_.data());
  auto radixDigit = [](char c, int radix) {
    if (c >= '0' && c <= '9') {
      return c - '0' < radix;
    }
    return radix == HEX_RADIX &&
           ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
  };

  if (*p_ == '&') {
    char const r = static_cast<char>(
        std::tolower(static_cast<unsigned char>(peekChar(1))));
    int radix = 0;
    if (r == 'h') {
      radix = HEX_RADIX;
    } else if (r == 'o') {
      radix = OCT_RADIX;
    } else if (r == 'b') {
      radix = BIN_RADIX;
    }
    if (radix != 0) {
      advance();
      advance();
      while (p_ < end_ && radixDigit(*p_, radix)) {
        ++p_;
      }
      Token t;
      t.kind = TokenKind::Number;
      t.beg = beg;
      t.end = static_cast<uint32_t>(p_ - src_.data());
      t.data = src_.data() + beg;
      return t;
    }
  }

  if (*p_ == '.') {
    advance();
    while (p_ < end_ && isDigit(*p_)) {
      ++p_;
    }
  } else {
    while (p_ < end_ && isDigit(*p_)) {
      ++p_;
    }
    if (p_ < end_ && *p_ == '.' && p_ + 1 < end_ && isDigit(*(p_ + 1))) {
      ++p_;
      while (p_ < end_ && isDigit(*p_)) {
        ++p_;
      }
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      const char *q = p_ + 1;
      if (q < end_ && (*q == '+' || *q == '-')) {
        ++q;
      }
      if (q < end_ && isDigit(*q)) {
        p_ = q;
        while (p_ < end_ && isDigit(*p_)) {
          ++p_;
        }
      }
    }
  }
  if (p_ < end_ && isSuffixChar(*p_)) {
    ++p_;
  }
  Token t;
  t.kind = TokenKind::Number;
  t.beg = beg;
  t.end = static_cast<uint32_t>(p_ - src_.data());
  t.data = src_.data() + beg;
  return t;
}

Token Lexer::lexSymbol() {
  uint32_t beg = static_cast<uint32_t>(p_ - src_.data());
  char const c = *p_;
  auto finish = [&](uint32_t end, std::string_view text) -> Token {
    Token t;
    t.kind = TokenKind::Symbol;
    t.beg = beg;
    t.end = end;
    t.data = text.data();
    return t;
  };

  switch (c) {
  case '.':
    if (peekChar(1) == '.' && peekChar(2) == '.') {
      p_ += 3;
      return finish(beg + 3, "...");
    }
    ++p_;
    return finish(beg + 1, ".");
  case '-':
    if (peekChar(1) == '>') {
      p_ += 2;
      return finish(beg + 2, "->");
    }
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "-=");
    }
    ++p_;
    return finish(beg + 1, "-");
  case '+':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "+=");
    }
    ++p_;
    return finish(beg + 1, "+");
  case '*':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "*=");
    }
    ++p_;
    return finish(beg + 1, "*");
  case '/':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "/=");
    }
    ++p_;
    return finish(beg + 1, "/");
  case '\\':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "\\=");
    }
    ++p_;
    return finish(beg + 1, "\\");
  case '&':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "&=");
    }
    ++p_;
    return finish(beg + 1, "&");
  case '<':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, "<=");
    }
    if (peekChar(1) == '>') {
      p_ += 2;
      return finish(beg + 2, "<>");
    }
    ++p_;
    return finish(beg + 1, "<");
  case '>':
    if (peekChar(1) == '=') {
      p_ += 2;
      return finish(beg + 2, ">=");
    }
    ++p_;
    return finish(beg + 1, ">");
  default: {
    ++p_;
    return finish(beg + 1, std::string_view(p_ - 1, 1));
  }
  }
}

} // namespace fblang
