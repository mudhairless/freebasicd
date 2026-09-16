#pragma once

#include <string_view>

#include "symbols.h"

namespace fblang {

// Parse `source` into document symbols, diagnostics, and block ranges.
// Everything is byte-offset and LSP-agnostic; the source must outlive the
// returned ParseResult (Symbol doc/name strings are copied, ranges point back).
ParseResult parseDocument(std::string_view source);

} // namespace fblang
