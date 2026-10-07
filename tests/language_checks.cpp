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
    char const *enclosing; // the record's own key; "" = unnamed
  };
  Row const rows[] = {
      // A procedure body is a statement list: everything is a member of it, and
      // that is why end-of-buffer is its closer's home.
      {BlockKind::Sub, "print 1", true, ""},
      {BlockKind::Sub, "x = 1", true, ""},
      {BlockKind::Function, "y", true, ""},
      // Fields. `Dim` is optional, so the bare form counts, and a reserved word
      // is a legal field name — the `as` clause is what decides.
      {BlockKind::Type, "x as single", true, ""},
      {BlockKind::Type, "as integer x", true, ""},
      {BlockKind::Type, "dim n as integer", true, ""},
      {BlockKind::Type, "b(3) as byte", true, ""},
      {BlockKind::Type, "redim q(3) as byte", true, ""},
      {BlockKind::Type, "static s as integer", true, ""},
      {BlockKind::Type, "const c = 1", true, ""},
      {BlockKind::Type, "declare sub go()", true, ""},
      {BlockKind::Type, "type inner", true, ""},
      {BlockKind::Type, "union u", true, ""},
      {BlockKind::Type, "enum e", true, ""},
      {BlockKind::Type, "public:", true, ""},
      {BlockKind::Type, "private:", true, ""},
      {BlockKind::Type, "protected:", true, ""},
      {BlockKind::Type, "rem a note", true, ""},
      {BlockKind::Type, "' a comment", true, ""},
      {BlockKind::Type, "DIM n AS INTEGER", true, ""}, // case-insensitive
      // A member procedure spelled with its body: fbc wants `Declare Sub` here
      // (`error 17`), but this parser reads it as a member (FreeBASIC.md §12),
      // so it must not become a boundary.
      {BlockKind::Type, "sub bump()", true, ""},
      {BlockKind::Type, "function f() as integer", true, ""},
      // Boundaries in a TYPE/UNION body — fbc `error 17` on each.
      {BlockKind::Type, "print 1", false, ""},
      {BlockKind::Type, "x", false, ""},
      {BlockKind::Type, "field = 4", false, ""}, // an opener-line modifier
      {BlockKind::Type, "x += 1", false, ""},
      {BlockKind::Type, "p.x = 1.5", false, ""},
      {BlockKind::Type, "x()", false, ""},
      {BlockKind::Type, "goto foo", false, ""},
      {BlockKind::Type, "if x then", false, ""},
      {BlockKind::Type, "for i = 1 to 2", false, ""},
      {BlockKind::Type, "with o", false, ""},
      {BlockKind::Type, "asm", false, ""},
      {BlockKind::Type, "namespace n", false, ""},
      {BlockKind::Type, "scope", false, ""},
      {BlockKind::Type, "var v", false, ""},
      {BlockKind::Type, "local v", false, ""},
      {BlockKind::Type, "common c2", false, ""},
      {BlockKind::Type, "export", false, ""},
      {BlockKind::Type, "dim shared g as integer", false, ""},
      {BlockKind::Type, "redim preserve q(3)", false, ""},
      {BlockKind::Type, "foo:", false, ""},     // a label
      {BlockKind::Type, "1 = 2", false, ""},    // a number starts an expression
      {BlockKind::Union, "public:", false, ""}, // sections are TYPE-only
      {BlockKind::Union, "private:", false, ""},
      {BlockKind::Union, "dim n as integer", true, ""},
      // An enum body is `name`, `name = expr`.
      {BlockKind::Enum, "a", true, ""},
      {BlockKind::Enum, "red = 1", true, ""},
      {BlockKind::Enum, "print 1", false, ""},
      {BlockKind::Enum, "dim n as integer", false, ""},
      {BlockKind::Enum, "public:", false, ""},
      {BlockKind::Enum, "as integer a", false, ""},
      // A reserved word is a legal enum member name for 230 of the 365 — the
      // I/O and intrinsic statement words above all — so an enum body is not
      // the
      // boundary every keyword used to be. `name = expr` and nothing more: the
      // tail is judged too, which is what fbc's `error 3` is about.
      {BlockKind::Enum, "print", true, ""},
      {BlockKind::Enum, "PRINT", true, ""}, // case-insensitive
      {BlockKind::Enum, "stop = 1", true, ""},
      {BlockKind::Enum, "data = 1 + 2", true, ""},
      {BlockKind::Enum, "input = -1", true, ""},
      {BlockKind::Enum, "a = 1 + 2", true, ""},
      {BlockKind::Enum, "a(3)", false, ""},
      {BlockKind::Enum, "print a = 1", false, ""},
      {BlockKind::Enum, "x as integer", false, ""},
      // ...and the 135 it refuses keep the answer they had, a boundary, with
      // the name named on the way out. A bare closer word is one of them: an
      // enum body is not a loop body, so nothing above it wants `next`.
      {BlockKind::Enum, "sub", false, ""},
      {BlockKind::Enum, "and", false, ""},
      {BlockKind::Enum, "type", false, ""},
      {BlockKind::Enum, "next", false, ""},
      {BlockKind::Enum, "wend", false, ""},
      {BlockKind::Enum, "loop", false, ""},
      {BlockKind::Enum, "end", false, ""},
      {BlockKind::Enum, "1", false, ""},
      // ...`rem` among them, and for a reason of its own: fbc reads the line as
      // a comment, so the enum ends up empty. The body is what a probe has to
      // get right here — with a second member present the source compiles, and
      // `rem` looks legal. By the time a statement reaches this function the
      // lexer has already called `rem` a comment, so the rows below are the
      // comment rows: nothing to judge. The empty enum fbc then objects to
      // (`error 256`) is a body-level check this server does not make; §12.
      {BlockKind::Enum, "rem", true, ""},
      {BlockKind::Enum, "rem note", true, ""},
      // A closer is never a member, but it is not a boundary either: the
      // parser's own closer path owns it, and a mismatching one is that path's
      // evidence.
      {BlockKind::Type, "end type", true, ""},
      {BlockKind::Type, "end sub", true, ""},
      {BlockKind::Type, "next", true, ""},
      {BlockKind::Type, "next i", true, ""}, // names the loop variable
      {BlockKind::Type, "loop until x = 0", true, ""},
      {BlockKind::Type, "loop while x", true, ""},
      {BlockKind::Type, "wend", true, ""},
      {BlockKind::Type, "loop", true, ""},
      {BlockKind::Enum, "end enum", true, ""},
      // ...which is why a closer *word* is not enough to end the body: those
      // words are legal field names, and `Next As Node Ptr` is the canonical
      // linked list (fbc accepts keyword field names; error 238 only when the
      // type holds member functions too).
      {BlockKind::Type, "next as node ptr", true, ""},
      {BlockKind::Type, "loop as integer", true, ""},
      {BlockKind::Type, "wend as integer", true, ""},
      {BlockKind::Type, "end as integer", true, ""},
      {BlockKind::Type, "end(3) as integer", true, ""},
      // Nothing to judge: a blank line, or one the parser never calls a
      // statement.
      {BlockKind::Type, "", true, ""},
      // The member the language cannot have: a field of the record declaring
      // it. fbc `error 88`, and no form but `ptr` or `static` escapes it — an
      // array dimension does not, and neither does a type suffix (`fb` ignores
      // one, warning 44).
      {BlockKind::Type, "dim p as point", false, "point"},
      {BlockKind::Type, "p as point", false, "point"},
      {BlockKind::Type, "as point p", false, "point"},
      {BlockKind::Type, "dim p(4) as point", false, "point"},
      {BlockKind::Type, "dim p as point(10)", false, "point"},
      {BlockKind::Type, "redim p(3) as point", false, "point"},
      {BlockKind::Type, "dim p as point, q as integer", false, "point"},
      {BlockKind::Type, "dim p as POINT", false, "point"},
      {BlockKind::Type, "dim p as point$", false, "point"},
      {BlockKind::Union, "dim p as u", false, "u"},
      {BlockKind::Type, "dim p as point ptr", true, "point"},
      {BlockKind::Type, "p as point ptr", true, "point"},
      {BlockKind::Type, "static p as point", true, "point"},
      {BlockKind::Type, "const c as point = 0", true, "point"},
      // Another type, an unrelated name that only looks like it, and a record
      // with no name of its own: none is a self-reference.
      {BlockKind::Type, "dim p as cell", true, "point"},
      {BlockKind::Type, "dim p as points", true, "point"},
      {BlockKind::Type, "dim p as point", true, ""},
      {BlockKind::Type, "dim p as point", true, "other"},
  };
  for (Row const &row : rows) {
    std::vector<Token> const stmt = statementOf(row.stmt);
    bool const got = acceptsBodyMember(row.kind, stmt, row.enclosing);
    if (got != row.want) {
      std::printf(
          "FAIL acceptsBodyMember(kind %d, \"%s\", \"%s\") = %s, want %s\n",
          static_cast<int>(row.kind), row.stmt, row.enclosing,
          got ? "true" : "false", row.want ? "true" : "false");
      ++failures;
    }
  }
  // An empty statement list is not a boundary, whatever the body.
  CHECK(acceptsBodyMember(BlockKind::Type, {}, "point"));
  CHECK(acceptsBodyMember(BlockKind::Enum, {}, "point"));
}

// The probed member-name tables, over the whole reserved-word catalog. The
// probe is `tools/probe_member_names.sh`, which compiles one `type`/`enum` per
// reserved word against fbc 1.10.2 and reports which words each body kind
// accepts; the answer is 16 words no record field may be named, 119 that a
// plain record accepts but fbc's `error 238` refuses in a record that also
// holds a member procedure, and 135 that no enum member may be named. The enum
// count is derived, not tabulated, and the derivation is what this checks.
static void testMemberNameTables() {
  // The 16, spelled out: this list *is* the `invalid-member-name` diagnostic
  // for a record field, so an edit to the table that adds or drops a word has
  // to edit this list too, which is what makes the pair reviewable.
  static char const *const kNever[] = {
      "and", "andalso", "const",  "delete",  "eqv", "imp", "mod", "new",
      "not", "or",      "orelse", "pointer", "ptr", "shl", "shr", "xor",
  };
  std::size_t const nNever = sizeof(kNever) / sizeof(kNever[0]);
  CHECK(nNever == 16);

  for (char const *const w : kNever) {
    if (!isNeverFieldName(w)) {
      std::printf("FAIL isNeverFieldName(\"%s\") = false, want true\n", w);
      ++failures;
    }
  }

  std::size_t nEnumLegal = 0;
  std::size_t nNeverSeen = 0;
  std::size_t nConditionalSeen = 0;
  for (std::string_view const w : reservedWords()) {
    bool const never = isNeverFieldName(w);
    bool const conditional = isConditionalFieldName(w);
    // Disjointness: a word fbc refuses as a field name outright cannot also be
    // one it accepts in a plain record — the two answers would contradict.
    if (never && conditional) {
      std::printf("FAIL \"%s\" is in both member-name tables\n",
                  std::string(w).c_str());
      ++failures;
    }
    nNeverSeen += never ? 1 : 0;
    nConditionalSeen += conditional ? 1 : 0;
    // The derived enum rule, checked against its own definition over every word
    // in the catalog rather than against a third hand-kept list:
    //     enum-illegal(135) == never-field(16) + conditional(119)
    bool const wantEnumLegal = !never && !conditional;
    if (isLegalEnumMemberName(w) != wantEnumLegal) {
      std::printf("FAIL isLegalEnumMemberName(\"%s\") = %d, want %d\n",
                  std::string(w).c_str(), isLegalEnumMemberName(w) ? 1 : 0,
                  wantEnumLegal ? 1 : 0);
      ++failures;
    }
    nEnumLegal += isLegalEnumMemberName(w) ? 1 : 0;
  }
  CHECK(reservedWords().size() == 365);
  CHECK(nNeverSeen == nNever);
  CHECK(nConditionalSeen == 119);
  CHECK(nEnumLegal == 230);

  // A word that is not reserved is in neither table. The enum predicate is
  // vacuously true for one — its rule is derived from the two tables, and a
  // word in neither has nothing against it, which is also the right answer:
  // an identifier is of course a legal enum member name.
  for (char const *const w : {"counter", "and_", "x", "", "print2"}) {
    CHECK(!isNeverFieldName(w));
    CHECK(!isConditionalFieldName(w));
    CHECK(isLegalEnumMemberName(w));
  }
  // `rem` is the word that looks like the exception to the enum rule and is
  // not: fbc reads a `rem` line as a *comment*, so no member named `rem` is
  // created and an enum holding nothing else is `error 256`. A probe whose body
  // held a second member could not tell that apart from a legal name and
  // reported `rem` as the one word accepted as an enum member yet refused under
  // `error 238`; the enum count is what caught it. The lexer is the layer that
  // answers it, which is why the parser asks this predicate nothing about it
  // (testAcceptsBodyMember sees a comment, not a statement).
  CHECK(isConditionalFieldName("rem"));
  CHECK(!isNeverFieldName("rem"));
  CHECK(!isLegalEnumMemberName("rem"));
  // The repair is one underscore away, which is what the quick fix writes.
  CHECK(isLegalEnumMemberName("rem_"));

  // The words that made an enum body look broken before the tables existed: the
  // I/O and intrinsic statement names, which fbc accepts as enum members.
  for (char const *const w : {"print", "stop", "data", "input", "line", "put",
                              "get", "error", "sleep", "width"}) {
    if (!isLegalEnumMemberName(w)) {
      std::printf("FAIL isLegalEnumMemberName(\"%s\") = false, want true\n", w);
      ++failures;
    }
  }
  // And the other direction: a word fbc refuses in an enum body is not silently
  // accepted, which is what `acceptsBodyMember` keys on.
  for (char const *const w : {"and", "type", "end", "next", "as", "const",
                              "ptr", "public", "dim", "if"}) {
    if (isLegalEnumMemberName(w)) {
      std::printf("FAIL isLegalEnumMemberName(\"%s\") = true, want false\n", w);
      ++failures;
    }
  }
  // A keyword field name is legal in a plain record — that is the whole reason
  // `next as node ptr` is the canonical linked list — so the field question has
  // its own answer, separate from the enum one.
  CHECK(isConditionalFieldName("next"));
  CHECK(isConditionalFieldName("end"));
  CHECK(!isLegalEnumMemberName("next"));

  // The twelve reserved words with their probed member-name answers
  // (tools/probe_member_names.sh): seven are conditional field names — illegal
  // as enum members and refused by `error 238` in an armed record — and five,
  // the quirk words, are legal everywhere, as a field, as an enum member, and
  // beside a member function. The two answers on one word are the evidence
  // that keyword legality is not one boolean.
  struct WordAnswer {
    char const *word;
    bool conditional; // legal plain-record field, refused when armed
    bool enumLegal;
  };
  WordAnswer const kNewReservedWords[] = {
      {"__fastcall", true, false}, {"__thiscall", true, false},
      {"cva_arg", true, false},    {"cva_copy", true, false},
      {"cva_end", true, false},    {"cva_start", true, false},
      {"defulng", false, true},    {"dynamic", false, true},
      {"include", false, true},    {"on", false, true},
      {"option", false, true},     {"va_first", true, false},
  };
  for (WordAnswer const &w : kNewReservedWords) {
    if (!isReservedWord(w.word)) {
      std::printf("FAIL isReservedWord(\"%s\") = false, want true\n", w.word);
      ++failures;
    }
    if (isNeverFieldName(w.word)) {
      std::printf("FAIL isNeverFieldName(\"%s\") = true, want false\n", w.word);
      ++failures;
    }
    if (isConditionalFieldName(w.word) != w.conditional) {
      std::printf("FAIL isConditionalFieldName(\"%s\") = %d, want %d\n", w.word,
                  isConditionalFieldName(w.word) ? 1 : 0,
                  w.conditional ? 1 : 0);
      ++failures;
    }
    if (isLegalEnumMemberName(w.word) != w.enumLegal) {
      std::printf("FAIL isLegalEnumMemberName(\"%s\") = %d, want %d\n", w.word,
                  isLegalEnumMemberName(w.word) ? 1 : 0, w.enumLegal ? 1 : 0);
      ++failures;
    }
  }
  // Their docs-pages rows (the naive KeyPg<word> rule is wrong for all of
  // them): the URLs keywordDocsUrl builds must name the real pages.
  CHECK(keywordDocsUrl("__fastcall").find("KeyPgFastcall") !=
        std::string::npos);
  CHECK(keywordDocsUrl("__thiscall").find("KeyPgThiscall") !=
        std::string::npos);
  CHECK(keywordDocsUrl("cva_arg").find("KeyPgCvaArg") != std::string::npos);
  // `defulng` is the one `def*` word with no wiki page yet; the row points
  // it at KeyPgDefulng, the name its siblings follow, explicitly rather
  // than through the naive rule that builds the same URL today.
  CHECK(keywordDocsUrl("defulng").find("KeyPgDefulng") != std::string::npos);
  CHECK(keywordDocsUrl("on").find("KeyPgOngoto") != std::string::npos);
  CHECK(keywordDocsUrl("dynamic").find("KeyPgOptiondynamic") !=
        std::string::npos);
  CHECK(keywordDocsUrl("va_first").find("KeyPgVaFirst") != std::string::npos);
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
  testMemberNameTables();
  testFolderNameCatalog();

  if (failures == 0) {
    std::printf("language_checks: all passed\n");
    return 0;
  }
  std::printf("language_checks: %d failure(s)\n", failures);
  return 1;
}
