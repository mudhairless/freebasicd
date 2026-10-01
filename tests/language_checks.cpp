/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Intrinsic catalog checks. LSP-agnostic: the catalog is plain data behind
// plain functions, so these exercise lookups, signatures, wiki URLs, and the
// statement-position predicate directly.

#include <cstdio>
#include <cstring>
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

#define CHECK_EQ_STR(a, b)                                                     \
  do {                                                                         \
    std::string const _a = std::string(a);                                     \
    std::string const _b = std::string(b);                                     \
    if (_a != _b) {                                                            \
      std::printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__,        \
                  _a.c_str(), _b.c_str());                                     \
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

static void checkParams(Intrinsic const &fn,
                        std::vector<std::string> const &want) {
  std::vector<std::string_view> const got = signatureParamLabels(fn);
  if (got.size() != want.size()) {
    std::printf("FAIL %s: param count %zu != %zu\n",
                std::string(fn.key).c_str(), got.size(), want.size());
    ++failures;
    return;
  }
  for (std::size_t i = 0; i < got.size(); ++i) {
    if (std::string(got[i]) != want[i]) {
      std::printf("FAIL %s: param %zu \"%s\" != \"%s\"\n",
                  std::string(fn.key).c_str(), i, std::string(got[i]).c_str(),
                  want[i].c_str());
      ++failures;
    }
  }
}

static void testLookup() {
  Intrinsic const *left = intrinsicFor("left");
  CHECK(left != nullptr);
  if (left == nullptr) {
    return;
  }
  CHECK_EQ_STR(left->key, "left");
  CHECK(left->hasDollar);
  CHECK(left->kind == IntrinsicKind::Function);
  CHECK_EQ_STR(left->signature,
               "Left$( str As String, n As Integer ) As String");
  CHECK(intrinsicDocsUrl(*left).find("KeyPgLeft") != std::string::npos);

  // Suffix stripping: `left$` and a bogus numeric suffix hit the same row.
  CHECK(intrinsicFor("left$") == left);
  CHECK(intrinsicFor("Left$") == left);
  CHECK(intrinsicFor("left%") == left);

  Intrinsic const *mid = intrinsicFor("mid");
  CHECK(mid != nullptr);
  if (mid != nullptr) {
    CHECK(mid->hasDollar);
    CHECK_EQ_STR(mid->page, "Midfunction");
    CHECK(intrinsicDocsUrl(*mid).find("KeyPgMidfunction") != std::string::npos);
  }

  Intrinsic const *print = intrinsicFor("print");
  CHECK(print != nullptr);
  CHECK(print != nullptr && print->kind == IntrinsicKind::Statement);

  CHECK(intrinsicFor("notanintrinsic") == nullptr);
  CHECK(intrinsicFor("") == nullptr);
  CHECK(intrinsicFor("$") == nullptr);
}

static void testCatalogShape() {
  std::vector<Intrinsic const *> const all = intrinsics();
  CHECK(all.size() > 200);
  std::string prev;
  for (Intrinsic const *fn : all) {
    CHECK(fn != nullptr);
    if (fn == nullptr) {
      continue;
    }
    CHECK(!fn->key.empty());
    CHECK(!fn->signature.empty());
    CHECK(!prev.empty() ? prev < std::string(fn->key) : true); // key-sorted
    prev = fn->key;
    for (char const c : fn->key) {
      CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'));
    }
    std::string const url = intrinsicDocsUrl(*fn);
    CHECK(url.rfind("https://www.freebasic.net/wiki/KeyPg", 0) == 0);
  }
}

static void testParamLabels() {
  Intrinsic const *left = intrinsicFor("left");
  CHECK(left != nullptr);
  if (left != nullptr) {
    checkParams(*left, {"str", "n"});
  }
  Intrinsic const *chr = intrinsicFor("chr");
  CHECK(chr != nullptr);
  if (chr != nullptr) {
    checkParams(*chr, {"ch", "..."});
  }
  Intrinsic const *cast = intrinsicFor("cast");
  CHECK(cast != nullptr);
  if (cast != nullptr) {
    checkParams(*cast, {"datatype", "expression"});
  }
  Intrinsic const *threadCreate = intrinsicFor("threadcreate");
  CHECK(threadCreate != nullptr);
  if (threadCreate != nullptr) {
    checkParams(*threadCreate, {"subname", "paramlist"});
  }
  Intrinsic const *err = intrinsicFor("err");
  CHECK(err != nullptr);
  if (err != nullptr) {
    checkParams(*err, {});
  }
}

static void testStatementPosition() {
  auto at = [](const char *src) {
    return statementPosition(tokensOf(src), (std::uint32_t)std::strlen(src));
  };
  CHECK(at(""));                 // start of file
  CHECK(at("pr"));               // a partial word at line start
  CHECK(at("x = 1\n"));          // after a logical newline
  CHECK(at("x = 1 : "));         // after a statement separator
  CHECK(at("if x then "));       // after Then
  CHECK(at("if x then\nelse ")); // after Else
  CHECK(!at("x = "));            // right of an assignment
  CHECK(!at("x = pr"));          // a partial word right of an assignment
  CHECK(!at("foo( "));           // inside a call
  CHECK(!at("a, "));             // right of a comma
  CHECK(!at("a.b "));            // right of member access
  CHECK(!at("print x "));        // after a keyword argument head
}

// One logical line's tokens, the way the parser hands them over: everything up
// to the newline (a `:`-separated statement is judged on its own tokens, and
// the lexer has already merged `_` continuation lines).
static std::vector<Token> statementOf(char const *srcC) {
  std::vector<Token> const all = tokensOf(srcC);
  std::vector<Token> out;
  for (Token const &t : all) {
    if (t.kind == TokenKind::Newline || t.kind == TokenKind::Eof ||
        t.kind == TokenKind::Comment || t.kind == TokenKind::DocComment) {
      break;
    }
    out.push_back(t);
  }
  return out;
}

// The record/enum body grammar, as a table: one row per probed statement, with
// the verdict fbc 1.10.2 gives it inside that body. A `false` row is a
// statement the body cannot accept, which is where the missing closer belongs.
static void testAcceptsBodyMember() {
  struct Row {
    BlockKind kind;
    char const *stmt;
    bool want;
  };
  Row const rows[] = {
      // A procedure body is a statement list: everything is a member of it, and
      // that is why end-of-buffer is its closer's home.
      {BlockKind::Sub, "print 1", true},
      {BlockKind::Sub, "x = 1", true},
      {BlockKind::Function, "y", true},
      // Fields. `Dim` is optional, so the bare form counts, and a reserved word
      // is a legal field name — the `as` clause is what decides.
      {BlockKind::Type, "x as single", true},
      {BlockKind::Type, "as integer x", true},
      {BlockKind::Type, "dim n as integer", true},
      {BlockKind::Type, "b(3) as byte", true},
      {BlockKind::Type, "redim q(3) as byte", true},
      {BlockKind::Type, "static s as integer", true},
      {BlockKind::Type, "const c = 1", true},
      {BlockKind::Type, "declare sub go()", true},
      {BlockKind::Type, "type inner", true},
      {BlockKind::Type, "union u", true},
      {BlockKind::Type, "enum e", true},
      {BlockKind::Type, "public:", true},
      {BlockKind::Type, "private:", true},
      {BlockKind::Type, "protected:", true},
      {BlockKind::Type, "rem a note", true},
      {BlockKind::Type, "' a comment", true},
      {BlockKind::Type, "DIM n AS INTEGER", true}, // case-insensitive
      // A member procedure spelled with its body: fbc wants `Declare Sub` here
      // (`error 17`), but this parser reads it as a member (FreeBASIC.md §12),
      // so it must not become a boundary.
      {BlockKind::Type, "sub bump()", true},
      {BlockKind::Type, "function f() as integer", true},
      // Boundaries in a TYPE/UNION body — fbc `error 17` on each.
      {BlockKind::Type, "print 1", false},
      {BlockKind::Type, "x", false},
      {BlockKind::Type, "field = 4", false}, // an opener-line modifier
      {BlockKind::Type, "x += 1", false},
      {BlockKind::Type, "p.x = 1.5", false},
      {BlockKind::Type, "x()", false},
      {BlockKind::Type, "goto foo", false},
      {BlockKind::Type, "if x then", false},
      {BlockKind::Type, "for i = 1 to 2", false},
      {BlockKind::Type, "with o", false},
      {BlockKind::Type, "asm", false},
      {BlockKind::Type, "namespace n", false},
      {BlockKind::Type, "scope", false},
      {BlockKind::Type, "var v", false},
      {BlockKind::Type, "local v", false},
      {BlockKind::Type, "common c2", false},
      {BlockKind::Type, "export", false},
      {BlockKind::Type, "dim shared g as integer", false},
      {BlockKind::Type, "redim preserve q(3)", false},
      {BlockKind::Type, "foo:", false},     // a label
      {BlockKind::Type, "1 = 2", false},    // a number starts an expression
      {BlockKind::Union, "public:", false}, // sections are TYPE-only
      {BlockKind::Union, "private:", false},
      {BlockKind::Union, "dim n as integer", true},
      // An enum body is `name`, `name = expr`.
      {BlockKind::Enum, "a", true},
      {BlockKind::Enum, "red = 1", true},
      {BlockKind::Enum, "print 1", false},
      {BlockKind::Enum, "dim n as integer", false},
      {BlockKind::Enum, "public:", false},
      {BlockKind::Enum, "as integer a", false},
      // A closer is never a member, but it is not a boundary either: the
      // parser's
      // own closer path owns it, and a mismatching one is that path's evidence.
      {BlockKind::Type, "end type", true},
      {BlockKind::Type, "end sub", true},
      {BlockKind::Type, "next", true},
      {BlockKind::Type, "wend", true},
      {BlockKind::Type, "loop", true},
      {BlockKind::Enum, "end enum", true},
      // Nothing to judge: a blank line, or one the parser never calls a
      // statement.
      {BlockKind::Type, "", true},
  };
  for (Row const &row : rows) {
    std::vector<Token> const stmt = statementOf(row.stmt);
    bool const got = acceptsBodyMember(row.kind, stmt);
    if (got != row.want) {
      std::printf("FAIL acceptsBodyMember(kind %d, \"%s\") = %s, want %s\n",
                  static_cast<int>(row.kind), row.stmt, got ? "true" : "false",
                  row.want ? "true" : "false");
      ++failures;
    }
  }
  // An empty statement list is not a boundary, whatever the body.
  CHECK(acceptsBodyMember(BlockKind::Type, {}));
  CHECK(acceptsBodyMember(BlockKind::Enum, {}));
}

static void testFolderNameCatalog() {
  // The project-layout folder-name catalog: source/include directory names per
  // language (full + common abbreviation). isSourceDirName/isIncludeDirName
  // match the ASCII-folded lowercase child-directory name the session probes.
  // English defaults + the abbreviations every project actually uses.
  CHECK(isSourceDirName("src"));
  CHECK(isSourceDirName("source"));
  CHECK(isIncludeDirName("inc"));
  CHECK(isIncludeDirName("include"));
  // Abbreviated non-English names.
  CHECK(isSourceDirName("fnt")); // Spanish/Esperanto fuente/fonto
  CHECK(isSourceDirName("srg")); // Italian sorgente
  CHECK(isSourceDirName("zdr")); // Czech/Slovak zdroj
  CHECK(isSourceDirName("for")); // Hungarian forrás
  CHECK(isIncludeDirName("incl"));
  CHECK(isIncludeDirName("inkl"));
  CHECK(isIncludeDirName("sis"));
  CHECK(isIncludeDirName("ein")); // German einschließen/Einbindung
  // Full non-English names (byte-exact, UTF-8 where applicable).
  CHECK(isSourceDirName("fuente"));
  CHECK(isSourceDirName("sumber"));
  CHECK(isIncludeDirName("inclusión"));
  CHECK(!isIncludeDirName("sumber")); // a source name, not an include name
  // Case-insensitive via the ASCII fold callers apply with toLowerChars.
  CHECK(isSourceDirName(fblang::toLowerChars("SRC")));
  CHECK(isSourceDirName(fblang::toLowerChars("Source")));
  CHECK(isIncludeDirName(fblang::toLowerChars("INCLUDE")));
  CHECK(isIncludeDirName(fblang::toLowerChars("Incl")));
  // Anything else is not a layout marker.
  CHECK(!isSourceDirName("app"));
  CHECK(!isSourceDirName("build"));
  CHECK(!isSourceDirName("src2"));
  CHECK(!isSourceDirName("data"));
  CHECK(!isIncludeDirName("inc2"));
  CHECK(!isIncludeDirName("lib"));
  CHECK(!isSourceDirName(""));
  CHECK(!isIncludeDirName(""));
}

int main() {
  testLookup();
  testCatalogShape();
  testParamLabels();
  testStatementPosition();
  testAcceptsBodyMember();
  testFolderNameCatalog();

  if (failures == 0) {
    std::printf("language_checks: all passed\n");
    return 0;
  }
  std::printf("language_checks: %d failure(s)\n", failures);
  return 1;
}
