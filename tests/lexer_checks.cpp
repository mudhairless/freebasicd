/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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
  // Keywords are case-insensitive, as fbc matches them: `SUB` is `sub`. The
  // token still carries the raw source slice, so callers compare lowercased.
  CHECK(isReservedWord("IF"));
  CHECK(isReservedWord("End"));
  CHECK(isReservedWord("sUb"));
  CHECK(!isReservedWord("IFX"));
  checkKinds("if then else end select case",
             {TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword,
              TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword});
  checkKinds("IF THEN End Select CASE",
             {TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword,
              TokenKind::Keyword, TokenKind::Keyword});
  {
    auto ts = tokensOf("End Sub");
    CHECK(ts[0].kind == TokenKind::Keyword);
    CHECK(ts[1].kind == TokenKind::Keyword);
    // Raw text, not folded: only the classification is case-insensitive.
    CHECK(std::string(ts[0].text()) == "End");
    CHECK(std::string(ts[1].text()) == "Sub");
  }
  // Keywords swallow a directly-attached suffix char (warning 44 in fbc: the
  // suffix is ignored, the word stays the bare keyword); `#` is the exception
  // because PRINT#1 is PRINT + "#1" channel. Longer words are plain
  // identifiers, and a spaced `%` is its own symbol.
  checkKinds("ifx % then",
             {TokenKind::Identifier, TokenKind::Symbol, TokenKind::Keyword});
  checkKinds("print#1,", {TokenKind::Keyword, TokenKind::Symbol,
                          TokenKind::Number, TokenKind::Symbol});
  {
    auto ts = tokensOf("end% if$ for & while # done#");
    CHECK(ts.size() == 8); // 7 tokens + Eof
    // The suffix is skipped, not folded in: the token stays the bare keyword.
    CHECK(ts[0].kind == TokenKind::Keyword);
    CHECK(std::string(ts[0].text()) == "end");
    CHECK(ts[1].kind == TokenKind::Keyword);
    CHECK(std::string(ts[1].text()) == "if");
    CHECK(ts[2].kind == TokenKind::Keyword);
    CHECK(std::string(ts[2].text()) == "for");
    // `&` is the LONG suffix plus a space: it is its own symbol here.
    CHECK(ts[3].kind == TokenKind::Symbol);
    CHECK(ts[4].kind == TokenKind::Keyword);
    CHECK(std::string(ts[4].text()) == "while");
    // `#` after a keyword is never a suffix (PRINT#1), so this one is a
    // Symbol; `done` is an identifier, so its directly-attached `#` folds in.
    CHECK(ts[5].kind == TokenKind::Symbol);
    CHECK(ts[6].kind == TokenKind::Identifier);
    CHECK(std::string(ts[6].text()) == "done#");
  }
  // The suffix-skipped keyword still folds into a combined assignment: and%=
  // is AND% + '=', i.e. AND=. The merged token spans the raw slice through
  // the '=' — "and%=", because the skipped '%' sits between the base and the
  // '=' — and isCombinedAssignKeyword strips it before comparing, which is
  // what the semantic-tokens classifier does.
  {
    auto ts = tokensOf("i and%= 1 and= 2");
    CHECK(ts.size() == 6);
    CHECK(ts[1].kind == TokenKind::Keyword);
    CHECK(std::string(ts[1].text()) == "and%=");
    CHECK(ts[1].suffixBeg == 5);
    CHECK(fblang::isCombinedAssignKeyword("and%="));
    CHECK(ts[3].kind == TokenKind::Keyword);
    CHECK(std::string(ts[3].text()) == "and=");
    CHECK(fblang::isCombinedAssignKeyword("and="));
  }

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

  // `/'` ... `'/` is a multi-line block comment, and it nests: contents are
  // inert (a `'` inside is not a line comment), so one token spans the whole
  // thing. An unterminated one is consumed to EOF with `terminated` false.
  {
    auto ts = tokensOf("print 1 /' note '/ print 2");
    CHECK(ts.size() == 6); // print 1 <comment> print 2 + Eof
    CHECK(ts[2].kind == TokenKind::Comment);
    CHECK(ts[2].terminated);
    auto nested = tokensOf("/' a /' b '/ c '/");
    CHECK(nested.size() == 2);
    CHECK(nested[0].kind == TokenKind::Comment);
    CHECK(std::string(nested[0].text()) == "/' a /' b '/ c '/");
    auto open = tokensOf("/' unterminated");
    CHECK(open[0].kind == TokenKind::Comment);
    CHECK(!open[0].terminated);
  }

  // A line-leading `Rem` is a comment whether or not anything follows it. The
  // bare form is the one that is easy to get wrong, because "end of line" is a
  // newline and not one of the blanks `isWhitespace` covers: `rem note` lexed
  // as a comment while a line-ending `rem` lexed as a keyword. In a record or
  // enum body that is the difference between "this line declares no member"
  // (what fbc means) and "this is an identifier the body must reject" — the
  // empty enum fbc then objects to is `error 256`, not a member-name error.
  {
    for (const char *src : {"rem\n", "rem \n", "rem note\n", "rem\tnote\n",
                            "rem' note\n", "rem"}) {
      auto const ts = tokensOf(src);
      if (ts[0].kind != TokenKind::Comment) {
        std::printf("FAIL \"%s\" lexed as kind %d, want Comment\n", src,
                    static_cast<int>(ts[0].kind));
        ++failures;
      }
    }
    // Not at the start of a line it is an ordinary keyword: that is what makes
    // `as integer rem` a field declaration fbc accepts.
    checkKinds("type t\n  as integer rem\nend type\n",
               {TokenKind::Keyword, TokenKind::Identifier, TokenKind::Newline,
                TokenKind::Keyword, TokenKind::Keyword, TokenKind::Keyword,
                TokenKind::Newline, TokenKind::Keyword, TokenKind::Keyword,
                TokenKind::Newline});
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

    // A trailing comment may sit between `_` and the newline: fbc reads
    // `_ ' note` as a continuation and joins the next line, so neither the
    // comment nor the newline survives.
    auto note = tokensOf("a = 1 _ ' note\n+ 2");
    CHECK(note.size() == 6); // a = 1 + 2 + Eof
    CHECK(std::string(note[3].text()) == "+");
  }

  // A continuation never joins a preprocessor directive line: the `_` stays a
  // Symbol and the newline survives, so `#if` starts its own line.
  {
    auto pp = tokensOf("a = 1 _\n#if 0\n#endif\n");
    CHECK(pp[3].kind == TokenKind::Symbol);
    CHECK(std::string(pp[3].text()) == "_");
    bool sawPreproc = false;
    for (const auto &t : pp) {
      if (t.kind == TokenKind::Preprocessor) {
        sawPreproc = true;
      }
    }
    CHECK(sawPreproc);
  }

  // The reverse join: a `#` directive whose code tail is `_` continues onto
  // the next line, so ONE Preprocessor token spans both lines and the parser
  // folds a single value out of them (fbc reads a continued define as one
  // directive).
  {
    auto ts = tokensOf("#define mx(a, b) _\n  ((a) + (b))\nprint 1\n");
    CHECK(ts.size() == 6); // Preprocessor, Newline, print, 1, Newline, Eof
    CHECK(ts[0].kind == TokenKind::Preprocessor);
    CHECK(std::string(ts[0].text()).find("((a) + (b))") !=
          std::string::npos); // the continuation line is inside the token
    CHECK(ts[1].kind == TokenKind::Newline); // no newline splits the join
  }

  // A trailing comment may sit between the directive's `_` and the newline,
  // exactly as for a code-line continuation.
  {
    auto ts = tokensOf("#define x 5 _ ' note\n+ 2\nprint 2\n");
    CHECK(ts[0].kind == TokenKind::Preprocessor);
    CHECK(std::string(ts[0].text()).find("+ 2") != std::string::npos);
    CHECK(ts[1].kind == TokenKind::Newline);
  }

  // A `_` inside a string literal is data, not a continuation: the directive
  // ends at the line's quote.
  {
    auto ts = tokensOf("#print \"a_\"\nprint 1\n");
    CHECK(ts[0].kind == TokenKind::Preprocessor);
    CHECK(std::string(ts[0].text()) == "#print \"a_\"");
    CHECK(ts[1].kind == TokenKind::Newline);
    CHECK(std::string(ts[2].text()) == "print");
  }

  // A `_` inside a trailing comment is data too.
  {
    auto ts = tokensOf("#define x 5 ' trailing_\nprint 2\n");
    CHECK(ts[0].kind == TokenKind::Preprocessor);
    CHECK(std::string(ts[0].text()) == "#define x 5 ' trailing_");
    CHECK(ts[1].kind == TokenKind::Newline);
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
