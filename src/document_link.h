/*
 * FreeBASIC Language Server
 * Copyright (C) 2026 Ebben Feagan
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "resolve.h"

namespace fblang {

// Document links (M16): a span an editor draws as a clickable resource. LSP-
// agnostic and in byte offsets like the rest of the language layer; the session
// turns one `DocumentLinkItem` into an `lsDocumentLink` and names the URI
// scheme, the same split code_lens and inlay_hints use.
//
// The resource this server links is the `#include` target: a filename literal
// that resolves to a file on disk. That is the one navigation the server
// otherwise lacks — `textDocument/definition` resolves *symbols*, so a click on
// `#include "lib.bi"` does nothing today — and the link is what opens the
// header. Keyword/intrinsic wiki pages are deliberately *not* links: hover
// already carries the page for the word under the cursor, and a document-wide
// link on every keyword would underline the whole buffer to repeat it.
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

} // namespace fblang
