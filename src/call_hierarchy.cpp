/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "call_hierarchy.h"

#include "lexer.h"
#include "symbols.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace fblang {

namespace {

// The declarations that can be a call-hierarchy node: anything with a body a
// call can live in.
bool isProcedureLike(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Sub:
  case SymbolKind::Function:
  case SymbolKind::Property:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Operator:
    return true;
  default:
    return false;
  }
}

// What a *call* can name. A Property is excluded: reading one is not a call,
// and the token stream alone cannot tell a property read from a property write
// without carrying the assignment context. An Operator overload is excluded
// because it is reached through an operator (`a + b`), never as `name(...)`.
bool isCallableKind(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Sub:
  case SymbolKind::Function:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
    return true;
  default:
    return false;
  }
}

// fbc 1.10.2, `-lang fb`: `s` and `s 1` both call the Sub `s` (only `Call s`
// needs a non-fb dialect — error 146), while a Function name in statement
// position is a value reference, not a call. So a bare statement-head name is
// only a call edge when it names a Sub-like declaration.
bool isStatementCallableKind(SymbolKind kind) {
  return kind == SymbolKind::Sub || kind == SymbolKind::Constructor ||
         kind == SymbolKind::Destructor;
}

bool accepts(CalleeRef::Shape shape, SymbolKind kind) {
  return shape == CalleeRef::Shape::Statement ? isStatementCallableKind(kind)
                                              : isCallableKind(kind);
}

bool isDot(Token const &t) {
  return t.kind == TokenKind::Symbol && (t.text() == "." || t.text() == "->");
}

bool isLParen(Token const &t) {
  return t.kind == TokenKind::Symbol && t.text() == "(";
}

// Tokens[i] starts a statement when nothing on the line can precede it: the
// previous token is a newline, a `:` separator, or a keyword that ends one
// (`then`, `else`, `do`). Comments and `#`/`$` lines end before their newline,
// so a token after one still sees that newline.
bool isStatementHead(std::vector<Token> const &tokens, std::size_t i) {
  if (i == 0) {
    return true;
  }
  Token const &prev = tokens[i - 1];
  return prev.kind == TokenKind::Newline || prev.kind == TokenKind::Keyword ||
         (prev.kind == TokenKind::Symbol && prev.text() == ":");
}

// Every declaration's name token, sorted so membership is a binary search. The
// scan needs it because a declaration's own name is call-*shaped* — `Sub s()`
// has `s` followed by `(` at a statement head — and would otherwise read as a
// call to itself, including a recursive Sub's own signature line.
std::vector<SourceRange> declNameRanges(std::vector<Symbol> const &roots) {
  std::vector<SourceRange> ranges;
  std::vector<Symbol const *> stack;
  for (auto const &root : roots) {
    stack.push_back(&root);
  }
  while (!stack.empty()) {
    Symbol const *const sym = stack.back();
    stack.pop_back();
    if (sym->selection.beg != sym->selection.end) {
      ranges.push_back(sym->selection);
    }
    for (auto const &child : sym->children) {
      stack.push_back(&child);
    }
  }
  std::sort(
      ranges.begin(), ranges.end(),
      [](SourceRange const &a, SourceRange const &b) { return a.beg < b.beg; });
  return ranges;
}

bool isDeclName(std::vector<SourceRange> const &declNames, Token const &t) {
  auto const it = std::lower_bound(
      declNames.begin(), declNames.end(), t.beg,
      [](SourceRange const &r, std::uint32_t beg) { return r.beg < beg; });
  return it != declNames.end() && it->beg == t.beg && it->end == t.end;
}

// The callee references in `doc.tokens[body]`, in source order. Nothing is
// resolved here: this is a shape test over the token stream, and the caller's
// resolver decides what each shape names.
std::vector<CalleeRef> callRefsIn(AnalyzedDoc const &doc,
                                  SourceRange const &body,
                                  std::vector<SourceRange> const &declNames) {
  std::vector<CalleeRef> refs;
  std::vector<Token> const &tokens = doc.tokens;
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    Token const &t = tokens[i];
    if (t.beg < body.beg || t.end > body.end) {
      continue;
    }
    // A reserved word is a legal member name (`obj.open(1)`, and the parser
    // records such a member — see the keyword-member hover test), so a member
    // access is scanned for keywords as well as identifiers. The resolver
    // answers only for a name that resolves, so an `if (` or a `then` costs a
    // lookup and nothing else.
    bool const member = i > 0 && isDot(tokens[i - 1]);
    if (!member && t.kind != TokenKind::Identifier) {
      continue;
    }
    CalleeRef::Shape shape;
    if (member) {
      shape = CalleeRef::Shape::Member;
    } else if (i + 1 < tokens.size() && isLParen(tokens[i + 1])) {
      shape = CalleeRef::Shape::Name;
    } else if (isStatementHead(tokens, i)) {
      shape = CalleeRef::Shape::Statement;
    } else {
      continue;
    }
    if (isDeclName(declNames, t)) {
      continue;
    }
    refs.push_back(CalleeRef{t.beg, {t.beg, t.end}, shape});
  }
  return refs;
}

SourceRange wholeDocument(AnalyzedDoc const &doc) {
  // The token stream always ends in the Eof token, whose offset is the
  // content's length (lexer.h).
  return SourceRange{0, doc.tokens.empty() ? 0u : doc.tokens.back().end};
}

} // namespace

CallItem callItemOf(Symbol const &decl, std::string const &file) {
  CallItem item;
  item.file = file;
  item.name = decl.name;
  item.detail = decl.signature;
  item.range = decl.range;
  item.selection = decl.selection;
  item.kind = decl.kind;
  return item;
}

Symbol const *procedureAt(AnalyzedDoc const &doc, std::uint32_t off) {
  // Innermost node containing the offset (a Dim or a nested `if` when the
  // cursor is on one), then out to the procedure around it — so a cursor on a
  // Sub's name, on a Dim inside it, or on a statement in a nested block all
  // answer the same Sub.
  Symbol const *cur = innermostNode(doc.parse, off);
  while (cur != nullptr && !isProcedureLike(cur->kind)) {
    cur = parentOf(doc.parse, cur);
  }
  return cur;
}

std::vector<OutgoingCall> outgoingCalls(AnalyzedDoc const &doc,
                                        std::string const &docPath,
                                        Symbol const *caller,
                                        CalleeResolver const &resolve) {
  std::vector<OutgoingCall> out;
  if (caller == nullptr || !resolve) {
    return out;
  }
  std::vector<SourceRange> const declNames = declNameRanges(doc.parse.roots);
  // Calls are merged per callee, keyed by declaration identity rather than by
  // pointer: a callee outside this file arrives as a snapshot symbol, and the
  // same callee can be reached through two lookups of different shapes.
  struct Entry {
    DeclIdentity id;
    OutgoingCall call;
  };
  std::vector<Entry> entries;
  for (CalleeRef const &ref : callRefsIn(doc, caller->range, declNames)) {
    CrossDecl const decl = resolve(doc, ref);
    if (decl.decl == nullptr || !accepts(ref.shape, decl.decl->kind)) {
      continue;
    }
    DeclIdentity const id = identityOf(decl, docPath);
    auto it = std::find_if(entries.begin(), entries.end(),
                           [id](Entry const &e) { return e.id == id; });
    if (it == entries.end()) {
      // The item is a copy: no symbol pointer escapes the call, so the snapshot
      // pin the resolver handed back is not needed past this line.
      it = entries.insert(
          entries.end(),
          Entry{id, OutgoingCall{callItemOf(*decl.decl, id.path), {}}});
    }
    it->call.sites.push_back(ref.range);
  }
  out.reserve(entries.size());
  for (Entry &entry : entries) {
    out.push_back(std::move(entry.call));
  }
  return out;
}

std::vector<IncomingCall> incomingCallsIn(AnalyzedDoc const &doc,
                                          std::string const &docPath,
                                          DeclIdentity const &target,
                                          CalleeResolver const &resolve) {
  std::vector<IncomingCall> out;
  if (!resolve) {
    return out;
  }
  std::vector<SourceRange> const declNames = declNameRanges(doc.parse.roots);
  for (CalleeRef const &ref : callRefsIn(doc, wholeDocument(doc), declNames)) {
    CrossDecl const decl = resolve(doc, ref);
    if (decl.decl == nullptr || !accepts(ref.shape, decl.decl->kind)) {
      continue;
    }
    if (!(identityOf(decl, docPath) == target)) {
      continue;
    }
    Symbol const *const caller = procedureAt(doc, ref.off);
    if (caller == nullptr) {
      continue; // module-level call: no enclosing declaration to be `from`
    }
    auto it =
        std::find_if(out.begin(), out.end(), [caller](IncomingCall const &c) {
          return c.caller == caller;
        });
    if (it == out.end()) {
      it = out.insert(out.end(), IncomingCall{caller, {}});
    }
    it->sites.push_back(ref.range);
  }
  return out;
}

} // namespace fblang
