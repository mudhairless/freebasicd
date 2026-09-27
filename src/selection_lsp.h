/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <string_view>
#include <vector>

// LspCpp's selectionRange.h uses `lsRange` without including it, so the
// dependency is named here rather than left to the order of includes in the
// .cpp — the header has to stand on its own.
#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/textDocument/selectionRange.h"

#include "symbols.h"

namespace fblang {

// LSP `textDocument/selectionRange` (M13): one nested chain per requested
// position, built from the byte-offset chains `selectionChain` returns
// (innermost level first, each level strictly containing the one below it).
//
// The result holds one entry per chain *in order*, because the client maps
// `result[i]` to `positions[i]`; a chain that came back empty (an empty
// document) still owes an entry, as a degenerate range at the start.
//
// `content` is the buffer the chains' byte offsets are relative to — the same
// one the request document's analysis was built from.
std::vector<SelectionRange>
selectionRanges(std::vector<std::vector<SourceRange>> const &chains,
                std::string_view content);

} // namespace fblang
