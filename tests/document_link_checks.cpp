/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Document-link checks for the M16 module.
//
// Byte-offset and LSP-agnostic, like the module: no index, no session. The only
// workspace state `documentLinks` reaches is the `IncludeResolver` seam, and
// these checks wire it to a fixed table, so what is under test is the part the
// module owns — which includes carry a link, the range, the order, and that an
// include the resolver refuses produces nothing. The resolution itself
// (`resolveIncludeTarget`) is the session's and is pinned by the integration
// driver.

#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "document_link.h"
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

// Byte offset of the filename literal (quotes excluded) of the `n`th include of
// `src`, so a range assertion is expressed against the fixture rather than a
// hand-counted column.
std::size_t literalOffset(std::string const &src, std::size_t n = 0) {
  std::size_t at = src.find('#');
  while (n > 0) {
    at = src.find('#', at + 1);
    --n;
  }
  return src.find('"', at) + 1;
}

// A resolver that resolves the literals in `known` and refuses the rest, and
// records the literals it was asked about.
IncludeResolver resolverFor(std::unordered_map<std::string, std::string> known,
                            std::vector<std::string> *asked) {
  return
      [known, asked](std::string const &literal) -> std::optional<std::string> {
        if (asked != nullptr) {
          asked->push_back(literal);
        }
        auto const it = known.find(literal);
        if (it == known.end()) {
          return std::nullopt;
        }
        return it->second;
      };
}

void TestResolvedIncludesCarryLinksInSourceOrder() {
  std::string const src = "#include \"first.bi\"\n"
                          "#include once \"second.bi\"\n"
                          "sub main()\n"
                          "end sub\n";
  AnalyzedDoc const doc = analyze(src);
  std::vector<std::string> asked;
  std::vector<DocumentLinkItem> const links =
      documentLinks(doc, resolverFor({{"first.bi", "/ws/first.bi"},
                                      {"second.bi", "/ws/second.bi"}},
                                     &asked));

  CHECK(links.size() == 2);
  CHECK(asked == (std::vector<std::string>{"first.bi", "second.bi"}));
  if (links.size() == 2) {
    // Source order, and the target is what the resolver returned unchanged.
    CHECK(links[0].target == "/ws/first.bi");
    CHECK(links[1].target == "/ws/second.bi");
    // `#include once` is the same directive for a link's purposes.
    std::size_t const first = literalOffset(src, 0);
    CHECK(links[0].range.beg == first);
    CHECK(links[0].range.end == first + std::string("first.bi").size());
    std::size_t const second = literalOffset(src, 1);
    CHECK(links[1].range.beg == second);
    CHECK(links[1].range.end == second + std::string("second.bi").size());
    // The range excludes the quotes: the click target is the filename, not the
    // punctuation around it.
    CHECK(src[links[0].range.beg] == 'f');
    CHECK(src[links[0].range.end] == '"');
  }
}

void TestRangeIsTheFilenameNotTheQuotes() {
  std::string const src = "#include \"dir/name.bi\"\n";
  AnalyzedDoc const doc = analyze(src);
  std::vector<DocumentLinkItem> const links = documentLinks(
      doc, resolverFor({{"dir/name.bi", "/x/dir/name.bi"}}, nullptr));
  CHECK(links.size() == 1);
  if (links.size() == 1) {
    CHECK(src.substr(links[0].range.beg,
                     links[0].range.end - links[0].range.beg) == "dir/name.bi");
  }
}

// The decision the module owns: a literal the resolver refuses is not a link.
// The include-not-found diagnostic reports it; a link would point at a file
// that is not there.
void TestUnresolvedIncludesCarryNoLink() {
  std::string const src = "#include \"known.bi\"\n"
                          "#include \"missing.bi\"\n";
  AnalyzedDoc const doc = analyze(src);
  std::vector<std::string> asked;
  std::vector<DocumentLinkItem> const links =
      documentLinks(doc, resolverFor({{"known.bi", "/ws/known.bi"}}, &asked));

  CHECK(links.size() == 1);
  if (links.size() == 1) {
    CHECK(links[0].target == "/ws/known.bi");
  }
  // The refused literal is still asked about: the resolver is the only thing
  // that can know, so the module must not skip it on a guess.
  CHECK(asked == (std::vector<std::string>{"known.bi", "missing.bi"}));
}

// A bare `#include` parses to an empty literal with a zero-width range. There
// is nothing to open, and a link at file offset 0 would be an invisible target.
void TestBareIncludeCarriesNoLink() {
  AnalyzedDoc const doc = analyze("#include\n#include \"real.bi\"\n");
  std::vector<std::string> asked;
  std::vector<DocumentLinkItem> const links =
      documentLinks(doc, resolverFor({{"real.bi", "/ws/real.bi"}}, &asked));

  CHECK(links.size() == 1);
  CHECK(asked == (std::vector<std::string>{"real.bi"}));
}

void TestEmptyAndPlainDocuments() {
  CHECK(documentLinks(analyze(""), resolverFor({}, nullptr)).empty());
  CHECK(documentLinks(analyze("dim x as integer\n"), resolverFor({}, nullptr))
            .empty());
}

// A session with no index still answers the request: a null seam yields no
// links rather than a crash, because the seam is optional.
void TestNullResolverYieldsNoLinks() {
  AnalyzedDoc const doc = analyze("#include \"a.bi\"\n");
  CHECK(documentLinks(doc, nullptr).empty());
}

} // namespace

int main() {
  TestResolvedIncludesCarryLinksInSourceOrder();
  TestRangeIsTheFilenameNotTheQuotes();
  TestUnresolvedIncludesCarryNoLink();
  TestBareIncludeCarriesNoLink();
  TestEmptyAndPlainDocuments();
  TestNullResolverYieldsNoLinks();

  if (failures == 0) {
    std::printf("document_link_checks: all passed\n");
    return 0;
  }
  std::printf("document_link_checks: %d failures\n", failures);
  return 1;
}
