/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "fbc_version.h"

// The fbc diagnostic catalog, as data.
//
// `src/fbc_diagnostics.inc` is generated from `tools/fbc_catalog.tsv` (itself
// extracted from `error.bas` at the pinned fbc tag); this header is the only
// way the rest of the server reads it. A diagnostic we map to fbc carries the
// compiler's own number and text, so a reader who knows `error 42` learns
// something from our output, and the LSP `Diagnostic.code` is the string
// `fbc error: 42`. The wiki page covering the catalog is the same for every
// message, so the session attaches it as `codeDescription.href`.
namespace fblang {

enum class FbcMessageKind { Error, Warning };

// The meaning of an fbc message, independent of the number fbc gives it. fbc
// numbers errors and warnings in separate spaces and has renumbered them
// occasionally, so a finding stores one of these identities and the number is
// resolved against the table for the compiler version probed at runtime.
// `None` means fbc has no message for this finding.
enum class FbcDiagId {
  None = 0,
  ExpectedEndSub,
  ExpectedEndFunction,
  ExpectedEndCtor,
  ExpectedEndDtor,
  ExpectedEndOperator,
  ExpectedEndProperty,
  ExpectedEndType,
  ExpectedEndEnum,
  ExpectedEndNamespace,
  ExpectedEndScope,
  ExpectedEndExtern,
  ExpectedEndWith,
  ExpectedEndSelect,
  ExpectedEndIf,
  ExpectedNext,
  ExpectedWend,
  ExpectedLoop,
  ExpectedEndAsm,
  ExpectedPpEndIf,
  ExpectedMacro,
  LoopWithoutDo,
  NextWithoutFor,
  WendWithoutWhile,
  EndWithWithoutWith,
  EndIfWithoutIf,
  EndSelectWithoutSelect,
  EndSubWithoutSub,
  EndScopeWithoutScope,
  EndNamespaceWithoutNamespace,
  EndExternWithoutExtern,
  IllegalEnd,
  NoClosingQuote,
  SuffixIgnored,
  KeywordFieldsNotAllowedInClasses,
  ExpectedIdentifier,
  ElseWithoutIf,
  ElseIfWithoutIf,
  CaseWithoutSelect,
  SyntaxError,
  ExpectedEol,
  ArgCntMismatch,
  InvalidPriority,
  DupDefinition,
  IllegalOutsideComp,
  ExpectedExpression,
  VariableNotDeclared,
  UndefinedSymbol,
  FileNotFound,
  IllegalInsideSelect,
  ExpectedCase,
  IllegalOutsideSelect,
  ExpectedSelect,
  NoEnclosedSelect,
  InvalidCaseRange,
  ForNextVariableMismatch,
  NextVarMeaningless,
  InvalidExitStmt,
  InvalidContinueStmt,
  IllegalInsideSub,
  IllegalInsideScope,
  BranchCrossingDynDataDef,
  BranchCrossingLocalVar,
  ExpectedLprnt,
  ExpectedRprnt,
  ExpectedEq,
  ExpectedComma,
  ExpectedRBracket,
  ExpectedGt,
  ExpectedConst,
  ExpectedRestype,
  TooManyExpressions,
  ExpectedSub,
  ExpectedDynamicArray,
  CantInitDynamicArrays,
  CantInitDynamicStrings,
  DynamicArrayWithEllipsis,
  ArrayFieldWithEllipsis,
  DynamicExternCantHaveBounds,
  DynamicArrayInUnion,
  RedimCtorMustBeCdecl,
  RedimDtorMustBeCdecl,
  RedimCtorMustHaveOneParam,
  EraseCtorMustBeCdecl,
  EraseDtorMustBeCdecl,
  EraseCtorMustHaveOneParam,
  PrivOrPubAttribNotAllowed,
  ExpectedFileNumberExpression,
  Count
};

// The catalog number `id` carries in `kind`'s space under `version`. A nullopt
// version resolves through the newest table (the posture the delivery gates
// already take: unknown version filters nothing); a probed version below
// every known range takes the oldest table, and one above takes the newest
// applicable. `None` resolves to 0.
int fbcNumber(FbcDiagId id, FbcMessageKind kind,
              std::optional<FbcVersion> const &version);

// One row of the imported catalog. `text` is fbc's own wording; `level` is a
// warning's `-w` gate level (0 means off by default) and 0 for errors.
struct FbcCatalogEntry {
  FbcMessageKind kind;
  int number;
  std::string_view name;
  std::string_view text;
  int level;
};

// The catalog row for `number`, or nullptr when the kind/number pair is not in
// the catalog. Numbers are 1-based and contiguous per kind.
FbcCatalogEntry const *fbcMessage(FbcMessageKind kind, int number);

// How many messages of `kind` the catalog holds.
int fbcMessageCount(FbcMessageKind kind);

// The fbc half of an LSP `Diagnostic.code` for a catalog message: `fbc error:
// 42` / `fbc warning: 5`. The session composes the full code as our routing key
// followed by this (`include-not-found, fbc error: 23`), so our identifier is
// always present; the number alone is what tests that pin the mapping match.
// Returns an empty string when the message is not in the catalog.
std::string fbcCode(FbcMessageKind kind, int number);

// The wiki page documenting the compiler's diagnostics, for the LSP
// `codeDescription.href`.
std::string_view fbcMessageDocsUrl();

} // namespace fblang
