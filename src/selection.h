/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "resolve.h"
#include "symbols.h"

namespace fblang {

// Expand-selection ranges (M13, `textDocument/selectionRange`), LSP-agnostic
// and in byte offsets like the rest of the language layer; the session
// converts each range to a UTF-16 LSP range.
//
// The chain is what a user grows a selection through, innermost first:
//   1. the token the offset sits in (identifier, number, string, keyword, ...;
//      an offset just past a token still selects that token);
//   2. the statement segment — the `:`-separated piece of the logical line the
//      offset is on, the lexer having already merged `_` continuations, so a
//      continued statement is one segment and spans its lines;
//   3. every enclosing block from `parse.blockRanges`, innermost out (if /
//      for / while / select / with / asm / extern / sub / type / enum / ...);
//   4. the whole document.
//
// Every range strictly contains the one before it — the protocol requires
// `parent.range` to contain `this.range`, and an editor expanding through a
// duplicate or a shrunken level looks broken. Levels that fail that test (an
// unterminated block whose closer never arrived, a `:` with nothing after it)
// are dropped rather than repaired, so the chain is always well-formed.
//
// A candidate is kept only when it both strictly contains the level below it
// and adds text that is not blank at either end: a level that would grow the
// selection by a newline alone is noise, not growth. That one rule is what
// drops the file level around a trailing blank line and keeps it around an
// unterminated block. A `:` is never a level at all — as the innermost
// candidate it is not contained by the statement segment, so it would
// suppress the very segment it belongs to.
//
// `content` must be the buffer `doc` was analyzed from, and is used only for
// the whole-document level and the blank-end test; an offset outside the
// document yields an empty chain.
std::vector<SourceRange> selectionChain(AnalyzedDoc const &doc,
                                        std::string_view content,
                                        std::uint32_t off);

} // namespace fblang
