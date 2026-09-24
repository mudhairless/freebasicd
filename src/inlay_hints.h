/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "resolve.h"

namespace fblang {

// One inlay hint anchored at a byte offset. Labels are display text; the
// session converts the offset to a UTF-16 position. LSP-agnostic: works in
// byte offsets like the rest of the lang library.
struct InlayHintItem {
  std::uint32_t bytePos = 0; // absolute byte offset of the anchor
  std::string label;         // "END SUB", "NEXT", "#ENDIF", "As String", ...
};

// Two hint kinds over an analyzed document:
//  - expected-closer hints at block openers (`SUB` -> "END SUB", `#IF` ->
//    "#ENDIF"), anchored at the end of the opener's line;
//  - cosmetic inferred-type hints for suffix-typed `dim`/`redim`/... without an
//    `AS` clause (`dim x$` -> "As String").
std::vector<InlayHintItem> inlayHints(AnalyzedDoc const &doc,
                                      std::string_view content);

} // namespace fblang
