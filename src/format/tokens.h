#pragma once

#include <string>
#include <vector>

namespace fbformat {

enum class TokenKind {
  Unknown,
  Whitespace,
  Newline,
  LineComment,  // ' or REM
  BlockComment, // /' ... '/
  String,
  Identifier,
  Keyword,
  Number,
  Operator,
  Punct,
  Directive,   // #...
  Metacommand, // $... in comment context
  Eof
};

struct Token {
  TokenKind kind;
  size_t start;
  size_t end; // byte offsets
  std::string text;
};

class TokenStream {
public:
  explicit TokenStream(const std::string &content);
  const std::vector<Token> &tokens() const { return tokens_; }
  const std::string &content() const { return content_; }

private:
  std::string content_;
  std::vector<Token> tokens_;
};

} // namespace fbformat