#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "symbols.h"

namespace fblang {

// Identifier resolution over a parsed document. Byte-offset based and
// LSP-agnostic; `src` must be the same buffer the ParseResult was built from.
//
// FreeBASIC scoping: the innermost declaration block (procedure, type, enum,
// namespace, SCOPE) is searched first, then successively outer blocks up to
// module level. A declaration is a Dim/Const/wild-card Symbol whose key (the
// case-insensitive lowercased name including any suffix char) matches the
// usage token.

// Innermost declaration-block scope containing `off`, or nullptr for module
// level. Dim/Const/Parameter nodes are not scopes and are walked through.
Symbol const* innermostScope(ParseResult const& parse, std::uint32_t off);

// The declaration a usage at `off` resolves to, or nullptr when the offset is
// not an identifier token or the name is unknown in every enclosing scope.
Symbol const* resolveAt(ParseResult const& parse, std::string_view src, std::uint32_t off);

// Reference sites of `decl` (usages that resolve to it), sorted by byte
// offset, excluding `decl`'s own name token. `decl` must point into `parse`'s
// symbol tree (as returned by resolveAt / parse.roots).
std::vector<SourceRange> occurrencesOf(ParseResult const& parse, std::string_view src,
                                       Symbol const& decl);

// Pointers to every named declaration visible at `off`, innermost scope first.
// A name listed earlier shadows any later entry with the same key (module
// level is last). Used to build completion candidates.
std::vector<Symbol const*> visibleSymbols(ParseResult const& parse, std::uint32_t off);

}  // namespace fblang