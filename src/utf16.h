/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <string_view>

#include "LibLsp/lsp/lsRange.h"

namespace fblang {

// Convert a zero-based byte offset into `text` to an LSP position whose
// `character` is measured in UTF-16 code units (a non-BMP code point occupies
// two units). `text` must be the same buffer the language layer parsed, so
// line numbers agree with the parser's byte offsets. Offsets past the end of
// the buffer clamp to the buffer end.
lsPosition utf16Position(std::string_view text, std::uint32_t byteOffset);

// Convert an exclusive byte range [beginByte, endByte) to an LSP range.
lsRange utf16Range(std::string_view text, std::uint32_t beginByte,
                   std::uint32_t endByte);

// Convert an LSP position (UTF-16 code units, zero-based) back to a byte
// offset into `text`. A character offset past the end of its line clamps to
// the end of that line, matching the LSP convention; a line index past the
// last line clamps to the buffer end.
std::uint32_t byteOffsetForUtf16Position(std::string_view text, lsPosition pos);

} // namespace fblang
