/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

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

// Deepest symbol-tree node containing `off` — any kind, inclusive of a node's
// end so a cursor on the closer token still lands inside — or nullptr for an
// offset no construct covers (module level). The starting point for asking a
// narrower question: innermostScope walks out of it to a scope kind, and
// procedureAt (call_hierarchy.h) walks out to the procedure around it.
Symbol const *innermostNode(ParseResult const &parse, std::uint32_t off);

// The §12.2 storage-gate predicate (FreeBASIC.md §8): whether `siteScope`
// (the innermost scope of the site, or nullptr) sits inside a procedure body.
// Module-level plain `Dim`/`Common` (no `Shared`) are invisible only then —
// fbc keeps error 42 for a procedure write. SCOPE/IF/FOR/... control blocks
// at module level inherit module scope, so the gate must key on a procedure
// boundary, not on "inside any block".
bool insideProcedureBody(ParseResult const &parse, Symbol const *siteScope);

// Parent of `node` in the parse symbol tree (the declaration block or module
// root owning it), or nullptr for a file root. Enum members are Const
// children of their Enum root — use this to tell an enum member from a
// statement-level Const (semantic token classification).
Symbol const *parentOf(ParseResult const &parse, Symbol const *node);
// Overload over a bare root list: cross-file resolution may land on a decl
// owned by an IndexedFile's roots (workspace snapshot, no ParseResult).
Symbol const *parentOf(std::vector<Symbol> const &roots, Symbol const *node);

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

// A declaration's cross-snapshot identity: its owning file plus the selection
// range of its name token. Unique per declaration — a (path, selection) pair
// pins one Symbol across index snapshots and fresh parses of the same file, so
// it is how two independently parsed copies of one file are compared (and how
// a call-hierarchy item is recognized when the client echoes it back).
struct DeclIdentity {
  std::string path;
  std::uint32_t beg = 0;
  std::uint32_t end = 0;

  bool operator==(DeclIdentity const &other) const {
    return path == other.path && beg == other.beg && end == other.end;
  }
};

// `d.decl`'s identity; `fallbackPath` is the owning file when `d.file` is null
// (the declaration lives in the caller's own document).
DeclIdentity identityOf(CrossDecl const &d, std::string const &fallbackPath);

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

// --- Member access (M9) ---

// The declared type name of `decl`, recovered from its signature line (the
// word after the first `as`, skipping `const`/`byref`-style modifiers), or
// empty when the declaration carries no explicit type (`dim v = expr`).
std::string declaredTypeName(Symbol const &decl);

// The member of a Type/Union declaration whose key equals `memberKey`
// (lowercased name including suffix char), or nullptr.
Symbol const *findMember(Symbol const &typeDecl, std::string const &memberKey);

// Cross-file lookup of a Type/Union declaration by key: the requesting doc's
// roots first (tier 1), then each file of the index's transitive include
// closure of `normalizedPath` in textual pre-order. `index == nullptr`
// restricts the search to the requesting doc.
CrossDecl findTypeDecl(AnalyzedDoc const &doc,
                       std::string const &normalizedPath,
                       std::string const &typeKey, WorkspaceIndex const *index);

// Result of resolving a `.`/`->` member access under the cursor.
struct MemberAccess {
  // The indexed entry `member` points into, pinned for as long as this result
  // lives — same contract as CrossDecl::file. The member is a child of the
  // owner type, which lives in another file's entry, and a background scan
  // replaces entries wholesale: a bare Symbol pointer into one dangles the
  // moment that scan upserts the same path. `file == nullptr` when `member`
  // points into the request-local `AnalyzedDoc` (an in-file `with`-implicit
  // base), which the caller already pins.
  std::shared_ptr<IndexedFile const> file;
  Symbol const *member = nullptr; // the field declaration found, or nullptr
  std::string baseName;      // root variable (`with` target or lhs identifier),
                             // for "member of `map`" display; empty for chained
                             // access whose owner is a plain type
  std::string ownerTypeName; // display name of the type that owns `member`
  bool direct = false; // `member` hangs directly off `baseName`'s declared type
  // The member's owner is an Enum root (`EnumName.member`). Display reads
  // "Enum member of `X`." instead of "Member of `X` (`X`)."
  bool enumMember = false;
  // The cursor is on a `.`/`->` member access whose chain root variable
  // resolved, even when the declared type — and so the member itself — could
  // not be pinned down (unknown/unindexed type). Lets the hover name the
  // owning variable instead of falling through to a colliding identifier.
  bool memberAccess = false;
};

// Resolves `expr.member` at `off`, where `off` may sit anywhere on the member
// identifier. Handles the implicit `with`-target base (`with map`: a leading
// `.member`), `var.member`, chained `a.b.c`, and indexed member access
// (`.arr(i).field` — the declared element type drives the next lookup).
// `index == nullptr` restricts base-variable resolution to the requesting doc
// (the with-scope scan is always in-file). `member` is null when the chain's
// declared type or a link cannot be resolved, but `memberAccess` (and the
// resolved `baseName`) still report the access for a soft-fallback hover.
MemberAccess resolveMemberAccess(AnalyzedDoc const &doc,
                                 std::string const &normalizedPath,
                                 std::uint32_t off,
                                 WorkspaceIndex const *index);

// Result of the member-completion scan at `off` — the same `.`/`->` chain
// shape as resolveMemberAccess, with the cursor either on a partially typed
// member name or right after the chain's final operator (`position.`), plus
// the `with`-implicit leading dot and qualified `EnumName.member` access.
struct MemberCompletion {
  // The Type/Union root owning `members` (or an Enum root for qualified
  // `EnumName.` access); empty when the chain base did not resolve.
  CrossDecl owner;
  // Every accessible member of the owner, access-filtered (FreeBASIC.md §7
  // Access sections):
  // Public members always; Private/Protected only when `off` sits inside a
  // member procedure implementation of the owner type (fbc error 202 on any
  // outside path). Enumerators are always listed.
  std::vector<Symbol const *> members;
  std::string baseName;      // root variable (`with` target or lhs identifier),
                             // for display; empty for a plain-type owner
  std::string ownerTypeName; // display name of the type that owns the members
  bool enumMember = false;   // qualified `EnumName.` access (no access gate)
  // The chain base resolved, even when the declared type — and so `members` —
  // could not be pinned down (unknown/unindexed owner). Mirrors
  // MemberAccess::memberAccess: a `.`/`->` chain must never fall back to
  // keyword/global/intrinsic completion.
  bool memberAccess = false;
};

MemberCompletion resolveMemberCompletion(AnalyzedDoc const &doc,
                                         std::string const &normalizedPath,
                                         std::uint32_t off,
                                         WorkspaceIndex const *index);

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

// The files that can name a declaration living in `declPath`: the requesting
// file, its forward include closure (textual pre-order), `declPath` itself (a
// tier-3 byKey hit can land outside the closure), and reverse reachability —
// every indexed file whose own closure reaches `declPath`, so a header
// declaration covers all its includers. Deduped, in that order. The candidate
// set behind occurrencesAcross, shared with the call-hierarchy incoming scan so
// one definition of "who can reference this" answers both.
std::vector<std::string> referencingFiles(std::string const &normalizedPath,
                                          std::string const &declPath,
                                          WorkspaceIndex const *index);

// --- Type graph and member implementation (M15) ---
//
// FreeBASIC's type graph is a single parent per type: `type d extends b` on
// the opener line is the only inheritance form there is (no `Type : base`, and
// no `interface` keyword at all — FreeBASIC.md §7), so a hierarchy walk is a
// chain, never a reconciliation. A member procedure is `declare`d inside the
// type and defined at module level as `Type.name`, and a derived type may not
// re-implement an inherited member, so the declared<->implemented edge is a
// function and never a fan-out. Both facts are recorded by the parser on
// `Symbol::extendsKey` / `Symbol::ownerKey`; everything here reads them.

// One type as a type-hierarchy node. Copied out of the symbol tree by value —
// the CallItem contract — so an item stays readable after the snapshot it was
// built from has been replaced, and a wire item is a plain value anyway. The
// declaring file travels with it because that is what a node is on the wire:
// `uri`, `range`, and `selectionRange` all describe one file. Carrying the
// file by value also means this type hands out no pointer into a snapshot, so
// it needs no pin of its own.
struct TypeItem {
  std::string file;      // normalized path of the file declaring the type
  std::string name;      // display name, original case
  std::string key;       // lookup key, lowercased — the seed of every walk
  std::string detail;    // the opener line, e.g. "type derived extends base"
  SourceRange range;     // the whole construct (TYPE ... END TYPE)
  SourceRange selection; // the name token
  SymbolKind kind = SymbolKind::Type;
};

// The type that *is* the type of the symbol the usage at `off` resolves to:
// the owner of a member the cursor names, the `as <type>` of a resolved
// variable/parameter/constant, the type a dot-qualified member implementation
// qualifies, or the type/union root itself when the cursor is already on one.
// Nullopt when the symbol carries no type (a Sub, a label, a constant without
// `as`) or when the type cannot be resolved. `index == nullptr` resolves
// in-file only.
std::optional<TypeItem> typeOf(AnalyzedDoc const &doc,
                               std::string const &normalizedPath,
                               std::uint32_t off, WorkspaceIndex const *index);

// The member `memberKey` visible on an instance of `typeKey`: the type's own
// members first, then each type it extends, nearest first (FreeBASIC.md §7 —
// an inherited field and an inherited member call both resolve through any
// number of levels). `findMember` is the own-members-only half and stays that
// way, because this walk needs the cross-file lookup and `findMember` is a
// pure in-tree one.
//
// The walk stops at the first base that cannot be resolved — fbc has no
// forward base references, so a broken chain is a defect, not a form to
// answer for — and never revisits a key, so a cycle a lenient parse let
// through terminates. `index == nullptr` restricts the walk to the requesting
// document.
CrossDecl findVisibleMember(AnalyzedDoc const &doc,
                            std::string const &normalizedPath,
                            std::string const &typeKey,
                            std::string const &memberKey,
                            WorkspaceIndex const *index);

// The module-level implementation of a member procedure declared inside a
// type: `sub t.go()` for `declare sub go()` in `type t`. FreeBASIC.md §7: a
// member procedure cannot be *defined* inside a type body (fbc error 17), so
// the implementation is always a module-level root qualified by the type name,
// and it may live in the includer .bas or a sibling header of the same
// include closure — which is why this is a closure search, like findTypeDecl.
// `index == nullptr` restricts the search to the requesting document. Empty
// when the member is not a procedure declaration, or has no implementation.
CrossDecl memberImplementation(AnalyzedDoc const &doc,
                               std::string const &normalizedPath,
                               std::string const &typeKey,
                               std::string const &memberKey,
                               WorkspaceIndex const *index);

// The other end of that edge: the `declare sub go()` inside `type t` for a
// module-level `sub t.go()`. Separate from memberImplementation rather than
// one function with a direction flag, because the two are two facts. A derived
// type cannot re-implement an inherited member (fbc error 158), so the edge is
// a function and this returns at most one declaration. Same closure and
// `index == nullptr` contract.
CrossDecl memberDeclaration(AnalyzedDoc const &doc,
                            std::string const &normalizedPath,
                            std::string const &typeKey,
                            std::string const &memberKey,
                            WorkspaceIndex const *index);

// The far end of that edge from a cursor position, in whichever direction the
// symbol under the cursor allows: a `declare sub go()` inside a type reaches
// the module-level `sub t.go()` that implements it, and a `sub t.go()` reaches
// the `declare`. One function rather than two entry points because from a
// position there is exactly one of the two facts available, and the position is
// what says which — and because the dispatch needs the symbol *tree* the
// declaration lives in, which a caller holding only a cross-file `CrossDecl`
// does not have. Empty when the cursor is not on a member procedure on either
// side of the edge: a field has no implementation, a plain procedure belongs to
// no type, and a `sub` with no dot is a module-level sub (constructor and
// destructor deliberately carry no owner — FreeBASIC.md §12.16). Same closure
// and `index == nullptr` contract.
CrossDecl implementationTarget(AnalyzedDoc const &doc,
                               std::string const &normalizedPath,
                               std::uint32_t off, WorkspaceIndex const *index);

// The Type/Union declaration whose name token is exactly `id` — the item a
// client echoes back from `textDocument/typeHierarchy` is a name-token range in
// one file, and a (path, selection) pair is the only thing that identifies a
// declaration across parses. Answers empty when the range matches no
// declaration or the name token is not a type, rather than falling back to "the
// type nearest the cursor": a client that trimmed the range, or sent an item
// from before an edit, should get nothing rather than a wrong type. The
// declaration is taken from `doc` alone, so the caller must hand the analysis
// of the file `id` names — an item can point at a file this request never
// opened.
CrossDecl typeAtIdentity(AnalyzedDoc const &doc, DeclIdentity const &id);

// The types `typeKey` extends, nearest first, following `Symbol::extendsKey`.
// Closure-scoped, because it starts from a declaration in the requesting
// document and walks up through that document's include closure — the
// opposite half from `subtypes`, which starts from a type and has to see the
// whole workspace. No key is visited twice, so a cycle terminates.
// `index == nullptr` restricts the walk to the requesting document.
std::vector<TypeItem> supertypes(AnalyzedDoc const &doc,
                                 std::string const &normalizedPath,
                                 std::string const &typeKey,
                                 WorkspaceIndex const *index);

// The types below `typeKey`, breadth-first: direct subtypes first, then their
// subtypes, which is the whole subtree a `typeHierarchy/subtypes` reply is.
// The workspace-wide half of the hierarchy, so a subtype declared in a file
// this request never loads is still found. No key is listed twice, so a cycle a
// lenient parse let through terminates. Empty when `index == nullptr` (no
// workspace to ask).
std::vector<TypeItem> subtypes(std::string const &typeKey,
                               WorkspaceIndex const *index);

} // namespace fblang
