/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fbc_diagnostics.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace fblang {
namespace {

// The generated arrays and their counts. Only this translation unit includes
// the data; every reader goes through the functions below.
#include "fbc_diagnostics.inc"

FbcCatalogEntry const *tableFor(FbcMessageKind kind) {
  return kind == FbcMessageKind::Error ? kFbcErrors : kFbcWarnings;
}

int countFor(FbcMessageKind kind) {
  return kind == FbcMessageKind::Error ? kFbcErrorCount : kFbcWarningCount;
}

constexpr std::size_t kDiagCount = static_cast<std::size_t>(FbcDiagId::Count);
using DiagNumbers = std::array<int, kDiagCount>;

constexpr DiagNumbers
makeNumbers(std::initializer_list<std::pair<FbcDiagId, int>> entries) {
  DiagNumbers out{};
  for (auto const &entry : entries) {
    out[static_cast<std::size_t>(entry.first)] = entry.second;
  }
  return out;
}

// One fbc version range's numbers. `first` is the first version the entry
// applies to; the delivery selector takes the last entry whose `first` is at or
// below the probed version. Every number here was read back out of fbc by
// `tools/fbc_catalog.tsv` (this repo's imported copy) and re-probed with the
// pinned `/usr/bin/fbc` for every spelling, including the ones the catalog
// names once for a family (19 EXPECTEDENDTYPE covers both TYPE and UNION).
struct FbcNumberRange {
  FbcVersion first;
  DiagNumbers errors;
  DiagNumbers warnings;
};

// fbc 1.10.x. `error.bas` is byte-identical across 1.10.0 to 1.10.3, so one
// entry covers the whole probed range. A future renumbering adds an entry here;
// no call site changes.
constexpr std::array<FbcNumberRange, 1> kNumberRanges{{
    {{1, 10, 0},
     makeNumbers({{FbcDiagId::ExpectedEndSub, 125},
                  {FbcDiagId::ExpectedEndFunction, 126},
                  {FbcDiagId::ExpectedEndCtor, 127},
                  {FbcDiagId::ExpectedEndDtor, 128},
                  {FbcDiagId::ExpectedEndOperator, 129},
                  {FbcDiagId::ExpectedEndProperty, 130},
                  {FbcDiagId::ExpectedEndType, 19},
                  {FbcDiagId::ExpectedEndEnum, 74},
                  {FbcDiagId::ExpectedEndNamespace, 121},
                  {FbcDiagId::ExpectedEndScope, 95},
                  {FbcDiagId::ExpectedEndExtern, 124},
                  {FbcDiagId::ExpectedEndWith, 60},
                  {FbcDiagId::ExpectedEndSelect, 35},
                  {FbcDiagId::ExpectedEndIf, 32},
                  {FbcDiagId::ExpectedNext, 13},
                  {FbcDiagId::ExpectedWend, 30},
                  {FbcDiagId::ExpectedLoop, 29},
                  {FbcDiagId::ExpectedEndAsm, 45},
                  {FbcDiagId::ExpectedPpEndIf, 290},
                  {FbcDiagId::ExpectedMacro, 134},
                  {FbcDiagId::LoopWithoutDo, 106},
                  {FbcDiagId::NextWithoutFor, 107},
                  {FbcDiagId::WendWithoutWhile, 108},
                  {FbcDiagId::EndWithWithoutWith, 109},
                  {FbcDiagId::EndIfWithoutIf, 110},
                  {FbcDiagId::EndSelectWithoutSelect, 111},
                  {FbcDiagId::EndSubWithoutSub, 112},
                  {FbcDiagId::EndScopeWithoutScope, 113},
                  {FbcDiagId::EndNamespaceWithoutNamespace, 114},
                  {FbcDiagId::EndExternWithoutExtern, 115},
                  {FbcDiagId::IllegalEnd, 33},
                  {FbcDiagId::KeywordFieldsNotAllowedInClasses, 238},
                  {FbcDiagId::ExpectedIdentifier, 14},
                  {FbcDiagId::ElseWithoutIf, 117},
                  {FbcDiagId::ElseIfWithoutIf, 116},
                  {FbcDiagId::CaseWithoutSelect, 118},
                  {FbcDiagId::SyntaxError, 17},
                  {FbcDiagId::ExpectedEol, 3},
                  {FbcDiagId::ArgCntMismatch, 1},
                  {FbcDiagId::InvalidPriority, 189},
                  {FbcDiagId::DupDefinition, 4},
                  {FbcDiagId::IllegalOutsideComp, 44},
                  {FbcDiagId::ExpectedExpression, 9},
                  {FbcDiagId::VariableNotDeclared, 42},
                  {FbcDiagId::UndefinedSymbol, 8},
                  {FbcDiagId::FileNotFound, 23},
                  {FbcDiagId::IllegalInsideSelect, 62},
                  {FbcDiagId::ExpectedCase, 34},
                  {FbcDiagId::IllegalOutsideSelect, 242},
                  {FbcDiagId::ExpectedSelect, 246},
                  {FbcDiagId::NoEnclosedSelect, 250},
                  {FbcDiagId::InvalidCaseRange, 323},
                  {FbcDiagId::ForNextVariableMismatch, 283},
                  {FbcDiagId::InvalidExitStmt, 251},
                  {FbcDiagId::InvalidContinueStmt, 252},
                  {FbcDiagId::IllegalInsideSub, 61},
                  {FbcDiagId::IllegalInsideScope, 96},
                  {FbcDiagId::BranchCrossingDynDataDef, 105},
                  {FbcDiagId::ExpectedLprnt, 6},
                  {FbcDiagId::ExpectedRprnt, 7},
                  {FbcDiagId::ExpectedEq, 10},
                  {FbcDiagId::ExpectedComma, 16},
                  {FbcDiagId::ExpectedRBracket, 66},
                  {FbcDiagId::ExpectedGt, 309},
                  {FbcDiagId::ExpectedConst, 11},
                  {FbcDiagId::ExpectedRestype, 68},
                  {FbcDiagId::TooManyExpressions, 67},
                  {FbcDiagId::ExpectedSub, 289},
                  {FbcDiagId::ExpectedDynamicArray, 54},
                  {FbcDiagId::CantInitDynamicArrays, 75},
                  {FbcDiagId::CantInitDynamicStrings, 87},
                  {FbcDiagId::DynamicArrayWithEllipsis, 76},
                  {FbcDiagId::ArrayFieldWithEllipsis, 77},
                  {FbcDiagId::DynamicExternCantHaveBounds, 136},
                  {FbcDiagId::DynamicArrayInUnion, 162},
                  {FbcDiagId::RedimCtorMustBeCdecl, 171},
                  {FbcDiagId::RedimDtorMustBeCdecl, 172},
                  {FbcDiagId::RedimCtorMustHaveOneParam, 173},
                  {FbcDiagId::EraseCtorMustBeCdecl, 174},
                  {FbcDiagId::EraseDtorMustBeCdecl, 175},
                  {FbcDiagId::EraseCtorMustHaveOneParam, 176},
                  {FbcDiagId::PrivOrPubAttribNotAllowed, 216},
                  {FbcDiagId::ExpectedFileNumberExpression, 327}}),
     makeNumbers({{FbcDiagId::NoClosingQuote, 12},
                  {FbcDiagId::SuffixIgnored, 44},
                  {FbcDiagId::ArgCntMismatch, 43},
                  {FbcDiagId::NextVarMeaningless, 26},
                  {FbcDiagId::BranchCrossingLocalVar, 14},
                  {FbcDiagId::IfFoundAfterElse, 32},
                  {FbcDiagId::NoExplicitParamMode, 15},
                  {FbcDiagId::CmdlineIgnored, 46},
                  {FbcDiagId::ConvOverflow, 25},
                  {FbcDiagId::ShiftExceedsBitsInDataType, 33},
                  {FbcDiagId::ForEndTooBig, 45}})},
}};

FbcNumberRange const &rangeFor(std::optional<FbcVersion> const &version) {
  if (!version) {
    return kNumberRanges.back();
  }
  // The iterator std::upper_bound returns here is a pointer in some
  // standard libraries and an iterator class in MSVC's, so the
  // declaration stays plain auto; see TIDY.md.
  // NOLINTNEXTLINE(readability-qualified-auto)
  auto const it =
      std::upper_bound(kNumberRanges.begin(), kNumberRanges.end(), *version,
                       [](FbcVersion const &v, FbcNumberRange const &range) {
                         return v < range.first;
                       });
  if (it == kNumberRanges.begin()) {
    return kNumberRanges.front();
  }
  return *(it - 1);
}

} // namespace

int fbcNumber(FbcDiagId id, FbcMessageKind kind,
              std::optional<FbcVersion> const &version) {
  if (id == FbcDiagId::None) {
    return 0;
  }
  std::size_t const index = static_cast<std::size_t>(id);
  if (index >= kDiagCount) {
    return 0;
  }
  FbcNumberRange const &range = rangeFor(version);
  return kind == FbcMessageKind::Error ? range.errors[index]
                                       : range.warnings[index];
}

FbcCatalogEntry const *fbcMessage(FbcMessageKind kind, int number) {
  int const count = countFor(kind);
  if (number < 1 || number > count) {
    return nullptr;
  }
  return &tableFor(kind)[number - 1];
}

int fbcMessageCount(FbcMessageKind kind) { return countFor(kind); }

std::string fbcCode(FbcMessageKind kind, int number) {
  if (fbcMessage(kind, number) == nullptr) {
    return {};
  }
  return std::string("fbc ") +
         (kind == FbcMessageKind::Error ? "error: " : "warning: ") +
         std::to_string(number);
}

std::string_view fbcMessageDocsUrl() {
  return "https://www.freebasic.net/wiki/CompilerErrMsg";
}

} // namespace fblang
