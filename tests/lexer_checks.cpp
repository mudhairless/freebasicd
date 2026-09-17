// Tokenizer checks for the FreeBASIC lexer. Byte-offset, LSP-agnostic.
//
// Token.data points into the source buffer that was lexed, so the sources are
// parked in g_sources to keep them alive for the lifetime of main(). The real
// callers (parser, session) keep their document buffers alive as documented.

#include <cstdio>
#include <string>
#include <vector>

#include "language.h"
#include "lexer.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;
static std::vector<std::string> g_sources;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

static std::vector<Token> tokensOf(const char *srcC) {
  g_sources.emplace_back(srcC);
  const std::string &src = g_sources.back();
  Lexer lx(src);
  std::vector<Token> out;
  for (;;) {
    Token t = lx.next();
    out.push_back(t);
    if (t.kind == TokenKind::Eof) {
      break;
    }
  }
  return out;
}

static void checkKinds(const char *src, const std::vector<TokenKind> &kinds) {
  auto ts = tokensOf(src);
  CHECK(ts.size() == kinds.size() + 1); // + Eof
  size_t n = kinds.size() < ts.size() ? kinds.size() : ts.size();
  for (size_t i = 0; i < n; ++i) {
    if (ts[i].kind != kinds[i]) {
      std::printf("  tok[%zu] '%s' kind=%d expected %d\n", i,
                  std::string(ts[i].text()).c_str(), (int)ts[i].kind,
                  (int)kinds[i]);
      CHECK(false);
    }
  }
  CHECK(ts.back().kind == TokenKind::Eof);
}

int main() {
  // Keywords vs identifiers.
  CHECK(isReservedWord("if"));
  CHECK(isReservedWord("select"));
  CHECK(isReservedWord("endif"));
  CHECK(isReservedWord("type"));
  CHECK(isReservedWord("as"));
  CHECK(isReservedWord("then"));
  CHECK(!isReservedWord("ifx"));
  CHECK(!isReservedWord("method"));
  checkKinds("if then else end select case",
             {TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword,
              TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword});
  // Keywords do not swallow suffix chars; longer words are plain identifiers.
  checkKinds("ifx % then",
             {TokenKind::Identifier, TokenKind::Symbol, TokenKind::Keyword});
  checkKinds("print#1,", {TokenKind::Keyword, TokenKind::Symbol,
                          TokenKind::Number, TokenKind::Symbol});

  // Identifier suffixes.
  {
    auto ts = tokensOf("foo$ i% n& f! d# ok");
    CHECK(ts.size() == 7);
    CHECK(std::string(ts[0].text()) == "foo$");
    CHECK(std::string(ts[1].text()) == "i%");
    CHECK(std::string(ts[2].text()) == "n&");
    CHECK(std::string(ts[3].text()) == "f!");
    CHECK(std::string(ts[4].text()) == "d#");
    CHECK(ts[5].kind == TokenKind::Identifier);
    CHECK(std::string(ts[5].text()) == "ok");
  }

  // Numbers: decimal, float, exponent, radix, suffix.
  {
    auto ts = tokensOf("123 1.5 .5 1e-5 &HFF &O17 &B101 100&");
    CHECK(ts.size() == 9); // 8 numbers + Eof
    for (size_t i = 0; i < 8; ++i) {
      CHECK(ts[i].kind == TokenKind::Number);
    }
    CHECK(std::string(ts[0].text()) == "123");
    CHECK(std::string(ts[2].text()) == ".5");
    CHECK(std::string(ts[3].text()) == "1e-5");
    CHECK(std::string(ts[4].text()) == "&HFF");
    CHECK(std::string(ts[6].text()) == "&B101");
    CHECK(std::string(ts[7].text()) == "100&");
  }

  // Strings: doubled-quote escape, unterminated clipping.
  {
    auto ts = tokensOf("\"a\"\"b\"");
    CHECK(ts.size() == 2);
    CHECK(ts[0].kind == TokenKind::String);
    CHECK(ts[0].terminated);
    auto us = tokensOf("\"abc");
    CHECK(us[0].kind == TokenKind::String);
    CHECK(!us[0].terminated);
    // Quote inside a string is not a comment.
    auto qs = tokensOf("print \"it's fine\"");
    CHECK(qs[1].kind == TokenKind::String);
  }

  // Comments and doc comments.
  {
    auto ts = tokensOf("' plain\n'' doc\n/// slash\nx = 1");
    CHECK(ts[0].kind == TokenKind::Comment);
    CHECK(ts[1].kind == TokenKind::Newline);
    CHECK(ts[2].kind == TokenKind::DocComment);
    CHECK(ts[3].kind == TokenKind::Newline);
    CHECK(ts[4].kind == TokenKind::DocComment);
    CHECK(ts[5].kind == TokenKind::Newline);
    // Mid-line '' (after ':') is an ordinary comment, not a doc comment.
    auto mid = tokensOf("x = 1 : '' not a doc");
    CHECK(mid[4].kind == TokenKind::Comment);
  }

  // Preprocessor and legacy meta lines.
  checkKinds("#include once\n", {TokenKind::Preprocessor, TokenKind::Newline});
  checkKinds("$DYNAMIC\nx",
             {TokenKind::Meta, TokenKind::Newline, TokenKind::Identifier});

  // Line continuation merges logical lines and drops the bare '_'.
  {
    auto ts = tokensOf("dim x = 1 + _\n    2");
    for (const auto &t : ts) {
      CHECK(t.kind != TokenKind::Symbol || std::string(t.text()) != "_");
    }
    bool foundNewline = false;
    for (const auto &t : ts) {
      if (t.kind == TokenKind::Newline) {
        foundNewline = true;
      }
    }
    CHECK(!foundNewline);
  }

  // Operators and punctuation.
  checkKinds("x = 1 + 2",
             {TokenKind::Identifier, TokenKind::Symbol, TokenKind::Number,
              TokenKind::Symbol, TokenKind::Number});
  {
    auto ts = tokensOf("a -> b ... c");
    CHECK(std::string(ts[1].text()) == "->");
    CHECK(std::string(ts[3].text()) == "...");
  }

  // Operator table stays in lockstep with the lexer: every symbolOperators()
  // entry lexes as one Symbol token with exactly that text. Spaced between
  // identifiers so no entry is read as a radix prefix (e.g. `&H`) or number.
  for (std::string_view const op : symbolOperators()) {
    std::string const src = "a " + std::string(op) + " b";
    auto ts = tokensOf(src.c_str());
    CHECK(ts.size() == 4); // a, op, b, Eof
    if (ts.size() == 4) {
      CHECK(ts[1].kind == TokenKind::Symbol);
      CHECK(ts[1].text() == op);
    }
  }

  // Combined-assignment keywords are single tokens the classifier maps to
  // `operator`; the predicate must cover exactly those spellings.
  for (char const *const word :
       {"and=", "or=", "xor=", "eqv=", "imp=", "mod=", "shl=", "shr="}) {
    CHECK(isCombinedAssignKeyword(word));
    checkKinds(word, {TokenKind::Keyword});
  }
  CHECK(!isCombinedAssignKeyword("and"));
  CHECK(!isCombinedAssignKeyword("+="));
  checkKinds("x and= 1",
             {TokenKind::Identifier, TokenKind::Keyword, TokenKind::Number});

  // '?' is the PRINT shortcut symbol.
  checkKinds("? x", {TokenKind::Symbol, TokenKind::Identifier});

  // Empty input.
  {
    auto ts = tokensOf("");
    CHECK(ts.size() == 1);
    CHECK(ts[0].kind == TokenKind::Eof);
  }

  // Suffix vs radix: '&' only radix when followed by h/o/b.
  {
    auto ts = tokensOf("x& &H1");
    CHECK(ts[0].kind == TokenKind::Identifier);
    CHECK(std::string(ts[0].text()) == "x&");
    CHECK(ts[1].kind == TokenKind::Number);
  }

  if (failures == 0) {
    std::printf("lexer_checks: all passed\n");
    return 0;
  }
  std::printf("lexer_checks: %d failures\n", failures);
  return 1;
}
