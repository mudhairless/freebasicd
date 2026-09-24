/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Semantic-token classification checks for the M9 token-stream classifier.
//
// Exercises the legend spellings (must match LSP 3.17 exactly), the
// tokenstream -> entries walk including UTF-16 column math on non-ASCII
// content (é = 2 UTF-8 bytes / 1 UTF-16 unit), the relative encoding, and the
// viewport filter. All LSP-agnostic: byte offsets + explicit UTF-16 columns.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "resolve.h"
#include "semantic_tokens.h"
#include "symbols.h"

using namespace fblang;

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

int main() {
  // Legend spellings must match the 3.17 standard exactly.
  {
    std::vector<std::string> const types = semanticTokenTypes();
    std::vector<std::string> const expectedTypes = {
        "keyword",  "string",     "number",    "comment",  "macro", "operator",
        "variable", "function",   "method",    "property", "type",  "class",
        "enum",     "enumMember", "parameter", "namespace"};
    CHECK(types == expectedTypes);
    std::vector<std::string> const mods = semanticTokenModifiers();
    std::vector<std::string> const expectedMods = {"declaration", "readonly"};
    CHECK(mods == expectedMods);
  }

  // Fixture: keyword, suffix-typed identifier (dim + usage), operators, a
  // combined assign, a #include line, a comment, and a SUB whose decl name
  // token must carry the declaration modifier. The string "héllo" pins the
  // UTF-16 column math: é is 2 UTF-8 bytes but 1 UTF-16 unit.
  std::string const src = "dim s$ = \"h\xc3\xa9llo\"\n"
                          "print s$ + 2\n"
                          "? s$ and= 3\n"
                          "#include \"x.bi\"\n"
                          "' note\n"
                          "sub hello()\n"
                          "end sub\n";
  AnalyzedDoc const doc = analyze(src);
  std::vector<SemanticTokenEntry> const entries = semanticTokens(doc, src);

  // Legend indices: keyword0 string1 number2 comment3 macro4 operator5
  // variable6 function7 method8 property9 type10 class11 enum12 enumMember13
  // parameter14 namespace15. Modifier bits: declaration=1, readonly=2.
  struct Expect {
    std::uint32_t line;
    std::uint32_t start;
    std::uint32_t len;
    std::uint32_t type;
    std::uint32_t mods;
  };
  std::vector<Expect> const want = {
      {0, 0, 3, 0, 0},  // dim
      {0, 4, 2, 6, 1},  // s$ (decl name, Dim -> variable+declaration)
      {0, 7, 1, 5, 0},  // =
      {0, 9, 7, 1, 0},  // "héllo" -> 7 UTF-16 units (é counts once)
      {1, 0, 5, 0, 0},  // print
      {1, 6, 2, 6, 0},  // s$ usage
      {1, 9, 1, 5, 0},  // +
      {1, 11, 1, 2, 0}, // 2
      {2, 0, 1, 5, 0},  // ? (PRINT shortcut)
      {2, 2, 2, 6, 0},  // s$ usage
      {2, 5, 4, 5, 0},  // and= (combined assign is a keyword token)
      {2, 10, 1, 2, 0}, // 3
      {3, 0, 15, 4, 0}, // whole #include line is one macro token
      {4, 0, 6, 3, 0},  // ' note
      {5, 0, 3, 0, 0},  // sub
      {5, 4, 5, 7, 1},  // hello (decl name, Function+declaration)
      {6, 0, 3, 0, 0},  // end
      {6, 4, 3, 0, 0},  // sub
  };
  CHECK(entries.size() == want.size());
  std::size_t const n =
      want.size() < entries.size() ? want.size() : entries.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (entries[i].line != want[i].line ||
        entries[i].startChar != want[i].start ||
        entries[i].length != want[i].len || entries[i].type != want[i].type ||
        entries[i].modifiers != want[i].mods) {
      std::printf("  entry[%zu]: got {%u,%u,%u,%u,%u} want {%u,%u,%u,%u,%u}\n",
                  i, entries[i].line, entries[i].startChar, entries[i].length,
                  entries[i].type, entries[i].modifiers, want[i].line,
                  want[i].start, want[i].len, want[i].type, want[i].mods);
      CHECK(false);
    }
  }

  // Relative encoding: 5 ints per token, deltas against the previous entry.
  {
    std::vector<std::int32_t> const data = encodeTokenData(entries);
    CHECK(data.size() == entries.size() * 5);
    std::vector<std::int32_t> const wantData = {
        0, 0, 3,  0, 0, // dim
        0, 4, 2,  6, 1, // s$
        0, 3, 1,  5, 0, // =
        0, 2, 7,  1, 0, // "héllo"
        1, 0, 5,  0, 0, // print
        0, 6, 2,  6, 0, // s$
        0, 3, 1,  5, 0, // +
        0, 2, 1,  2, 0, // 2
        1, 0, 1,  5, 0, // ?
        0, 2, 2,  6, 0, // s$
        0, 3, 4,  5, 0, // and=
        0, 5, 1,  2, 0, // 3
        1, 0, 15, 4, 0, // #include
        1, 0, 6,  3, 0, // ' note
        1, 0, 3,  0, 0, // sub
        0, 4, 5,  7, 1, // hello
        1, 0, 3,  0, 0, // end
        0, 4, 3,  0, 0, // sub
    };
    CHECK(data == wantData);
  }

  // Viewport filter: inclusive line range.
  {
    std::vector<SemanticTokenEntry> const mid = filterTokens(entries, 1, 3);
    CHECK(mid.size() == 9);
    for (auto const &e : mid) {
      CHECK(e.line >= 1 && e.line <= 3);
    }
    std::vector<SemanticTokenEntry> const single = filterTokens(entries, 5, 5);
    CHECK(single.size() == 2);
    CHECK(single[0].type == 0 && single[1].type == 7);
  }

  // Enum members are Const children of the Enum root: the declaration tokens
  // classify as enumMember (parentOf-based); a bare module-level usage of a
  // *plain* enum's member resolves to the member (module-scope constant,
  // FreeBASIC.md §8 + KeyPgEnum, fbc-verified) and classifies as enumMember,
  // while the same usage of an `Explicit` enum's member resolves nowhere and
  // falls back to variable.
  {
    std::string const esrc = "enum hue\n"
                             "    red\n"
                             "    green\n"
                             "end enum\n"
                             "dim c as integer\n"
                             "c = red\n";
    AnalyzedDoc const edoc = analyze(esrc);
    std::vector<SemanticTokenEntry> const e = semanticTokens(edoc, esrc);
    auto at = [&](std::uint32_t line, std::uint32_t col) {
      for (auto const &x : e) {
        if (x.line == line && x.startChar == col) {
          return x;
        }
      }
      return SemanticTokenEntry{};
    };
    SemanticTokenEntry const color = at(0, 5);
    CHECK(color.type == 12 && color.modifiers == 1); // enum + declaration
    SemanticTokenEntry const redDecl = at(1, 4);
    CHECK(redDecl.type == 13 && redDecl.modifiers == 1); // enumMember decl
    SemanticTokenEntry const greenDecl = at(2, 4);
    CHECK(greenDecl.type == 13 && greenDecl.modifiers == 1);
    SemanticTokenEntry const cDecl = at(4, 4);
    CHECK(cDecl.type == 6 && cDecl.modifiers == 1); // dim decl
    SemanticTokenEntry const redUsage = at(5, 4);
    CHECK(redUsage.type == 13 && redUsage.modifiers == 0); // enumMember usage
  }

  // `Explicit` enums gate their members behind `Name.member`, so a bare
  // member usage resolves to nothing and classifies as a plain variable.
  {
    std::string const esrc = "enum hue explicit\n"
                             "    red\n"
                             "end enum\n"
                             "dim c as integer\n"
                             "c = red\n";
    AnalyzedDoc const edoc = analyze(esrc);
    std::vector<SemanticTokenEntry> const e = semanticTokens(edoc, esrc);
    for (auto const &x : e) {
      if (x.line == 4 && x.startChar == 4) {
        CHECK(x.type == 6 && x.modifiers == 0); // unresolved, plain variable
      }
    }
  }

  if (failures == 0) {
    std::printf("semantic_tokens_checks: all passed\n");
    return 0;
  }
  std::printf("semantic_tokens_checks: %d failures\n", failures);
  return 1;
}