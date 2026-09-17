#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "index.h"
#include "lexer.h"
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
Symbol const *resolveAt(AnalyzedDoc const &doc, std::uint32_t off);
// Byte range of the identifier token under `off` in `doc` (empty when `off`
// is not on an identifier token). The cursor's own token, not the resolved
// declaration's name token — the two live in different files when a usage
// resolves cross-file, and prepareRename must report the requesting
// document's selection.
SourceRange tokenRangeAt(AnalyzedDoc const &doc, std::uint32_t off);
// The decl's own precomputed usage sites (empty unless `doc` came from
// analyze()). `decl` must point into `doc.parse`'s symbol tree.
std::vector<Occurrence> occurrencesOf(AnalyzedDoc const &doc,
                                      Symbol const &decl);
// Pointers to every named declaration visible at `off`, innermost scope first.
std::vector<Symbol const *> visibleSymbols(AnalyzedDoc const &doc,
                                           std::uint32_t off);

// Innermost declaration-block scope containing `off`, or nullptr for module
// level. Dim/Const/Parameter nodes are not scopes and are walked through.
Symbol const *innermostScope(ParseResult const &parse, std::uint32_t off);

// Parent of `node` in the parse symbol tree (the declaration block or module
// root owning it), or nullptr for a file root. Enum members are Const
// children of their Enum root — use this to tell an enum member from a
// statement-level Const (semantic token classification).
Symbol const *parentOf(ParseResult const &parse, Symbol const *node);

// Where a cross-file resolution landed. `file == nullptr` means `decl` points
// into the request-local `AnalyzedDoc` (tier 1); otherwise `file` pins the
// workspace snapshot that owns `decl` (tiers 2/3), kept alive by the caller.
struct CrossDecl {
  std::shared_ptr<IndexedFile const> file;
  Symbol const *decl = nullptr;

  bool operator==(CrossDecl const &other) const {
    return file.get() == other.file.get() && decl == other.decl;
  }
};

// Cross-file resolution over an analyzed document, three tiers (FreeBASIC.md
// §9/§12.2): (1) in-file scopes, shadowing wins; (2) module scope of each
// file in `index`'s transitive include closure of `normalizedPath`, textual
// pre-order, first key match honoring the storage gate; (3) a lenient
// `byKey` workspace fallback for names that resolve nowhere in the closure
// (still-unincluded headers — a documented divergence). Returns an empty
// CrossDecl when `off` is not an identifier token or no tier resolves.
// `normalizedPath` is the request document's normalized absolute path.
CrossDecl resolveAcross(AnalyzedDoc const &doc,
                        std::string const &normalizedPath, std::uint32_t off,
                        WorkspaceIndex const &index);

// --- Cross-file rename support (M8) ---

// A single rename site: the file and byte range to replace.
struct OccurrenceSite {
  std::string file;  // normalized absolute path
  SourceRange range; // byte range of the token to replace
};

// One immutable content+analysis unit served by the cross-file content seam.
// `analysis` borrows `content`'s bytes (Token::data == content.data() + beg,
// lexer.h), and both members live in the same heap object, so pinning the
// shared_ptr pins a coherent pair. Callers must never dismantle a
// DocumentContent while its analysis is in use.
struct DocumentContent {
  std::string content;
  AnalyzedDoc analysis;
};

// Content provider for occurrencesAcross: returns a content+analysis unit for
// a file (open buffer or disk), or nullptr when unavailable. The analysis
// must have been built from exactly that content (content-addressed callers
// guarantee this); ranges in the result are byte offsets into it.
using ContentProvider =
    std::function<std::shared_ptr<DocumentContent const>(std::string const &)>;

// All reference sites of the symbol the usage at `off` resolves to, across the
// workspace: the requesting file, its forward include closure, the resolving
// declaration's own file, and reverse reachability (every file whose closure
// includes the declaration's file). Each candidate token is re-resolved
// shadowing-aware, so a same-named local that shadows the declaration is
// untouched. Ranges are byte offsets into the exact content `content` serves.
// `normalizedPath` is the request document's normalized absolute path;
// `index == nullptr` restricts to the requesting file.
std::vector<OccurrenceSite> occurrencesAcross(AnalyzedDoc const &doc,
                                              std::string const &normalizedPath,
                                              std::uint32_t off,
                                              WorkspaceIndex const *index,
                                              ContentProvider const &content);

} // namespace fblang
