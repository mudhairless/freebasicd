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
// and the fbc half the session composes into the LSP code (`fbc error: 42`; the
// wire code is our routing key then that, e.g. `include-not-found, fbc error:
// 23`). The catalog is
// imported, not read from the compiler tree, so this test has no external
// dependency (AGENTS.md, "import appropriate corpus checks into our tests").

#include "fbc_catalog.h"
#include "fbc_diagnostics.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
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

void TestVersionedNumber() {
  using fblang::FbcDiagId;
  using fblang::FbcMessageKind;
  using fblang::FbcVersion;
  std::optional<FbcVersion> const v1102 = FbcVersion{1, 10, 2};

  // A representative error and warning at the probed 1.10.x version.
  CHECK(fblang::fbcNumber(FbcDiagId::VariableNotDeclared, FbcMessageKind::Error,
                          v1102) == 42);
  CHECK(fblang::fbcNumber(FbcDiagId::SuffixIgnored, FbcMessageKind::Warning,
                          v1102) == 44);

  // The two identities that hold 44 today are in different spaces.
  CHECK(fblang::fbcNumber(FbcDiagId::SuffixIgnored, FbcMessageKind::Warning,
                          v1102) == 44);
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalOutsideComp, FbcMessageKind::Error,
                          v1102) == 44);
  CHECK(fblang::fbcNumber(FbcDiagId::SuffixIgnored, FbcMessageKind::Error,
                          v1102) == 0);
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalOutsideComp,
                          FbcMessageKind::Warning, v1102) == 0);

  // An unknown version and one below every known range both resolve through
  // the 1.10 table.
  std::optional<FbcVersion> const unknown;
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedEndSub, FbcMessageKind::Error,
                          unknown) == 125);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedEndSub, FbcMessageKind::Error,
                          FbcVersion{1, 9, 9}) == 125);

  // The `select` family, each pinned to the catalog number the
  // probe read back from fbc 1.10.3 (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalInsideSelect, FbcMessageKind::Error,
                          v1102) == 62);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedCase, FbcMessageKind::Error,
                          v1102) == 34);
  CHECK(fblang::fbcNumber(FbcDiagId::CaseWithoutSelect, FbcMessageKind::Error,
                          v1102) == 118);
  CHECK(fblang::fbcNumber(FbcDiagId::EndSelectWithoutSelect,
                          FbcMessageKind::Error, v1102) == 111);
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalOutsideSelect,
                          FbcMessageKind::Error, v1102) == 242);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedSelect, FbcMessageKind::Error,
                          v1102) == 246);
  CHECK(fblang::fbcNumber(FbcDiagId::NoEnclosedSelect, FbcMessageKind::Error,
                          v1102) == 250);
  CHECK(fblang::fbcNumber(FbcDiagId::InvalidCaseRange, FbcMessageKind::Error,
                          v1102) == 323);

  // The loop families: the landed closer pair plus the new
  // mismatch/warning identities, each pinned to the catalog number
  // (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedNext, FbcMessageKind::Error,
                          v1102) == 13);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedLoop, FbcMessageKind::Error,
                          v1102) == 29);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedWend, FbcMessageKind::Error,
                          v1102) == 30);
  CHECK(fblang::fbcNumber(FbcDiagId::LoopWithoutDo, FbcMessageKind::Error,
                          v1102) == 106);
  CHECK(fblang::fbcNumber(FbcDiagId::NextWithoutFor, FbcMessageKind::Error,
                          v1102) == 107);
  CHECK(fblang::fbcNumber(FbcDiagId::WendWithoutWhile, FbcMessageKind::Error,
                          v1102) == 108);
  CHECK(fblang::fbcNumber(FbcDiagId::ForNextVariableMismatch,
                          FbcMessageKind::Error, v1102) == 283);
  CHECK(fblang::fbcNumber(FbcDiagId::NextVarMeaningless,
                          FbcMessageKind::Warning, v1102) == 26);

  // The `exit`/`continue` target family, each pinned to the
  // catalog number the probe read back from fbc 1.10.3
  // (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::InvalidExitStmt, FbcMessageKind::Error,
                          v1102) == 251);
  CHECK(fblang::fbcNumber(FbcDiagId::InvalidContinueStmt, FbcMessageKind::Error,
                          v1102) == 252);

  // The placement / branch-crossing families, each pinned to the
  // catalog number the probe read back from fbc 1.10.3
  // (tools/fbc_catalog.tsv). The two `44` identities share the number in
  // different spaces and must stay distinct.
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalInsideSub, FbcMessageKind::Error,
                          v1102) == 61);
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalInsideScope, FbcMessageKind::Error,
                          v1102) == 96);
  CHECK(fblang::fbcNumber(FbcDiagId::BranchCrossingDynDataDef,
                          FbcMessageKind::Error, v1102) == 105);
  CHECK(fblang::fbcNumber(FbcDiagId::BranchCrossingLocalVar,
                          FbcMessageKind::Warning, v1102) == 14);
  CHECK(fblang::fbcNumber(FbcDiagId::IllegalInsideSub, FbcMessageKind::Warning,
                          v1102) == 0);
  CHECK(fblang::fbcNumber(FbcDiagId::BranchCrossingDynDataDef,
                          FbcMessageKind::Warning, v1102) == 0);
  CHECK(fblang::fbcNumber(FbcDiagId::BranchCrossingLocalVar,
                          FbcMessageKind::Error, v1102) == 0);

  // The punctuator family, each pinned to the catalog number the
  // probe read back from fbc 1.10.3 (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedLprnt, FbcMessageKind::Error,
                          v1102) == 6);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedRprnt, FbcMessageKind::Error,
                          v1102) == 7);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedEq, FbcMessageKind::Error,
                          v1102) == 10);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedComma, FbcMessageKind::Error,
                          v1102) == 16);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedRBracket, FbcMessageKind::Error,
                          v1102) == 66);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedGt, FbcMessageKind::Error,
                          v1102) == 309);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedConst, FbcMessageKind::Error,
                          v1102) == 11);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedRestype, FbcMessageKind::Error,
                          v1102) == 68);
  CHECK(fblang::fbcNumber(FbcDiagId::TooManyExpressions, FbcMessageKind::Error,
                          v1102) == 67);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedSub, FbcMessageKind::Error,
                          v1102) == 289);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedRprnt, FbcMessageKind::Warning,
                          v1102) == 0);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedGt, FbcMessageKind::Warning,
                          v1102) == 0);

  // The statement-specific family, each pinned to the catalog
  // number the probe read back from fbc 1.10.3 (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedDynamicArray,
                          FbcMessageKind::Error, v1102) == 54);
  CHECK(fblang::fbcNumber(FbcDiagId::CantInitDynamicArrays,
                          FbcMessageKind::Error, v1102) == 75);
  CHECK(fblang::fbcNumber(FbcDiagId::CantInitDynamicStrings,
                          FbcMessageKind::Error, v1102) == 87);
  CHECK(fblang::fbcNumber(FbcDiagId::DynamicArrayWithEllipsis,
                          FbcMessageKind::Error, v1102) == 76);
  CHECK(fblang::fbcNumber(FbcDiagId::ArrayFieldWithEllipsis,
                          FbcMessageKind::Error, v1102) == 77);
  CHECK(fblang::fbcNumber(FbcDiagId::DynamicExternCantHaveBounds,
                          FbcMessageKind::Error, v1102) == 136);
  CHECK(fblang::fbcNumber(FbcDiagId::DynamicArrayInUnion, FbcMessageKind::Error,
                          v1102) == 162);
  CHECK(fblang::fbcNumber(FbcDiagId::RedimCtorMustBeCdecl,
                          FbcMessageKind::Error, v1102) == 171);
  CHECK(fblang::fbcNumber(FbcDiagId::RedimDtorMustBeCdecl,
                          FbcMessageKind::Error, v1102) == 172);
  CHECK(fblang::fbcNumber(FbcDiagId::RedimCtorMustHaveOneParam,
                          FbcMessageKind::Error, v1102) == 173);
  CHECK(fblang::fbcNumber(FbcDiagId::EraseCtorMustBeCdecl,
                          FbcMessageKind::Error, v1102) == 174);
  CHECK(fblang::fbcNumber(FbcDiagId::EraseDtorMustBeCdecl,
                          FbcMessageKind::Error, v1102) == 175);
  CHECK(fblang::fbcNumber(FbcDiagId::EraseCtorMustHaveOneParam,
                          FbcMessageKind::Error, v1102) == 176);
  CHECK(fblang::fbcNumber(FbcDiagId::PrivOrPubAttribNotAllowed,
                          FbcMessageKind::Error, v1102) == 216);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedFileNumberExpression,
                          FbcMessageKind::Error, v1102) == 327);
  CHECK(fblang::fbcNumber(FbcDiagId::ExpectedDynamicArray,
                          FbcMessageKind::Warning, v1102) == 0);

  // The remaining warnings, each pinned to the catalog number the
  // probe read back from fbc 1.10.3 (tools/fbc_catalog.tsv).
  CHECK(fblang::fbcNumber(FbcDiagId::IfFoundAfterElse, FbcMessageKind::Warning,
                          v1102) == 32);
  CHECK(fblang::fbcNumber(FbcDiagId::NoExplicitParamMode,
                          FbcMessageKind::Warning, v1102) == 15);
  CHECK(fblang::fbcNumber(FbcDiagId::CmdlineIgnored, FbcMessageKind::Warning,
                          v1102) == 46);
  CHECK(fblang::fbcNumber(FbcDiagId::ConvOverflow, FbcMessageKind::Warning,
                          v1102) == 25);
  CHECK(fblang::fbcNumber(FbcDiagId::ShiftExceedsBitsInDataType,
                          FbcMessageKind::Warning, v1102) == 33);
  CHECK(fblang::fbcNumber(FbcDiagId::ForEndTooBig, FbcMessageKind::Warning,
                          v1102) == 45);
  CHECK(fblang::fbcNumber(FbcDiagId::ForEndTooBig, FbcMessageKind::Error,
                          v1102) == 0);

  // No identity is no number, in either space.
  CHECK(fblang::fbcNumber(FbcDiagId::None, FbcMessageKind::Error, v1102) == 0);
  CHECK(fblang::fbcNumber(FbcDiagId::None, FbcMessageKind::Warning, v1102) ==
        0);
}

} // namespace

int main() {
  TestFreshness();
  TestSnapshotShape();
  TestRuntimeLookup();
  TestCodeIdentity();
  TestVersionedNumber();

  if (failures == 0) {
    std::printf("fbc_diagnostics_checks: all passed\n");
    return 0;
  }
  std::printf("fbc_diagnostics_checks: %d failure(s)\n", failures);
  return 1;
}
