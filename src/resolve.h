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
// module level. A declaration is a Dim/Const/Parameter symbol whose key (the
// case-insensitive lowercased name including any suffix char) matches the
// usage token.

struct Token;

// One document analyzed once: parse tree, full token stream (borrows the
// `source` buffer passed to analyze — must not outlive it), and raw include
// directives in source order. `parse.roots` carries the M5 occurrence
// projection: every Symbol's `occurrences` are filled and file-root Symbols
// are tagged `moduleScope=true`.
struct AnalyzedDoc {
    ParseResult parse;
    std::vector<Token> tokens;
    std::vector<IncludeDirective> includes;

    // A `#pragma once` line was seen somewhere in the source. Recorded as
    // IndexedFile metadata so the index can acknowledge the header's own
    // once-guard; guard-aware duplicate processing is still a documented
    // divergence (FreeBASIC.md §12.6), so this is metadata only.
    bool pragmaOnce = false;
};

// Single shared analysis: one lex, one parse, one occurrence sweep, include
// extraction. The index scan, the open-buffer upsert, and request-side
// resolution all build on it so no layer can diverge on a document's contents.
AnalyzedDoc analyze(std::string_view source);

// Resolution over an analyzed document (no re-lexing, no re-parsing).
Symbol const* resolveAt(AnalyzedDoc const& doc, std::uint32_t off);
// The decl's own precomputed usage sites (empty unless `doc` came from
// analyze()). `decl` must point into `doc.parse`'s symbol tree.
std::vector<Occurrence> occurrencesOf(AnalyzedDoc const& doc, Symbol const& decl);
// Pointers to every named declaration visible at `off`, innermost scope first.
std::vector<Symbol const*> visibleSymbols(AnalyzedDoc const& doc, std::uint32_t off);

// Innermost declaration-block scope containing `off`, or nullptr for module
// level. Dim/Const/Parameter nodes are not scopes and are walked through.
Symbol const* innermostScope(ParseResult const& parse, std::uint32_t off);

// The declaration a usage at `off` resolves to, or nullptr when the offset is
// not an identifier token or the name is unknown in every enclosing scope.
//
// Legacy ParseResult-based forms (each lexes the source once), kept so
// existing ParseResult-only callers stay green; prefer the AnalyzedDoc
// overloads in new code.
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