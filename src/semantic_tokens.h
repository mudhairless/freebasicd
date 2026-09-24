/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "resolve.h"

namespace fblang {

// LSP-agnostic semantic-token classification over an analyzed document.
//
// Works in byte offsets + explicit UTF-16 columns so the lang lib never links
// LspCpp: the module re-walks the content with its own code-point walker
// (mirroring utf16.cpp) and hands the session absolute (non-delta) entries
// the session encodes and layers a resultId on.

// One token, positions in UTF-16 code units (relative to their line start).
struct SemanticTokenEntry {
  std::uint32_t line = 0;      // zero-based
  std::uint32_t startChar = 0; // UTF-16 units since the line start
  std::uint32_t length = 0;    // UTF-16 units; a token never spans a line
  std::uint32_t type = 0;      // index into semanticTokenTypes()
  std::uint32_t modifiers = 0; // bit flags into semanticTokenModifiers()
};

// 3.17 legend names in definitive order — spellings must match the spec
// exactly. Returned by value to keep binary exports simple.
std::vector<std::string> semanticTokenTypes();
std::vector<std::string> semanticTokenModifiers();

// Classify every token of `doc`. Skips Newline/Eof and pure punctuation
// (symbols outside symbolOperators()). `content` must be the buffer `doc` was
// analyzed from.
std::vector<SemanticTokenEntry> semanticTokens(AnalyzedDoc const &doc,
                                               std::string_view content);

// Flat 5-int-per-token encoding with relative deltas ("relative" format).
std::vector<std::int32_t>
encodeTokenData(std::vector<SemanticTokenEntry> const &tokens);

// Viewport filter for the /range provider: entries whose line lies in
// [begLine, endLine], inclusive.
std::vector<SemanticTokenEntry>
filterTokens(std::vector<SemanticTokenEntry> const &tokens,
             std::uint32_t begLine, std::uint32_t endLine);

} // namespace fblang