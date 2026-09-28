/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "resolve.h"

namespace fblang {

// Code lens (M13): the annotation an editor draws above a declaration line —
// here, how many places reference it. LSP-agnostic and in byte offsets like
// the rest of the language layer; the session turns one `CodeLens` into an
// `lsCodeLens` and answers the click.
//
// A lens cannot carry an edit: the protocol's CodeLens has a range, an optional
// command, and an optional data blob, and no field for a WorkspaceEdit. So the
// only thing a lens can do when clicked is send a command id back to the
// server, which is why the count is not baked in at scan time but answered on
// request — and why the command has to be able to name the declaration the
// lens was drawn for.

// One declaration that carries a lens. Pointer-free on purpose: an anchor
// travels from the parse tree to the wire without pinning anything, and the
// counter below is handed it rather than a `Symbol const *` into a snapshot.
struct LensAnchor {
  SourceRange selection; // the name token: always one line, and the position
                         // the click carries back to identify the declaration
  std::string name;      // display name, original case
  SymbolKind kind = SymbolKind::Sub;
};

// How many references a lens reports for one anchor. This is the module's only
// seam on workspace state, and the reason a count cannot drift from the list a
// click produces: the session answers both from one walk, and this is how the
// walk reaches the module. A declaration that cannot be counted (no index, a
// name that resolved nowhere) reports zero.
using ReferenceCounter = std::function<std::size_t(LensAnchor const &)>;

struct CodeLens {
  LensAnchor anchor;
  std::string title; // localized "N references"
  std::size_t references = 0;
};

// One lens per declaration in `doc`, in source order (the protocol lets a
// client render the array in any order, and source order is the one a reader
// expects).
//
// The anchors are the procedure-like kinds and the type-ish roots — the
// declarations a reader names. Variables, parameters, and labels are not
// anchors: a lens per local would bury the file. Nesting is flattened, so a
// member procedure inside a TYPE gets its own lens; that is the declaration
// someone looking at a `.` access wants counted.
//
// The count is the number of reference sites *excluding* the declaration's own
// name token — what `textDocument/references` reports with `includeDeclaration`
// false. A lens carries no ReferenceContext, so it cannot ask for the other
// convention; it fixes this one, and the command answers the same set so the
// number on screen is the number a click lists.
std::vector<CodeLens> codeLenses(AnalyzedDoc const &doc,
                                 ReferenceCounter const &count);

} // namespace fblang
