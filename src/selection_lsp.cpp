/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "selection_lsp.h"

#include <cstdint>
#include <deque>
#include <string_view>
#include <vector>

#include "LibLsp/lsp/lsRange.h"
#include "LibLsp/lsp/textDocument/selectionRange.h"

#include "symbols.h"
#include "utf16.h"

namespace fblang {
namespace {

// LspCpp links a range's parent through `optional<SelectionRange*>` — a
// *non-owning* pointer — and the response vector owns only the innermost node,
// so every ancestor has to outlive the handler that built it. It does, by
// exactly one write: RemoteEndPoint::sendHandlerResult hands the handler's
// response to sendSessionMessage, which serializes it synchronously, on the
// handler's own thread, under the output mutex, and ResponseOrError(T&&)
// moves the response in (a vector move steals the buffer, so the parents stay
// put). Reflect(Writer&, SelectionRange*) then walks those pointers.
//
// So the ancestors live in a per-thread arena that the next request on that
// thread reuses: one thread, one arena, no lock, and the nodes are consumed by
// that single write before anything can reuse them. Were serialization ever to
// move off this thread, the parents would dangle — session_integration asserts
// the reply carries the *nested* chain, which is what actually proves the
// lifetime rather than assuming it.
thread_local std::deque<SelectionRange> arena;

// A node in the arena. A deque is what keeps it safe: push_back never
// invalidates a reference to an element already in it, and the chain is wired
// by pointer.
SelectionRange *park(lsRange range) {
  arena.push_back(SelectionRange{});
  SelectionRange &node = arena.back();
  node.range = range;
  node.parent = optional<SelectionRange *>();
  return &node;
}

} // namespace

std::vector<SelectionRange>
selectionRanges(std::vector<std::vector<SourceRange>> const &chains,
                std::string_view content) {
  arena.clear(); // the previous request on this thread has been written out
  std::vector<SelectionRange> result;
  result.reserve(chains.size());
  for (std::vector<SourceRange> const &chain : chains) {
    // Wired outermost first, so each node's parent already exists when the node
    // that points at it is parked. An empty chain (an empty document) owes an
    // entry all the same, or the client would map result[i] to the wrong
    // position: a degenerate range at the start stands in for it.
    SelectionRange *innermost =
        chain.empty() ? park(utf16Range(content, 0, 0)) : nullptr;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      SelectionRange *node = park(utf16Range(content, it->beg, it->end));
      if (innermost != nullptr) {
        node->parent = innermost;
      }
      innermost = node;
    }
    result.push_back(*innermost);
  }
  return result;
}

} // namespace fblang
