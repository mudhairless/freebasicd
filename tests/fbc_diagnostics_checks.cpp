/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// fbc catalog checks: the imported data (tools/fbc_catalog.tsv), the generated
// include (src/fbc_diagnostics.inc) that carries it, and the runtime lookup in
// src/fbc_diagnostics.cpp.
//
// The freshness half regenerates the include in memory through the same
// emitter `gen_fbc_catalog` writes with, then byte-diffs the committed copy;
// the reader half pins the numbers a fbc user recognizes (42, 14, warning 12)
// and the code identity the LSP carries (`fbc error: 42`). The catalog is
// imported, not read from the compiler tree, so this test has no external
// dependency (AGENTS.md, "import appropriate corpus checks into our tests").

#include "fbc_catalog.h"
#include "fbc_diagnostics.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef FBC_CATALOG_TSV
#define FBC_CATALOG_TSV "tools/fbc_catalog.tsv"
#endif

#ifndef FBC_CATALOG_INCLUDE
#define FBC_CATALOG_INCLUDE "src/fbc_diagnostics.inc"
#endif

static int failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

namespace {

std::string readFile(std::string const &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void TestFreshness() {
  std::string const tsv = readFile(FBC_CATALOG_TSV);
  CHECK(!tsv.empty());
  std::string const committed = readFile(FBC_CATALOG_INCLUDE);
  CHECK(!committed.empty());
  if (tsv.empty() || committed.empty()) {
    return;
  }
  std::string const regenerated =
      fbc_catalog::emitCatalogInclude(fbc_catalog::parseCatalog(tsv));
  if (regenerated != committed) {
    std::printf("FAIL: %s is stale; regenerate it with\n"
                "      cmake --build <build> --target fbc-catalog\n",
                FBC_CATALOG_INCLUDE);
    ++failures;
  }
}

void TestSnapshotShape() {
  std::string const tsv = readFile(FBC_CATALOG_TSV);
  std::vector<fbc_catalog::Entry> const entries =
      fbc_catalog::parseCatalog(tsv);
  int errors = 0;
  int warnings = 0;
  int emptyText = 0;
  for (fbc_catalog::Entry const &entry : entries) {
    entry.warning ? ++warnings : ++errors;
    CHECK(!entry.name.empty());
    // fbc's own catalog carries exactly one empty message: warning 37
    // AMBIGIOUSLENSIZEOF has @"" in error.bas. Preserve the quirk; if a
    // regeneration ever changes it, that is a fact worth being told about.
    if (entry.text.empty()) {
      ++emptyText;
      CHECK(entry.warning && entry.number == 37 &&
            entry.name == "AMBIGIOUSLENSIZEOF");
    }
    if (entry.warning) {
      CHECK(entry.level >= 0 && entry.level <= 3);
    } else {
      CHECK(entry.level == 0);
    }
  }
  CHECK(errors == 328);
  CHECK(warnings == 49);
  CHECK(emptyText == 1);
}

void TestRuntimeLookup() {
  CHECK(fbcMessageCount(fblang::FbcMessageKind::Error) == 328);
  CHECK(fbcMessageCount(fblang::FbcMessageKind::Warning) == 49);

  using fblang::FbcMessageKind;
  fblang::FbcCatalogEntry const *first =
      fblang::fbcMessage(FbcMessageKind::Error, 1);
  CHECK(first != nullptr);
  if (first != nullptr) {
    CHECK(first->name == "ARGCNTMISMATCH");
    CHECK(first->text == "Argument count mismatch");
    CHECK(first->kind == FbcMessageKind::Error);
  }
  fblang::FbcCatalogEntry const *e42 =
      fblang::fbcMessage(FbcMessageKind::Error, 42);
  CHECK(e42 != nullptr && e42->text == "Variable not declared");
  fblang::FbcCatalogEntry const *e14 =
      fblang::fbcMessage(FbcMessageKind::Error, 14);
  CHECK(e14 != nullptr && e14->name == "EXPECTEDIDENTIFIER");

  fblang::FbcCatalogEntry const *w12 =
      fblang::fbcMessage(FbcMessageKind::Warning, 12);
  CHECK(w12 != nullptr);
  if (w12 != nullptr) {
    CHECK(w12->text == "Missing closing quote in literal string");
    CHECK(w12->level == 1);
    CHECK(w12->kind == FbcMessageKind::Warning);
  }
  // Warning 40 is off by default (level 0); 49 is the highest number.
  fblang::FbcCatalogEntry const *w40 =
      fblang::fbcMessage(FbcMessageKind::Warning, 40);
  CHECK(w40 != nullptr && w40->level == 0);
  fblang::FbcCatalogEntry const *w49 =
      fblang::fbcMessage(FbcMessageKind::Warning, 49);
  CHECK(w49 != nullptr && w49->name == "UPCASTDISCARDSINITIALIZER");

  // Out of range, in both directions and on both kinds.
  CHECK(fblang::fbcMessage(FbcMessageKind::Error, 0) == nullptr);
  CHECK(fblang::fbcMessage(FbcMessageKind::Error, 329) == nullptr);
  CHECK(fblang::fbcMessage(FbcMessageKind::Warning, 0) == nullptr);
  CHECK(fblang::fbcMessage(FbcMessageKind::Warning, 50) == nullptr);

  // Every number in range resolves to itself.
  for (int n = 1; n <= 328; ++n) {
    fblang::FbcCatalogEntry const *entry =
        fblang::fbcMessage(FbcMessageKind::Error, n);
    CHECK(entry != nullptr && entry->number == n &&
          entry->kind == FbcMessageKind::Error);
  }
  for (int n = 1; n <= 49; ++n) {
    fblang::FbcCatalogEntry const *entry =
        fblang::fbcMessage(FbcMessageKind::Warning, n);
    CHECK(entry != nullptr && entry->number == n &&
          entry->kind == FbcMessageKind::Warning);
  }
}

void TestCodeIdentity() {
  using fblang::FbcMessageKind;
  CHECK(fblang::fbcCode(FbcMessageKind::Error, 42) == "fbc error: 42");
  CHECK(fblang::fbcCode(FbcMessageKind::Warning, 12) == "fbc warning: 12");
  CHECK(fblang::fbcCode(FbcMessageKind::Error, 0).empty());
  CHECK(fblang::fbcCode(FbcMessageKind::Warning, 99).empty());
  CHECK(fblang::fbcMessageDocsUrl() ==
        "https://www.freebasic.net/wiki/CompilerErrMsg");
}

} // namespace

int main() {
  TestFreshness();
  TestSnapshotShape();
  TestRuntimeLookup();
  TestCodeIdentity();

  if (failures == 0) {
    std::printf("fbc_diagnostics_checks: all passed\n");
    return 0;
  }
  std::printf("fbc_diagnostics_checks: %d failure(s)\n", failures);
  return 1;
}
