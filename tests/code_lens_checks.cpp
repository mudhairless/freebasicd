/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Code-lens checks for the M13 module (aspect 3).
//
// Byte-offset and LSP-agnostic, like the module: no index, no session. The only
// workspace state `codeLenses` reaches is the `ReferenceCounter` seam, and
// these checks wire it to a fixed table, so what is under test is the part the
// module owns — which declarations carry a lens, in what order, and the
// localized title each count gets. The count itself is a session-side walk
// (`FreeBasicServer::referenceSites`) and is pinned by the integration driver.
//
// The title assertions are the reason this suite exists: the plural is chosen
// by ngettext, so a count of 1 must not read "1 references" — the failure that
// a two-msgid hand-rolled plural would have shipped in every language whose
// rule has more than two forms.

#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "code_lens.h"
#include "resolve.h"
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

namespace {

// The declarations a lens may anchor on, and the ones it must ignore: a module
// Sub, a Function, a Type with a member procedure, an Enum, a Namespace, a
// module-level Const, a Dim, a parameter, and a local inside a body. The
// member procedure is the case that decides whether nesting is flattened.
char const kFixture[] = "const kLimit = 3\n"
                        "dim shared counter as integer\n"
                        "namespace tools\n"
                        "end namespace\n"
                        "enum mode\n"
                        "    modeOff\n"
                        "end enum\n"
                        "type point\n"
                        "    x as integer\n"
                        "    sub move()\n"
                        "    end sub\n"
                        "end type\n"
                        "sub mainProc(byval n as integer)\n"
                        "    dim scratch as integer\n"
                        "    counter = n\n"
                        "end sub\n";

// The names of a document's lens anchors, in the order the lenses come back.
std::vector<std::string> anchorNames(AnalyzedDoc const &doc) {
  std::vector<std::string> names;
  for (CodeLens const &lens :
       codeLenses(doc, [](LensAnchor const &) { return std::size_t{0}; })) {
    names.push_back(lens.anchor.name);
  }
  return names;
}

// The anchor named `name`, or nullptr. A `find` on a name table returns the
// first hit, which is the declaration here only because names are unique in
// this fixture — a suite that wanted a duplicate name would have to anchor on
// the name *and* the line, like the call-hierarchy suite does.
LensAnchor const *anchorNamed(std::vector<CodeLens> const &lenses,
                              std::string const &name) {
  for (CodeLens const &lens : lenses) {
    if (lens.anchor.name == name) {
      return &lens.anchor;
    }
  }
  return nullptr;
}

void TestAnchorsAreTheNamedDeclarations() {
  AnalyzedDoc const doc = analyze(kFixture);
  std::vector<std::string> const names = anchorNames(doc);

  // Source order, and only the procedure-like kinds and the type-ish roots. A
  // lens per `dim` would bury the file, and `kLimit`/`counter` are module state
  // a reader does not navigate to.
  std::vector<std::string> const expected{"tools", "mode", "point", "move",
                                          "mainProc"};
  CHECK(names == expected);
  if (names != expected) {
    std::printf("  anchors:");
    for (std::string const &n : names) {
      std::printf(" %s", n.c_str());
    }
    std::printf("\n");
  }
}

// A declaration written without a name gets no lens. Its selection is now the
// block's own opener keyword (so it lies inside its range like every other
// declaration), which means the old zero-width guard would have anchored a
// lens to it: `<anonymous enum>` with a permanent "0 references" and no name
// for the click to carry back. The guard is the same empty `key` the rest of
// the parser reads as "unnamed" — one signal, every consumer.
void TestUnnamedDeclarationsCarryNoLens() {
  AnalyzedDoc const doc = analyze("enum\n"
                                  "    red\n"
                                  "end enum\n"
                                  "type t\n"
                                  "    union\n"
                                  "        dim a as integer\n"
                                  "    end union\n"
                                  "    sub move()\n"
                                  "    end sub\n"
                                  "end type\n");
  std::vector<std::string> const names = anchorNames(doc);
  std::vector<std::string> const expected{"t", "move"};
  CHECK(names == expected);
  if (names != expected) {
    std::printf("  anchors:");
    for (std::string const &n : names) {
      std::printf(" %s", n.c_str());
    }
    std::printf("\n");
  }
}

void TestAnchorIsTheNameToken() {
  AnalyzedDoc const doc = analyze(kFixture);
  std::vector<CodeLens> const lenses =
      codeLenses(doc, [](LensAnchor const &) { return std::size_t{0}; });

  // The anchor must be the name token itself: one line, and a position the
  // click's command can resolve back to this declaration. `sub move()` is
  // indented, so a range measured from the construct's start would not match.
  LensAnchor const *const move = anchorNamed(lenses, "move");
  CHECK(move != nullptr);
  if (move != nullptr) {
    std::size_t const at = std::string(kFixture).find("move");
    CHECK(move->selection.beg == at);
    CHECK(move->selection.end == at + std::string("move").size());
    CHECK(move->kind == SymbolKind::Sub);
  }
  LensAnchor const *const mainProc = anchorNamed(lenses, "mainProc");
  CHECK(mainProc != nullptr);
  if (mainProc != nullptr) {
    CHECK(mainProc->kind == SymbolKind::Sub);
    std::string const src(kFixture);
    CHECK(src.substr(mainProc->selection.beg,
                     mainProc->selection.end - mainProc->selection.beg) ==
          "mainProc");
  }
}

void TestKindTravelsForEveryAnchorKind() {
  AnalyzedDoc const doc = analyze("function f() as integer\n"
                                  "    return 0\n"
                                  "end function\n"
                                  "sub s()\n"
                                  "end sub\n"
                                  "type t\n"
                                  "    dim a as integer\n"
                                  "end type\n");
  std::vector<CodeLens> const lenses =
      codeLenses(doc, [](LensAnchor const &) { return std::size_t{0}; });
  CHECK(lenses.size() == 3);
  if (lenses.size() == 3) {
    CHECK(lenses[0].anchor.kind == SymbolKind::Function);
    CHECK(lenses[1].anchor.kind == SymbolKind::Sub);
    CHECK(lenses[2].anchor.kind == SymbolKind::Type);
  }
}

void TestCountFeedsTheTitle() {
  // The counter is looked up by name, so a lens that asked about a declaration
  // the walk never saw would report zero instead of a wrong number.
  std::unordered_map<std::string, std::size_t> const counts{
      {"point", 0}, {"move", 1}, {"mainProc", 2}, {"tools", 12}};
  AnalyzedDoc const doc = analyze(kFixture);
  std::vector<CodeLens> const lenses =
      codeLenses(doc, [&counts](LensAnchor const &anchor) -> std::size_t {
        auto const it = counts.find(anchor.name);
        return it == counts.end() ? 0 : it->second;
      });

  // ngettext picks the form: singular for exactly 1, plural otherwise. A
  // two-msgid plural hand-rolled in code would print "1 references" here and
  // could not express a four-form language at all.
  for (CodeLens const &lens : lenses) {
    auto const it = counts.find(lens.anchor.name);
    std::size_t const n = it == counts.end() ? 0 : it->second;
    std::string const expected =
        n == 1 ? "1 reference" : std::to_string(n) + " references";
    if (lens.title != expected) {
      std::printf("  %s: title \"%s\", expected \"%s\"\n",
                  lens.anchor.name.c_str(), lens.title.c_str(),
                  expected.c_str());
    }
    CHECK(lens.title == expected);
    CHECK(lens.references == n);
  }
}

void TestCounterCalledOncePerAnchor() {
  AnalyzedDoc const doc = analyze(kFixture);
  std::vector<std::string> asked;
  std::vector<CodeLens> const lenses =
      codeLenses(doc, [&asked](LensAnchor const &anchor) -> std::size_t {
        asked.push_back(anchor.name);
        return 3;
      });
  // One call per anchor, in anchor order: the count is a walk, and asking twice
  // would walk the closure twice for a number the reply already has.
  std::vector<std::string> expected{"tools", "mode", "point", "move",
                                    "mainProc"};
  CHECK(asked == expected);
  for (CodeLens const &lens : lenses) {
    CHECK(lens.title == "3 references");
  }
}

void TestEmptyAndPlainDocuments() {
  // No declarations means no lenses, and a document of only locals means none
  // either: the caller must never have to filter the reply.
  CHECK(anchorNames(analyze("")).empty());
  CHECK(anchorNames(analyze("dim x as integer\n")).empty());
  CHECK(anchorNames(analyze("sub a()\n  dim y as integer\nend sub\n")).size() ==
        1);
  // An unterminated construct still anchors on its declaration, so a lens
  // survives the half-written file a reader is typing.
  CHECK(anchorNames(analyze("sub a()\n  a()\n")).size() == 1);
}

void TestNullCounterCountsZero() {
  // A session with no index still answers the request; every count is zero
  // rather than a crash, because the seam is optional.
  AnalyzedDoc const doc = analyze(kFixture);
  std::vector<CodeLens> const lenses = codeLenses(doc, nullptr);
  CHECK(lenses.size() == 5);
  for (CodeLens const &lens : lenses) {
    CHECK(lens.references == 0);
    CHECK(lens.title == "0 references");
  }
}

} // namespace

int main() {
  TestAnchorsAreTheNamedDeclarations();
  TestUnnamedDeclarationsCarryNoLens();
  TestAnchorIsTheNameToken();
  TestKindTravelsForEveryAnchorKind();
  TestCountFeedsTheTitle();
  TestCounterCalledOncePerAnchor();
  TestEmptyAndPlainDocuments();
  TestNullCounterCountsZero();

  if (failures == 0) {
    std::printf("code_lens_checks: all passed\n");
    return 0;
  }
  std::printf("code_lens_checks: %d failures\n", failures);
  return 1;
}
