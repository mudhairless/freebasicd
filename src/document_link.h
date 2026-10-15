/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "resolve.h"

namespace fblang {

// Include navigation (M16): what the server does when the cursor is on an
// `#include` filename literal. LSP-agnostic and in byte offsets like the rest
// of the language layer; the session turns a `DocumentLinkItem` into an
// `lsDocumentLink` and names the URI scheme, the same split code_lens and
// inlay_hints use.
//
// The resource named is the `#include` target: a filename literal that resolves
// to a file on disk — the one thing the server navigates to that is not a
// *symbol*. `textDocument/definition` answers a click on the literal from here,
// and the same literal is offered as a `documentLink` for the clients that draw
// one (not all do: Kate's LSP client has no documentLink support, but it does
// honor definition). Keyword/intrinsic wiki pages are deliberately *not* links:
// hover already carries the page for the word under the cursor, and a
// document-wide link on every keyword would underline the whole buffer to
// repeat it.
struct DocumentLinkItem {
  SourceRange range;  // the filename literal in source (quotes excluded)
  std::string target; // resolved normalized absolute path
};

// The module's only seam on workspace state, exactly as ReferenceCounter is for
// code lens: resolve a filename literal to an absolute path, or nullopt when
// the include resolves nowhere. A link is offered only when this accepts the
// literal, so an unresolved include stays unlinked — its `include-not-found`
// diagnostic is the report — rather than pointing at a file that is not there.
// A null resolver yields no links.
using IncludeResolver =
    std::function<std::optional<std::string>(std::string const &literal)>;

// One link per `#include`/`#include once` literal that resolves, in source
// order. An empty literal — the shape a bare `#include` line parses to — names
// nothing and carries no link.
std::vector<DocumentLinkItem> documentLinks(AnalyzedDoc const &doc,
                                            IncludeResolver const &resolve);

// The `#include` filename literal that `offset` sits on, or nullptr — the
// position half of include navigation, over the same literal ranges the links
// are built from. A `textDocument/definition` on an include literal asks this
// to decide whether the click is on one of them.
//
// The source range is half-open (`[beg, end)`), but the end is accepted too, so
// a cursor just past the last character — on the closing quote — still hits. A
// bare `#include` (or `#include ""`) has a zero-width range and is never a hit,
// so no click can land on a literal that names nothing.
IncludeDirective const *includeLiteralAt(AnalyzedDoc const &doc,
                                         std::uint32_t offset);

} // namespace fblang
