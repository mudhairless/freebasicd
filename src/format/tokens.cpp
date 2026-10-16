#include "format/tokens.h"
#include <cstddef>
#include <string>

namespace fbformat {

namespace {
bool isIdentStart(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool isIdentChar(char c) {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}
bool isRadixPrefix(char c) {
  return c == 'h' || c == 'H' || c == 'o' || c == 'O' || c == 'b' || c == 'B';
}
}  // namespace

TokenStream::TokenStream(const std::string& content) : content_(content) {
  const char* p = content_.data();
  const char* end = p + content_.size();

  auto atLineStart = [&](const char* cur) -> bool {
    const char* q = cur;
    while (q > content_.data() && *(q - 1) != '\n' && *(q - 1) != '\r') {
      --q;
    }
    while (q < cur && isWhitespace(*q)) {
      ++q;
    }
    return q == cur;
  };
  auto peekChar = [&](const char* cur, size_t ahead) -> char {
    const char* q = cur + ahead;
    return q < end ? *q : '\0';
  };
  auto addToken = [&](TokenKind k, size_t start, size_t endOff) {
    Token t;
    t.kind = k;
    t.start = start;
    t.end = endOff;
    t.text = content_.substr(start, endOff - start);
    tokens_.push_back(t);
  };

  while (p < end) {
    size_t start = p - content_.data();
    if (isWhitespace(*p)) {
      while (p < end && isWhitespace(*p)) {
        ++p;
      }
      addToken(TokenKind::Whitespace, start, p - content_.data());
      continue;
    }
    if (*p == '\r' || *p == '\n') {
      if (*p == '\r' && p + 1 < end && *(p + 1) == '\n') {
        p += 2;
      } else {
        ++p;
      }
      addToken(TokenKind::Newline, start, p - content_.data());
      continue;
    }
    if (*p == '/' && p + 1 < end && *(p + 1) == '\'') {
      p += 2;
      int depth = 1;
      while (p < end && depth > 0) {
        if (*p == '/' && p + 1 < end && *(p + 1) == '\'') {
          ++depth;
          p += 2;
        } else if (*p == '\'' && p + 1 < end && *(p + 1) == '/') {
          --depth;
          p += 2;
        } else {
          ++p;
        }
      }
      addToken(TokenKind::BlockComment, start, p - content_.data());
      continue;
    }
    if (*p == '#') {
      // meta/pp style handled later; but handle here generally? only if line start per spec
      if (atLineStart(p)) {
        while (p < end && *p != '\n' && *p != '\r') ++p;
        addToken(TokenKind::Directive, start, p - content_.data());
        continue;
      }
    }
    if (*p == '$' && atLineStart(p)) {
      while (p < end && *p != '\n' && *p != '\r') {
        ++p;
      }
      addToken(TokenKind::Metacommand, start, p - content_.data());
      continue;
    }
    if (atLineStart(p) && p + 3 <= end) {
      // REM
      char c = *p;
      if ((c == 'r' || c == 'R') && ((*(p+1)=='e'||*(p+1)=='E')) && ((*(p+2)=='m'||*(p+2)=='M'))) {
        if (p + 3 == end || isWhitespace(peekChar(p, 3)) || peekChar(p, 3) == '\'' || peekChar(p, 3) == '\n' || peekChar(p, 3) == '\r') {
          while (p < end && *p != '\n' && *p != '\r') ++p;
          addToken(TokenKind::LineComment, start, p - content_.data());
          continue;
        }
      }
    }
    if (*p == '/' && atLineStart(p) && p + 2 < end && *(p + 1) == '/' && *(p + 2) == '/') {
      while (p < end && *p != '\n' && *p != '\r') {
        ++p;
      }
      addToken(TokenKind::LineComment, start, p - content_.data());
      continue;
    }
    if (*p == '\'') {
      while (p < end && *p != '\n' && *p != '\r') {
        ++p;
      }
      addToken(TokenKind::LineComment, start, p - content_.data());
      continue;
    }
    if (*p == '"') {
      ++p;
      while (p < end && *p != '\n' && *p != '\r') {
        if (*p == '"' && p + 1 < end && *(p + 1) == '"') {
          p += 2;
          continue;
        }
        if (*p == '"') {
          ++p;
          break;
        }
        ++p;
      }
      addToken(TokenKind::String, start, p - content_.data());
      continue;
    }
    if (isIdentStart(*p)) {
      while (p < end && isIdentChar(*p)) {
        ++p;
      }
      addToken(TokenKind::Identifier, start, p - content_.data());
      continue;
    }
    if (isDigit(*p) || (*p == '&' && isRadixPrefix(peekChar(p, 1))) ||
        (*p == '.' && peekChar(p, 1) >= '0' && peekChar(p, 1) <= '9')) {
      while (p < end && isIdentChar(*p)) {
        ++p;
      }
      addToken(TokenKind::Number, start, p - content_.data());
      continue;
    }
    ++p;
    addToken(TokenKind::Operator, start, p - content_.data());
  }
}
}  // namespace fbformat
