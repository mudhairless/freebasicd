/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "symbols.h"

namespace fblang {

// A folded numeric literal. The floating part is what the folder always
// carried. The optional exact part is present only when the literal is
// an integer in the range -2^63..2^64-1; a floating literal or one past
// that range leaves it empty. Judgments against 64-bit type limits read
// it, so a value such as 18446744073709551615 stays exact wherever the
// width of `long double` differs. The sign is separate from the
// magnitude so the full range fits in 64 bits without signed overflow.
struct FoldedConstant {
  struct ExactInteger {
    bool negative = false;
    std::uint64_t magnitude = 0;
  };
  long double numeric = 0;
  std::optional<ExactInteger> exact;
};

// Fold a numeric literal as written in a document (radix forms, an
// exponent, and a type suffix included). Empty when the text is not a
// numeric literal.
std::optional<FoldedConstant> foldNumericLiteral(std::string_view text);

// Parse `source` into document symbols, diagnostics, and block ranges.
// Everything is byte-offset and LSP-agnostic; the source must outlive the
// returned ParseResult (Symbol doc/name strings are copied, ranges point back).
ParseResult parseDocument(std::string_view source);

} // namespace fblang
