/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string>
#include <string_view>

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

// The LSP `Diagnostic.code` for a catalog message: `fbc error: 42` /
// `fbc warning: 5`. Returns an empty string when the message is not in the
// catalog.
std::string fbcCode(FbcMessageKind kind, int number);

// The wiki page documenting the compiler's diagnostics, for the LSP
// `codeDescription.href`.
std::string_view fbcMessageDocsUrl();

} // namespace fblang
