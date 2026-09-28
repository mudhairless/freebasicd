/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "resolve.h"

namespace fblang {

// Call hierarchy (M13) over an analyzed document: `prepareCallHierarchy`
// answers "which procedure is this?", and the two follow-ups answer "what does
// it call?" and "who calls it?". Byte offsets and LSP-agnostic, like the rest
// of the language layer; the session converts and pins.
//
// A node is a procedure-like declaration. Everything here works in terms of
// *declarations*, not symbols-with-names: FreeBASIC is case-insensitive and
// `foo`/`foo$` are one symbol in `fb` mode, so a key match is never enough to
// say a site calls a given procedure. Each call site is therefore resolved by
// the caller through the `CalleeResolver` seam and matched by `DeclIdentity`
// (owning file + name-token range), which is unique per declaration even across
// independently parsed copies of the same file.

// A procedure-like declaration as a call-hierarchy node. Copied out of the
// symbol tree by value, so an item stays readable after the snapshot it was
// built from has been replaced — and a wire item is a plain value anyway. The
// declaring file travels with it because that is what a node is on the wire:
// `uri`, `range`, and `selectionRange` all describe one file.
struct CallItem {
  std::string file;      // normalized path of the file declaring it
  std::string name;      // display name, original case
  std::string detail;    // the declaration's signature, for the client's detail
  SourceRange range;     // the whole construct (SUB ... END SUB)
  SourceRange selection; // the name token
  SymbolKind kind = SymbolKind::Sub;
};

CallItem callItemOf(Symbol const &decl, std::string const &file);

// The procedure-like declaration `off` belongs to: the innermost enclosing
// Sub/Function/Property/Constructor/Destructor/Operator, whether the cursor is
// on its name token or anywhere in its body (a cursor inside a nested `if`
// block still belongs to the procedure around it). Null at module level and
// outside every construct — a call-hierarchy node is a declaration, and
// FreeBASIC's top-level code has none to offer.
Symbol const *procedureAt(AnalyzedDoc const &doc, std::uint32_t off);

// A callee reference the token scan found, handed to the resolver. `shape` is
// *how* it was written, which decides both how to resolve it and whether it is
// a call at all — see `CallShape`.
struct CalleeRef {
  enum class Shape {
    Name,      // `name(...)` — a call written with a parameter list
    Member,    // `expr.name` / `expr->name` — a call through a receiver; the
               // parameter list is optional, so this shape is read the same
               // way (a property read is filtered out by callee kind)
    Statement, // a bare `name` in statement position, and the arguments (if
               // any) follow: fbc accepts `s` and `s 1` as calls in `fb` mode
  };

  std::uint32_t off = 0; // offset of the callee's identifier token
  SourceRange range;     // that token — the call site's range on the wire
  Shape shape = Shape::Name;
};

// Resolves a callee reference to the declaration it names, or an empty
// CrossDecl when the name resolves to nothing. This is the module's only seam
// on workspace state, so the scans stay testable in one file and the session
// keeps sole ownership of which index a walk consults. `Name` and `Statement`
// are the same lookup; `Member` needs the member resolver.
using CalleeResolver =
    std::function<CrossDecl(AnalyzedDoc const &doc, CalleeRef const &ref)>;

// One callee of `caller`, with every site in that body naming it. Sites are
// byte ranges in the caller's own file — which is what the protocol asks for
// (`fromRanges` is relative to the caller).
struct OutgoingCall {
  CallItem to;
  std::vector<SourceRange> sites;
};

// The calls made from inside `caller`'s body, one entry per callee in source
// order, callees called more than once merged. `docPath` is the normalized path
// of `doc` (a resolved declaration with `file == nullptr` lives in that file).
std::vector<OutgoingCall> outgoingCalls(AnalyzedDoc const &doc,
                                        std::string const &docPath,
                                        Symbol const *caller,
                                        CalleeResolver const &resolve);

// One caller of `target` inside one scanned file. `caller` is the calling
// procedure — always a node of the scanned document (the scan finds the
// enclosing procedure by walking that document's own tree), so the caller of
// this function must keep the scanned `doc` alive for as long as the result is
// read. `sites` are byte ranges in that document.
struct IncomingCall {
  Symbol const *caller = nullptr;
  std::vector<SourceRange> sites;
};

// The calls to `target` in one file, one entry per calling procedure. The
// caller must sweep the files `referencingFiles` names (resolve.h); this is the
// per-file half. A call at module level is dropped: it has no enclosing
// procedure to be the `from` node, and a node the client cannot navigate to is
// not an answer.
std::vector<IncomingCall> incomingCallsIn(AnalyzedDoc const &doc,
                                          std::string const &docPath,
                                          DeclIdentity const &target,
                                          CalleeResolver const &resolve);

} // namespace fblang
